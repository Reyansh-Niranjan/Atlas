/**
 * @file audio_player.c
 * @brief Streams WAV audio from SD card to MAX98357A I2S DAC with volume control & mute.
 */
#include "audio_player.h"
#include "sdcard.h"

#include <string.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_player";

/* ── Shared Playback & AV-Sync State ─────────────────────────────────────── */
audio_sync_t g_audio_sync = {
    .samples_written    = 0,
    .audio_start_us     = 0,
    .paused_time_us     = 0,
    .audio_running      = false,
    .playing            = true,
    .restart_req_audio  = false,
    .restart_req_video  = false,
    .video_primed       = false,
    .volume_pct         = AUDIO_DEFAULT_VOL,
    .audio_ready        = false,
};

static i2s_chan_handle_t s_tx_chan = NULL;

/* 4KB read buffer (1024 stereo samples = 23.2ms) matching FAT cluster and DMA size */
#define READ_BUF_BYTES   4096
static uint8_t s_read_buf[READ_BUF_BYTES];

/* ── RIFF / WAV Format Parser ────────────────────────────────────────────── */
typedef struct {
    uint32_t data_offset;       /* Byte offset to PCM audio data */
    uint32_t data_size;         /* Length of PCM audio data in bytes */
    uint32_t sample_rate;       /* Sampling frequency in Hz (e.g. 44100) */
    uint16_t channels;          /* Channel count (e.g. 2 for stereo) */
    uint16_t bits_per_sample;   /* Bits per sample (e.g. 16) */
} wav_header_info_t;

/**
 * @brief Parse RIFF header and scan chunks to find the exact 'data' chunk offset.
 * Skips metadata chunks (e.g. 'LIST', 'INFO', 'JUNK') to prevent ASCII header text
 * from being played as high-amplitude noise clicks at track start/restart.
 */
static bool parse_wav_header(FILE *f, wav_header_info_t *info)
{
    if (!f || !info) return false;
    memset(info, 0, sizeof(*info));

    fseek(f, 0, SEEK_SET);

    uint8_t buf[12];
    if (fread(buf, 1, 12, f) != 12) return false;

    /* Verify "RIFF" and "WAVE" signatures */
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Not a valid RIFF/WAVE file");
        return false;
    }

    bool found_fmt = false;
    bool found_data = false;

    while (!found_data) {
        uint8_t chunk_hdr[8];
        if (fread(chunk_hdr, 1, 8, f) != 8) break;

        uint32_t chunk_size = (uint32_t)chunk_hdr[4] |
                             ((uint32_t)chunk_hdr[5] << 8) |
                             ((uint32_t)chunk_hdr[6] << 16) |
                             ((uint32_t)chunk_hdr[7] << 24);

        if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
            uint8_t fmt_buf[16];
            if (fread(fmt_buf, 1, 16, f) != 16) return false;

            info->channels = (uint16_t)fmt_buf[2] | ((uint16_t)fmt_buf[3] << 8);
            info->sample_rate = (uint32_t)fmt_buf[4] | ((uint32_t)fmt_buf[5] << 8) |
                                ((uint32_t)fmt_buf[6] << 16) | ((uint32_t)fmt_buf[7] << 24);
            info->bits_per_sample = (uint16_t)fmt_buf[14] | ((uint16_t)fmt_buf[15] << 8);
            found_fmt = true;

            if (chunk_size > 16) {
                fseek(f, chunk_size - 16, SEEK_CUR);
            }
        } else if (memcmp(chunk_hdr, "data", 4) == 0) {
            info->data_offset = (uint32_t)ftell(f);
            info->data_size   = chunk_size;
            found_data = true;
            break;
        } else {
            /* Skip metadata / info chunks */
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    if (found_fmt && found_data) {
        ESP_LOGI(TAG, "WAV Verified: %lu Hz, %d-ch, %d-bit | Data offset: %lu, size: %lu bytes",
                 (unsigned long)info->sample_rate, (int)info->channels, (int)info->bits_per_sample,
                 (unsigned long)info->data_offset, (unsigned long)info->data_size);
        return true;
    }

    ESP_LOGE(TAG, "WAV missing fmt or data chunk (fmt=%d, data=%d)", found_fmt, found_data);
    return false;
}

/**
 * @brief High-performance digital volume scaler with click-eliminating micro-fades.
 * In IRAM for zero cache-miss latency during I2S DMA feeding.
 */
static void IRAM_ATTR audio_apply_volume_and_fade(int16_t *samples, size_t count, int vol,
                                                  int *fade_in_rem, int *fade_out_rem)
{
    if (vol <= 0) {
        memset(samples, 0, count * sizeof(int16_t));
        return;
    }

    size_t pairs = count / 2;
    for (size_t i = 0; i < pairs; i++) {
        int32_t s0 = samples[i * 2];
        int32_t s1 = samples[i * 2 + 1];

        /* Scale by hardware volume (0..100) */
        s0 = (s0 * vol) / 100;
        s1 = (s1 * vol) / 100;

        /* Startup / Loop Micro Fade-In (256 samples = ~5.8 ms ramp) */
        if (fade_in_rem && *fade_in_rem > 0) {
            int ramp = 256 - *fade_in_rem;
            s0 = (s0 * ramp) >> 8;
            s1 = (s1 * ramp) >> 8;
            (*fade_in_rem)--;
        }

        /* Ending Micro Fade-Out (256 samples = ~5.8 ms ramp) */
        if (fade_out_rem && *fade_out_rem > 0) {
            int ramp = *fade_out_rem;
            s0 = (s0 * ramp) >> 8;
            s1 = (s1 * ramp) >> 8;
            (*fade_out_rem)--;
        }

        samples[i * 2]     = (int16_t)s0;
        samples[i * 2 + 1] = (int16_t)s1;
    }
}

int64_t audio_get_playback_time_us(void)
{
    if (!atomic_load_explicit(&g_audio_sync.audio_running, memory_order_acquire)) {
        return atomic_load_explicit(&g_audio_sync.paused_time_us, memory_order_relaxed);
    }
    if (!atomic_load_explicit(&g_audio_sync.playing, memory_order_relaxed)) {
        return atomic_load_explicit(&g_audio_sync.paused_time_us, memory_order_relaxed);
    }
    int64_t start = atomic_load_explicit(&g_audio_sync.audio_start_us, memory_order_relaxed);
    if (start == 0) {
        return 0;
    }

    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_us = now_us - start;
    if (elapsed_us < 0) elapsed_us = 0;

    /* Clamp against total audio samples fed to I2S DMA to prevent clock runaway during SD card stalls */
    uint64_t written = atomic_load_explicit(&g_audio_sync.samples_written, memory_order_relaxed);
    if (written > 0) {
        int64_t max_audio_us = (int64_t)(written * 1000000ULL / AUDIO_SAMPLE_RATE);
        if (elapsed_us > max_audio_us) {
            elapsed_us = max_audio_us;
        }
    }

    return elapsed_us;
}

void audio_set_sd_mode(bool enable)
{
    gpio_set_level((gpio_num_t)AUDIO_GPIO_SD_MODE, enable ? 1 : 0);
}

esp_err_t audio_init(void)
{
    /* 1. Configure SD_MODE GPIO as output */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << AUDIO_GPIO_SD_MODE),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* Active-HIGH on boot */
    audio_set_sd_mode(true);

    /* 2. Configure I2S Standard Channel */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = AUDIO_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = AUDIO_DMA_BUF_LEN / (AUDIO_BITS / 8) / AUDIO_CHANNELS;
    chan_cfg.auto_clear    = true;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_GPIO_BCLK,
            .ws   = AUDIO_GPIO_WS,
            .dout = AUDIO_GPIO_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    g_audio_sync.audio_ready = true;
    ESP_LOGI(TAG, "I2S initialised: %d Hz 16-bit stereo on DIN:%d BCLK:%d WS:%d SD_MODE:%d",
             AUDIO_SAMPLE_RATE, AUDIO_GPIO_DIN, AUDIO_GPIO_BCLK, AUDIO_GPIO_WS, AUDIO_GPIO_SD_MODE);
    return ESP_OK;
}

void audio_task(void *pvParam)
{
    (void)pvParam;

    /* Ramp-up delay before streaming */
    vTaskDelay(pdMS_TO_TICKS(100));
    audio_set_sd_mode(true);

    bool last_playing = true;

    while (true) {
        ESP_LOGI(TAG, "Opening audio asset: %s", AUDIO_WAV_PATH);
        FILE *wav = sdcard_open(AUDIO_WAV_PATH, "rb");
        if (!wav) {
            ESP_LOGW(TAG, "Not found at %s, trying fallback %s", AUDIO_WAV_PATH, AUDIO_WAV_FALLBACK);
            wav = sdcard_open(AUDIO_WAV_FALLBACK, "rb");
        }
        if (!wav) {
            ESP_LOGE(TAG, "Cannot open audio asset, retrying in 2s...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        /* Dynamically parse WAV structure to identify exact audio data offset and length */
        wav_header_info_t wav_info;
        if (!parse_wav_header(wav, &wav_info)) {
            ESP_LOGW(TAG, "Header parse failed; falling back to standard offset 44");
            wav_info.data_offset = 44;
            wav_info.data_size   = 0xFFFFFFFF;
        }

        fseek(wav, wav_info.data_offset, SEEK_SET);
        uint32_t bytes_remaining = wav_info.data_size;
        int fade_in_samples  = 256; /* 5.8ms micro-fade to eliminate startup pop */
        int fade_out_samples = 0;

        /* Wait for video pipeline to prime frames in PSRAM before starting audio clock */
        while (!atomic_load_explicit(&g_audio_sync.video_primed, memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        while (true) {
            /* 1. Handle Play / Pause State */
            bool cur_playing = atomic_load_explicit(&g_audio_sync.playing, memory_order_relaxed);
            if (cur_playing != last_playing) {
                last_playing = cur_playing;
                if (!cur_playing) {
                    /* Paused: pull SD_MODE LOW immediately to kill speaker hiss */
                    audio_set_sd_mode(false);
                    int64_t start = atomic_load_explicit(&g_audio_sync.audio_start_us, memory_order_relaxed);
                    int64_t cur_time = (start > 0) ? (esp_timer_get_time() - start) : 0;
                    atomic_store_explicit(&g_audio_sync.paused_time_us, cur_time, memory_order_relaxed);
                    atomic_store_explicit(&g_audio_sync.audio_running, false, memory_order_release);
                    ESP_LOGI(TAG, "Audio PAUSED at %lld us -> MAX98357A muted", (long long)cur_time);
                } else {
                    /* Resumed: pull SD_MODE HIGH, wait for amp power-on settle */
                    audio_set_sd_mode(true);
                    vTaskDelay(pdMS_TO_TICKS(25));
                    fade_in_samples = 256; /* micro-fade on unpause */
                    int64_t paused_time = atomic_load_explicit(&g_audio_sync.paused_time_us, memory_order_relaxed);
                    atomic_store_explicit(&g_audio_sync.audio_start_us, esp_timer_get_time() - paused_time, memory_order_relaxed);
                    atomic_store_explicit(&g_audio_sync.audio_running, true, memory_order_release);
                    ESP_LOGI(TAG, "Audio RESUMED from %lld us -> MAX98357A active", (long long)paused_time);
                }
            }

            if (!cur_playing) {
                /* While paused, yield CPU and wait */
                vTaskDelay(pdMS_TO_TICKS(15));
                continue;
            }

            /* 2. Handle Restart / Loop Request */
            if (atomic_load_explicit(&g_audio_sync.restart_req_audio, memory_order_relaxed)) {
                i2s_channel_disable(s_tx_chan);
                fseek(wav, wav_info.data_offset, SEEK_SET);
                bytes_remaining = wav_info.data_size;
                atomic_store_explicit(&g_audio_sync.samples_written, 0, memory_order_relaxed);
                atomic_store_explicit(&g_audio_sync.audio_start_us, 0, memory_order_relaxed);
                atomic_store_explicit(&g_audio_sync.paused_time_us, 0, memory_order_relaxed);
                atomic_store_explicit(&g_audio_sync.audio_running, false, memory_order_release);
                atomic_store_explicit(&g_audio_sync.restart_req_audio, false, memory_order_relaxed);
                i2s_channel_enable(s_tx_chan);
                fade_in_samples  = 256; /* micro-fade on restart */
                fade_out_samples = 0;
                ESP_LOGI(TAG, "Audio restarted from beginning (offset=%lu)", (unsigned long)wav_info.data_offset);
                while (!atomic_load_explicit(&g_audio_sync.video_primed, memory_order_acquire)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }

            /* 3. Read Sector-Aligned PCM Chunk from SD Card (Capped to Exact Audio Data Size) */
            size_t to_read = READ_BUF_BYTES;
            if (bytes_remaining > 0 && to_read > bytes_remaining) {
                to_read = bytes_remaining;
            }

            size_t bytes_read = fread(s_read_buf, 1, to_read, wav);
            if (bytes_read == 0) {
                /* End of WAV audio data reached: trigger synchronized track loop */
                ESP_LOGI(TAG, "WAV EOF reached — requesting synchronized track loop");
                atomic_store_explicit(&g_audio_sync.restart_req_audio, true, memory_order_release);
                atomic_store_explicit(&g_audio_sync.restart_req_video, true, memory_order_release);
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            if (bytes_remaining >= bytes_read) {
                bytes_remaining -= bytes_read;
            } else {
                bytes_remaining = 0;
            }

            /* If within final 256 samples (1024 bytes), activate micro fade-out */
            if (bytes_remaining <= 1024 && fade_out_samples == 0) {
                fade_out_samples = 256;
            }

            /* 4. Apply Digital Volume Attenuation & Click-Eliminating Micro-Fades */
            int ui_vol = atomic_load_explicit(&g_audio_sync.volume_pct, memory_order_relaxed);
            if (ui_vol < 0) ui_vol = 0;
            if (ui_vol > AUDIO_UI_MAX_VOL) ui_vol = AUDIO_UI_MAX_VOL;
            int hw_vol = (ui_vol * AUDIO_HW_MAX_VOL) / AUDIO_UI_MAX_VOL;

            int16_t *pcm = (int16_t *)s_read_buf;
            size_t sample_count = bytes_read / sizeof(int16_t);
            audio_apply_volume_and_fade(pcm, sample_count, hw_vol, &fade_in_samples, &fade_out_samples);

            /* 5. Push to I2S DMA Ring Buffer */
            size_t bytes_written = 0;
            esp_err_t err = i2s_channel_write(s_tx_chan, s_read_buf, bytes_read,
                                              &bytes_written, portMAX_DELAY);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "i2s_channel_write error: %s", esp_err_to_name(err));
            }

            /* 6. Anchor Master Audio Clock on initial buffer transmission */
            if (!atomic_load_explicit(&g_audio_sync.audio_running, memory_order_relaxed)) {
                int64_t t0 = esp_timer_get_time();
                atomic_store_explicit(&g_audio_sync.audio_start_us, t0, memory_order_release);
                atomic_store_explicit(&g_audio_sync.audio_running, true, memory_order_release);
                ESP_LOGI(TAG, "Master audio clock started at %lld us", (long long)t0);
            }

            /* 7. Update Monotonic Atomic AV-Sync Clock (Stereo Sample Pairs) */
            uint64_t new_sample_pairs = bytes_written / (AUDIO_BITS / 8) / AUDIO_CHANNELS;
            atomic_fetch_add_explicit(&g_audio_sync.samples_written,
                                      new_sample_pairs,
                                      memory_order_relaxed);
        }

        fclose(wav);
    }
}
