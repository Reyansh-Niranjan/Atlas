/**
 * @file atlas_audio.c
 * @brief Bi-Directional Audio Driver: MAX98357A TX with Strict Hardware Clamping & INMP441 RX with DC Filter
 */
#include "atlas_audio.h"
#include "atlas_pins.h"

#include <string.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "atlas_audio";

/* ── Hardware Channel Configuration ──────────────────────────────────────── */
#define SPK_I2S_NUM          I2S_NUM_0
#define MIC_I2S_NUM          I2S_NUM_1

#define AUDIO_DMA_BUF_COUNT  8
#define AUDIO_DMA_BUF_LEN    512   /* bytes per DMA descriptor */
#define AUDIO_CHUNK_SAMPLES  256

/* Fixed-Point Q15 Scale Ceiling: 100% SW Volume = 50% HW Volume (16384 / 32768) */
#define HW_VOLUME_MAX_Q15    16384

static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static atomic_int        s_volume_scale_q15 = 16384; /* Default: 100% UI -> 50% HW */
static atomic_bool       s_audio_muted = false;

/* High-Pass Filter State for INMP441 DC-Offset Rejection */
static int32_t s_hp_x_prev = 0;
static int32_t s_hp_y_prev = 0;

/* ── Amplifier Shutdown / Mute Pin Control ───────────────────────────────── */
static void spk_set_sd_mode(bool active)
{
    gpio_set_level(ATLAS_PIN_SPK_SD_MODE, active ? 1 : 0);
}

/* ── Public API Implementation ───────────────────────────────────────────── */

esp_err_t atlas_audio_init(uint32_t sample_rate)
{
    ESP_LOGI(TAG, "Initializing Atlas Audio Driver (TX: MAX98357A, RX: INMP441) @ %lu Hz...", (unsigned long)sample_rate);

    /* 1. Configure SD_MODE Enable Pin for MAX98357A (Active-HIGH) */
    gpio_config_t sd_mode_cfg = {
        .pin_bit_mask = (1ULL << ATLAS_PIN_SPK_SD_MODE),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sd_mode_cfg));
    spk_set_sd_mode(false); /* Start muted until audio is pushed */

    /* 2. Configure I2S TX Channel (Speaker Amplifier) */
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_NUM, I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num  = AUDIO_DMA_BUF_COUNT;
    tx_chan_cfg.dma_frame_num = AUDIO_DMA_BUF_LEN / 4; /* 16-bit stereo frame = 4 bytes */
    tx_chan_cfg.auto_clear    = true;

    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = ATLAS_PIN_I2S_SPK_BCLK,
            .ws   = ATLAS_PIN_I2S_SPK_WS,
            .dout = ATLAS_PIN_I2S_SPK_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &tx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    /* 3. Configure I2S RX Channel (Digital Microphone) */
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_NUM, I2S_ROLE_MASTER);
    rx_chan_cfg.dma_desc_num  = AUDIO_DMA_BUF_COUNT;
    rx_chan_cfg.dma_frame_num = AUDIO_DMA_BUF_LEN / 2; /* 16-bit mono frame = 2 bytes */
    rx_chan_cfg.auto_clear    = true;

    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &s_rx_chan));

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = ATLAS_PIN_I2S_MIC_SCK,
            .ws   = ATLAS_PIN_I2S_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = ATLAS_PIN_I2S_MIC_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &rx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));

    /* Set default volume: 100% SW -> 50% HW capped */
    atlas_audio_set_volume(100);

    ESP_LOGI(TAG, "Audio initialized: SPK(DIN:%d BCLK:%d WS:%d SD:%d) MIC(SD:%d SCK:%d WS:%d)",
             ATLAS_PIN_I2S_SPK_DIN, ATLAS_PIN_I2S_SPK_BCLK, ATLAS_PIN_I2S_SPK_WS, ATLAS_PIN_SPK_SD_MODE,
             ATLAS_PIN_I2S_MIC_SD, ATLAS_PIN_I2S_MIC_SCK, ATLAS_PIN_I2S_MIC_WS);
    return ESP_OK;
}

void atlas_audio_set_volume(uint8_t sw_volume_percent)
{
    if (sw_volume_percent == 0) {
        atomic_store_explicit(&s_volume_scale_q15, 0, memory_order_relaxed);
        atomic_store_explicit(&s_audio_muted, true, memory_order_relaxed);
        spk_set_sd_mode(false); /* Sleep mode eliminates hiss */
        return;
    }

    if (sw_volume_percent > 100) {
        sw_volume_percent = 100;
    }

    /* Fixed-point scale: 100% SW volume maps strictly to 50% physical ceiling (16384 in Q15) */
    int scale_q15 = ((int)sw_volume_percent * HW_VOLUME_MAX_Q15) / 100;
    atomic_store_explicit(&s_volume_scale_q15, scale_q15, memory_order_relaxed);
    atomic_store_explicit(&s_audio_muted, false, memory_order_relaxed);
    spk_set_sd_mode(true);
}

esp_err_t atlas_audio_write(const int16_t *pcm_data, size_t num_samples, size_t *bytes_written, TickType_t timeout)
{
    if (!pcm_data || num_samples == 0 || !s_tx_chan) {
        if (bytes_written) *bytes_written = 0;
        return ESP_ERR_INVALID_ARG;
    }

    bool muted = atomic_load_explicit(&s_audio_muted, memory_order_relaxed);
    int scale_q15 = atomic_load_explicit(&s_volume_scale_q15, memory_order_relaxed);

    /* Chunked scaling buffer to avoid dynamic allocation or excessive stack usage */
    int16_t chunk_buf[AUDIO_CHUNK_SAMPLES];
    size_t total_written = 0;
    size_t samples_left = num_samples;
    const int16_t *src_ptr = pcm_data;

    while (samples_left > 0) {
        size_t to_process = samples_left > AUDIO_CHUNK_SAMPLES ? AUDIO_CHUNK_SAMPLES : samples_left;

        if (muted || scale_q15 == 0) {
            memset(chunk_buf, 0, to_process * sizeof(int16_t));
        } else {
            /* Fixed-point sample attenuation with saturation clamping */
            for (size_t i = 0; i < to_process; i++) {
                int32_t val = ((int32_t)src_ptr[i] * scale_q15) >> 15;
                if (val > 32767)   val = 32767;
                if (val < -32768)  val = -32768;
                chunk_buf[i] = (int16_t)val;
            }
        }

        size_t written_now = 0;
        esp_err_t ret = i2s_channel_write(s_tx_chan, chunk_buf, to_process * sizeof(int16_t),
                                          &written_now, timeout);
        total_written += written_now;
        if (ret != ESP_OK) {
            if (bytes_written) *bytes_written = total_written;
            return ret;
        }

        src_ptr += to_process;
        samples_left -= to_process;
    }

    if (bytes_written) *bytes_written = total_written;
    return ESP_OK;
}

esp_err_t atlas_audio_read(int16_t *pcm_data, size_t num_samples, size_t *bytes_read, TickType_t timeout)
{
    if (!pcm_data || num_samples == 0 || !s_rx_chan) {
        if (bytes_read) *bytes_read = 0;
        return ESP_ERR_INVALID_ARG;
    }

    size_t req_bytes = num_samples * sizeof(int16_t);
    size_t actual_read = 0;
    esp_err_t ret = i2s_channel_read(s_rx_chan, pcm_data, req_bytes, &actual_read, timeout);
    if (ret != ESP_OK) {
        if (bytes_read) *bytes_read = actual_read;
        return ret;
    }

    /* Single-pole IIR high-pass filter: y[n] = x[n] - x[n-1] + alpha * y[n-1] (alpha ≈ 0.995) */
    size_t samples_received = actual_read / sizeof(int16_t);
    for (size_t i = 0; i < samples_received; i++) {
        int32_t x = pcm_data[i];
        int32_t diff = x - s_hp_x_prev;
        s_hp_x_prev = x;

        int32_t y = diff + ((s_hp_y_prev * 32600) >> 15);
        if (y > 32767)  y = 32767;
        if (y < -32768) y = -32768;
        s_hp_y_prev = y;

        pcm_data[i] = (int16_t)y;
    }

    if (bytes_read) *bytes_read = actual_read;
    return ESP_OK;
}
