/**
 * @file atlas_gfx.h
 * @brief Lightweight Embedded 2D Graphics & Font Engine for Atlas UI
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "atlas_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RGB565 Byte-swapped color macro for ILI9341 SPI DMA */
#define RGB565_SWAP(r, g, b)  ((uint16_t)(((((uint16_t)(r) & 0xF8) << 8) | (((uint16_t)(g) & 0xFC) << 3) | (((uint16_t)(b) & 0xF8) >> 3))))
#define SWAP16(c)             __builtin_bswap16((uint16_t)(c))
#define ATLAS_COLOR(r, g, b)  SWAP16(RGB565_SWAP(r, g, b))

/* Palette */
#define COLOR_BLACK           0x0000
#define COLOR_WHITE           0xFFFF
#define COLOR_BG_DARK         ATLAS_COLOR(15, 18, 26)
#define COLOR_PANEL_BG        ATLAS_COLOR(24, 30, 44)
#define COLOR_PANEL_SEL       ATLAS_COLOR(36, 46, 68)
#define COLOR_BORDER_NORM     ATLAS_COLOR(48, 62, 85)
#define COLOR_BORDER_SEL      ATLAS_COLOR(0, 210, 255)
#define COLOR_TEXT_MAIN       ATLAS_COLOR(235, 240, 250)
#define COLOR_TEXT_MUTED      ATLAS_COLOR(125, 140, 165)
#define COLOR_CYAN            ATLAS_COLOR(0, 210, 255)
#define COLOR_GREEN           ATLAS_COLOR(45, 220, 95)
#define COLOR_YELLOW          ATLAS_COLOR(255, 205, 10)
#define COLOR_RED             ATLAS_COLOR(255, 65, 65)
#define COLOR_ORANGE          ATLAS_COLOR(255, 130, 20)
#define COLOR_PURPLE          ATLAS_COLOR(180, 80, 255)

void atlas_gfx_clear(uint16_t *fb, uint16_t color);
void atlas_gfx_fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color);
void atlas_gfx_draw_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color);
void atlas_gfx_draw_char(uint16_t *fb, int x, int y, char c, uint16_t fg, uint16_t bg, int scale);
void atlas_gfx_draw_string(uint16_t *fb, int x, int y, const char *str, uint16_t fg, uint16_t bg, int scale);
void atlas_gfx_draw_string_centered(uint16_t *fb, int y, const char *str, uint16_t fg, uint16_t bg, int scale);
void atlas_gfx_draw_card(uint16_t *fb, int x, int y, int w, int h, const char *title, const char *tagline, bool selected);

#ifdef __cplusplus
}
#endif
