/**
 * @file atlas_audio.h
 * @brief Bi-Directional Audio Driver for Atlas: MAX98357A I2S Speaker & INMP441 I2S Digital Mic
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize both I2S TX (Speaker) and I2S RX (Microphone) channels at target sample rate.
 * Configures MAX98357A shutdown GPIO and INMP441 digital audio pipeline.
 *
 * @param sample_rate Audio sampling rate in Hz (e.g. 16000, 44100, 48000).
 * @return ESP_OK on success.
 */
esp_err_t atlas_audio_init(uint32_t sample_rate);

/**
 * @brief Set master playback volume with strict hardware acoustic clamping.
 * User software volume (0..100) is hard-capped to a physical maximum of 50% [0.0, 0.50]
 * to prevent voice coil overheating and clipping distortion. Volume 0 pulls SD_MODE low.
 *
 * @param sw_volume_percent User volume percentage (0 = Mute/Sleep, 100 = 50% Hardware Max).
 */
void atlas_audio_set_volume(uint8_t sw_volume_percent);

/**
 * @brief Write 16-bit PCM samples to the I2S speaker DMA ring buffer.
 * Performs real-time fixed-point integer volume scaling and hardware saturation protection.
 *
 * @param pcm_data      Pointer to input 16-bit PCM audio samples.
 * @param num_samples   Number of samples to transmit.
 * @param bytes_written Pointer to store total bytes written to I2S DMA.
 * @param timeout       FreeRTOS block timeout.
 * @return ESP_OK on success.
 */
esp_err_t atlas_audio_write(const int16_t *pcm_data, size_t num_samples, size_t *bytes_written, TickType_t timeout);

/**
 * @brief Read 16-bit PCM samples from the INMP441 I2S digital microphone.
 * Applies real-time fixed-point IIR high-pass filtering to remove MEMS DC-offset bias.
 *
 * @param pcm_data    Pointer to output buffer for captured 16-bit PCM audio samples.
 * @param num_samples Maximum number of samples to receive.
 * @param bytes_read  Pointer to store total bytes read from I2S DMA.
 * @param timeout     FreeRTOS block timeout.
 * @return ESP_OK on success.
 */
esp_err_t atlas_audio_read(int16_t *pcm_data, size_t num_samples, size_t *bytes_read, TickType_t timeout);

#ifdef __cplusplus
}
#endif
