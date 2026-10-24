/**
 * @file atlas_display.h
 * @brief Bare-Metal High-Performance ILI9341 SPI Display Driver for Atlas
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ATLAS_LCD_WIDTH    320
#define ATLAS_LCD_HEIGHT   240
#define ATLAS_LCD_PIXELS   (ATLAS_LCD_WIDTH * ATLAS_LCD_HEIGHT)
#define ATLAS_LCD_BYTES    (ATLAS_LCD_PIXELS * sizeof(uint16_t))

/**
 * @brief Initialize SPI host, GPIOs, and ILI9341 controller registers.
 * Allocates DMA-capable internal SRAM ping-pong buffers and configures asynchronous pipeline.
 *
 * @return ESP_OK on success.
 */
esp_err_t atlas_display_init(void);

/**
 * @brief Set display backlight brightness using hardware LEDC PWM (0..255).
 * If backlight is hardwired to a power rail (BCKL == -1), this safely no-ops.
 *
 * @param brightness 0 (off/black) to 255 (maximum brightness).
 */
void atlas_display_set_brightness(uint8_t brightness);

/**
 * @brief Asynchronously flush a rectangular pixel block to the display via SPI DMA.
 * Does not block CPU; queues transaction to SPI DMA engine. Call atlas_display_wait_flush()
 * before modifying or reusing the buffer.
 *
 * @param x0     Left X coordinate (0..319)
 * @param y0     Top Y coordinate (0..239)
 * @param x1     Right X coordinate (0..319, inclusive)
 * @param y1     Bottom Y coordinate (0..239, inclusive)
 * @param buffer Pointer to RGB565 pixel data ((x1 - x0 + 1) * (y1 - y0 + 1) * 2 bytes)
 */
void atlas_display_flush(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, const uint16_t *buffer);

/**
 * @brief Block until all pending asynchronous DMA flush transactions complete.
 */
void atlas_display_wait_flush(void);

#ifdef __cplusplus
}
#endif
