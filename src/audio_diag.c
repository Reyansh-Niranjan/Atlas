/**
 * @file audio_diag.c
 * @brief I2S Audio TX & RX Drivers for ATLAS Diagnostic Suite
 */
#include "audio_diag.h"

#include <math.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_diag";

/* Pin definitions */
#define PIN_MAX98357A_DIN     21
#define PIN_MAX98357A_BCLK    17
#define PIN_MAX98357A_WS      18
#define PIN_MAX98357A_SD_MODE 47
#define PIN_MAX98357A_GAIN    2

#define PIN_INMP441_SD        16
#define PIN_INMP441_SCK       14
#define PIN_INMP441_WS        15

static i2s_chan_handle_t s_tx_chan = NULL; // I2S_NUM_0
static i2s_chan_handle_t s_rx_chan = NULL; // I2S_NUM_1

static volatile _Atomic(int) s_audio_mode = AUDIO_MODE_SILENCE;
static volatile _Atomic(uint32_t) s_mic_rms = 0;
static volatile _Atomic(int32_t)  s_mic_peak = 0;
static volatile _Atomic(uint32_t) s_mic_pct = 0;

esp_err_t audio_diag_init(void)
{
    /* 1. Configure MAX98357A Control Pins: SD_MODE (GPIO 47) & GAIN (GPIO 2) */
    gpio_config_t ctl_io = {
        .pin_bit_mask = (1ULL << PIN_MAX98357A_SD_MODE) | (1ULL << PIN_MAX98357A_GAIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&ctl_io));
    gpio_set_level(PIN_MAX98357A_GAIN, 0);    /* Output LOW for 12dB boost */
    gpio_set_level(PIN_MAX98357A_SD_MODE, 1); /* Active HIGH output to unmute */

    /* 2. Configure MAX98357A I2S TX (I2S_NUM_0) */
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num  = 8;
    tx_chan_cfg.dma_frame_num = AUDIO_SAMPLES_PER_BLOCK;
    tx_chan_cfg.auto_clear    = true;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MAX98357A_BCLK,
            .ws   = PIN_MAX98357A_WS,
            .dout = PIN_MAX98357A_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &tx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    /* 3. Configure INMP441 I2S RX (I2S_NUM_1)
     * NOTE: INMP441 hardware requires 64 BCLK pulses per WS audio frame.
     * We configure STEREO 32-bit slot mode so the master clocks 64 cycles.
     * With L/R tied to GND, data is received on the LEFT slot.
     */
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    rx_chan_cfg.dma_desc_num  = 8;
    rx_chan_cfg.dma_frame_num = AUDIO_SAMPLES_PER_BLOCK;
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &s_rx_chan));

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_INMP441_SCK,
            .ws   = PIN_INMP441_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_INMP441_SD,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &rx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));

    ESP_LOGI(TAG, "Audio Diag Initialized: TX on I2S_0 (16-bit stereo), RX on I2S_1 (32-bit stereo framing)");
    return ESP_OK;
}

void audio_diag_set_mode(audio_mode_t mode)
{
    atomic_store_explicit(&s_audio_mode, mode, memory_order_relaxed);
}

audio_mode_t audio_diag_get_mode(void)
{
    return (audio_mode_t)atomic_load_explicit(&s_audio_mode, memory_order_relaxed);
}

mic_metrics_t audio_diag_get_metrics(void)
{
    mic_metrics_t m;
    m.rms = atomic_load_explicit(&s_mic_rms, memory_order_relaxed);
    m.peak = atomic_load_explicit(&s_mic_peak, memory_order_relaxed);
    m.peak_pct = atomic_load_explicit(&s_mic_pct, memory_order_relaxed);
    return m;
}

void audio_diag_task(void *pvParam)
{
    (void)pvParam;
    /* Static buffers to prevent task stack pressure */
    static int32_t s_rx_raw[AUDIO_SAMPLES_PER_BLOCK * 2];
    static int16_t s_tx_buf[AUDIO_SAMPLES_PER_BLOCK * 2]; // Stereo (L, R)
    static int16_t s_processed_pcm[AUDIO_SAMPLES_PER_BLOCK];
    float phase = 0.0f;
    float dc_filter = 0.0f;
    float peak_decay = 0.0f;

    while (true) {
        size_t bytes_read = 0;
        esp_err_t rx_err = i2s_channel_read(s_rx_chan, s_rx_raw, sizeof(s_rx_raw),
                                            &bytes_read, portMAX_DELAY);
        if (rx_err != ESP_OK || bytes_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        /* Each stereo frame contains 2x 32-bit words (Left and Right) */
        size_t frame_count = bytes_read / (2 * sizeof(int32_t));
        int64_t sum_sq = 0;
        int32_t block_peak = 0;

        for (size_t i = 0; i < frame_count; i++) {
            /* Left channel contains INMP441 audio (L/R tied to GND)
             * 24-bit data is MSB-aligned in 32-bit slot (bits 31..8).
             * Shift right by 16 to extract clean 16-bit signed PCM.
             */
            int32_t raw_left = s_rx_raw[2 * i];
            int16_t raw_pcm = (int16_t)(raw_left >> 16);

            /* Single-pole IIR high-pass filter to remove MEMS DC bias (~20Hz cutoff) */
            dc_filter += 0.015f * ((float)raw_pcm - dc_filter);
            int16_t ac_pcm = (int16_t)((float)raw_pcm - dc_filter);
            s_processed_pcm[i] = ac_pcm;

            int32_t abs_val = (ac_pcm < 0) ? -ac_pcm : ac_pcm;
            if (abs_val > block_peak) block_peak = abs_val;
            sum_sq += (int64_t)ac_pcm * ac_pcm;
        }

        uint32_t block_rms = (frame_count > 0) ? (uint32_t)sqrtf((float)(sum_sq / frame_count)) : 0;
        atomic_store_explicit(&s_mic_rms, block_rms, memory_order_relaxed);
        atomic_store_explicit(&s_mic_peak, block_peak, memory_order_relaxed);

        /* Smooth VU meter ballistics: instant attack, smooth decay */
        if ((float)block_peak > peak_decay) {
            peak_decay = (float)block_peak;
        } else {
            peak_decay *= 0.85f; // Exponential decay
        }

        /* Scale peak: 18000 counts (~-5 dBFS) maps to 100% full scale */
        uint32_t pct = (uint32_t)((peak_decay * 100.0f) / 18000.0f);
        if (pct > 100) pct = 100;
        atomic_store_explicit(&s_mic_pct, pct, memory_order_relaxed);

        /* Handle TX Output */
        audio_mode_t current_mode = audio_diag_get_mode();
        if (current_mode == AUDIO_MODE_TONE_440 || current_mode == AUDIO_MODE_TONE_880) {
            float freq = (current_mode == AUDIO_MODE_TONE_440) ? 440.0f : 880.0f;
            float phase_inc = (2.0f * (float)M_PI * freq) / (float)AUDIO_SAMPLE_RATE;
            for (size_t i = 0; i < frame_count; i++) {
                int16_t val = (int16_t)(sinf(phase) * 10000.0f);
                phase += phase_inc;
                if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
                s_tx_buf[2 * i]     = val;
                s_tx_buf[2 * i + 1] = val;
            }
        } else if (current_mode == AUDIO_MODE_LOOPBACK) {
            /* Live mic loopback using DC-filtered audio with clean 4x gain */
            for (size_t i = 0; i < frame_count; i++) {
                int32_t amp = (int32_t)s_processed_pcm[i] * 4;
                if (amp > 32767)  amp = 32767;
                if (amp < -32768) amp = -32768;
                s_tx_buf[2 * i]     = (int16_t)amp;
                s_tx_buf[2 * i + 1] = (int16_t)amp;
            }
        } else {
            /* Silence */
            memset(s_tx_buf, 0, frame_count * 2 * sizeof(int16_t));
        }

        size_t bytes_written = 0;
        i2s_channel_write(s_tx_chan, s_tx_buf, frame_count * 2 * sizeof(int16_t),
                          &bytes_written, portMAX_DELAY);
    }
}
