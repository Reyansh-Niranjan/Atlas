/**
 * @file gfx.h
 * @brief High-performance embedded 2D graphics primitives & font for ATLAS ILI9341
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_W 320
#define LCD_H 240

/* RGB565 color macro with byte-swap for SPI display */
#define RGB565(r, g, b) (uint16_t)(((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))
#define SWAP16(c)       __builtin_bswap16((uint16_t)(c))
#define COLOR(r, g, b)  SWAP16(RGB565(r, g, b))

/* Pre-defined swapped colors */
#define C_BLACK        0x0000
#define C_WHITE        0xFFFF
#define C_DARK_BG      COLOR(15, 20, 28)
#define C_PANEL_BG     COLOR(25, 32, 44)
#define C_BORDER       COLOR(50, 65, 85)
#define C_TEXT_DIM     COLOR(120, 140, 160)
#define C_RED          COLOR(255, 60, 60)
#define C_GREEN        COLOR(40, 230, 90)
#define C_BLUE         COLOR(40, 140, 255)
#define C_CYAN         COLOR(0, 230, 255)
#define C_MAGENTA      COLOR(255, 60, 220)
#define C_YELLOW       COLOR(255, 220, 0)
#define C_ORANGE       COLOR(255, 140, 0)
#define C_GREY         COLOR(70, 80, 95)

void gfx_clear(uint16_t *fb, uint16_t color);
void gfx_draw_pixel(uint16_t *fb, int x, int y, uint16_t color);
void gfx_fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color);
void gfx_draw_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color);
void gfx_draw_line(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t color);
void gfx_draw_char(uint16_t *fb, int x, int y, char c, uint16_t fg, uint16_t bg, int scale);
void gfx_draw_str(uint16_t *fb, int x, int y, const char *str, uint16_t fg, uint16_t bg, int scale);

#ifdef __cplusplus
}
#endif
