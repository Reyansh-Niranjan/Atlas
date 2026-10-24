#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the high-speed SIMD JPEG decoder.
 * @return true on success.
 */
bool fast_jpeg_init(void);

/**
 * @brief Decompress a JPEG buffer directly into a 16-bit RGB565 (big-endian) framebuffer.
 *
 * @param jpeg_data Pointer to raw JPEG file/frame in memory.
 * @param jpeg_len  Size in bytes of raw JPEG data.
 * @param out_fb    Pointer to output 16-bit RGB565 framebuffer (LCD_WIDTH * height pixels).
 * @param out_w     Pointer to receive decoded image width.
 * @param out_h     Pointer to receive decoded image height.
 * @return true on success, false on decode error.
 */
bool fast_jpeg_decode(const uint8_t *jpeg_data, size_t jpeg_len,
                      uint16_t *out_fb, int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif
