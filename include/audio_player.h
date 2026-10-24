/**
 * @file audio_player.h
 * @brief I2S audio streaming from SD card via MAX98357A for ATLAS AV Player.
 *
 * Hardware:
 *   DIN  = GPIO 21 | BCLK = GPIO 17 | LRCLK/WS = GPIO 18
 *   SD_MODE = GPIO 47  (Active-High output; pull LOW when paused to eliminate hiss)
 *
 * Concurrency & AV-Sync:
 *   g_audio_sync.samples_written is an atomic 64-bit monotonic sample counter
 *   incremented by audio_task on Core 0 and read tear-free by video_task on Core 1.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Hardware Pins ────────────────────────────────────────────────────────── */
#define AUDIO_GPIO_DIN      21
#define AUDIO_GPIO_BCLK     17
#define AUDIO_GPIO_WS       18
#define AUDIO_GPIO_SD_MODE  47

/* ── Asset Path ──────────────────────────────────────────────────────────── */
#define AUDIO_WAV_PATH      "/sdcard/assets/DraculaSong/InterceptorEdit/InterceptorEdit.wav"
#define AUDIO_WAV_FALLBACK  "/sdcard/assets/DraculaSong/InterceptorEdit.wav"

/* ── I2S / DMA Settings ──────────────────────────────────────────────────── */
#define AUDIO_I2S_NUM       I2S_NUM_0
#define AUDIO_DMA_BUF_COUNT 8           /* 8 descriptors × 512 B = 4096 B ≈ 23.2 ms */
#define AUDIO_DMA_BUF_LEN   512         /* bytes per descriptor                      */
#define AUDIO_SAMPLE_RATE   44100
#define AUDIO_BITS          16
#define AUDIO_CHANNELS      2

/* ── Volume Limits (UI scale: 0%..100% mapped to HW ceiling: 0%..50%) ───── */
#define AUDIO_DEFAULT_VOL   100         /* 100% startup UI volume (mapped to 50% HW) */
#define AUDIO_UI_MAX_VOL    100         /* 100% UI maximum scale                     */
#define AUDIO_HW_MAX_VOL    50          /* 50% physical hardware acoustic ceiling    */
#define AUDIO_VOL_STEP      1           /* 1% UI step per button press (0%..100%)    */

/**
 * @brief Shared AV-sync and playback control state.
 */
typedef struct {
    atomic_uint_least64_t samples_written;    /**< Tear-free 64-bit stereo sample counter  */
    atomic_int_least64_t  audio_start_us;     /**< esp_timer timestamp when playback started */
    atomic_int_least64_t  paused_time_us;     /**< Accumulated playback time when paused   */
    atomic_bool           audio_running;      /**< true when audio clock is actively ticking */
    atomic_bool           playing;            /**< true: running, false: paused            */
    atomic_bool           restart_req_audio;  /**< Request audio task to rewind to start   */
    atomic_bool           restart_req_video;  /**< Request video task to rewind to start   */
    atomic_bool           video_primed;       /**< true once video queue has primed frames */
    atomic_int            volume_pct;         /**< Current digital volume (0..AUDIO_UI_MAX_VOL)*/
    volatile bool         audio_ready;        /**< true once I2S driver is up              */
} audio_sync_t;

extern audio_sync_t g_audio_sync;

/**
 * @brief Initialise I2S TX channel and MAX98357A SD_MODE control pin.
 */
esp_err_t audio_init(void);

/**
 * @brief Get the current continuous master audio playback time in microseconds.
 * @return Microseconds elapsed since audio playback started (tear-free, sub-millisecond accurate).
 */
int64_t audio_get_playback_time_us(void);

/**
 * @brief Turn amplifier on/off via SD_MODE pin.
 * @param enable true = 5V amp active, false = shutdown (zero hiss)
 */
void audio_set_sd_mode(bool enable);

/**
 * @brief Core 0 FreeRTOS audio task body.
 */
void audio_task(void *pvParam);

#ifdef __cplusplus
}
#endif
