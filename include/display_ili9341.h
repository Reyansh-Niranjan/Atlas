/**
 * @file display_ili9341.h
 * @brief ILI9341 SPI TFT driver – init + chunked DMA scanline transfers.
 *
 * Shared SPI bus (MOSI=11, SCK=12, MISO=13) with the SD card.
 * Display CS=5, DC=7, RESET=6.  SD card CS=10 (managed by sdcard.c).
 *
 * SPI bus contention model
 * ────────────────────────
 * display_push_frame() sends one frame as CHUNK_LINES (16) scanline strips.
 * Between each strip it calls vTaskDelay(0), yielding to audio_task so SD
 * reads and I2S DMA refills can happen during the inter-strip gaps.
 *
 * spi_device_acquire_bus() / spi_device_release_bus() wrap the whole frame
 * push to prevent the SD card sdspi driver from inserting transactions
 * mid-frame while still releasing the bus cleanly at frame boundaries.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Pin definitions ─────────────────────────────────────────────────────── */
#define LCD_GPIO_MOSI   11
#define LCD_GPIO_SCK    12
#define LCD_GPIO_MISO   13
#define LCD_GPIO_CS      5
#define LCD_GPIO_DC      7
#define LCD_GPIO_RST     6

/* ── Display geometry ────────────────────────────────────────────────────── */
#define LCD_WIDTH   320
#define LCD_HEIGHT  240

/* ── SPI clock ───────────────────────────────────────────────────────────── */
#define LCD_SPI_CLOCK_HZ   (80 * 1000 * 1000)  /* 80 MHz — silicon maximum for fast DMA */

/* ── Frame buffer size (RGB565, 2 bytes/pixel) ───────────────────────────── */
#define LCD_FRAME_BYTES  (LCD_WIDTH * LCD_HEIGHT * 2)

/**
 * @brief Initialise the shared SPI bus and the ILI9341 controller.
 *
 * Also initialises the SPI bus host (SPI2_HOST).  Call once before
 * sdcard_init so sdcard can add itself as a second device on the same host.
 *
 * @return ESP_OK on success.
 */
esp_err_t display_init(void);

/**
 * @brief Push one full RGB565 frame to the display via chunked DMA strips.
 *
 * Splits the frame into CHUNK_LINES-high strips and yields between each one
 * (vTaskDelay(0)) so audio_task can service SD reads mid-frame.
 *
 * Acquires the SPI bus (blocking until SD card releases it) at the start of
 * the frame and releases it at the end.  Returns only after all DMA is done;
 * the caller may immediately reuse the buffer.
 *
 * @param frame_rgb565  Pointer to LCD_FRAME_BYTES of RGB565 pixel data.
 */
void display_push_frame(const uint8_t *frame_rgb565);

/**
 * @brief Push an active vertical region of RGB565 scanlines to the display via DMA.
 * For 16:9 widescreen (320x180 on 320x240 screen), y_start = 30, height = 180.
 * Only transmits the active video lines, saving 25% SPI bus bandwidth and DMA time.
 *
 * @param y_start     First scanline coordinate (0..239)
 * @param height      Number of scanlines to push (1..240)
 * @param pixels      Pointer to height * LCD_WIDTH * 2 bytes of RGB565 pixel data
 */
void display_push_region(uint16_t y_start, uint16_t height, const uint8_t *pixels);

/**
 * @brief Clear the top and bottom letterbox bars to black in ILI9341 GRAM.
 * Call once when playing letterboxed video (height < 240) so static bars stay black.
 */
void display_clear_letterbox(uint16_t y_start, uint16_t height);

/**
 * @brief Set display color inversion.
 * @param enable true = INVON (0x21, eliminates washed-out whitish haze), false = INVOFF (0x20)
 */
void display_set_inversion(bool enable);

/**
 * @brief Set MADCTL memory access control register (0x36) for rotation and color order.
 * @param madctl Register value (e.g. 0x28 for Landscape MV=1 BGR=1, 0xE8 for Inverted Landscape)
 */
void display_set_madctl(uint8_t madctl);

/**
 * @brief Wait for any in-flight DMA transaction to complete.
 *
 * Called internally by display_push_frame() between strips.
 * Also safe to call from video_task before freeing a frame buffer.
 */
void display_flush_wait(void);

#ifdef __cplusplus
}
#endif
