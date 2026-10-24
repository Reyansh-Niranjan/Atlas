#include "fast_jpeg.h"
#include "display_ili9341.h"
#include "esp_log.h"
#include <string.h>

#include "jpegdec/JPEGDEC.h"
#include "jpegdec/jpeg.inl"

static const char *TAG = "fast_jpeg";
static JPEGIMAGE s_jpeg;

/**
 * @brief Pixel-accurate MCU drawing callback.
 * Renders SIMD-converted pixel blocks from 16-byte aligned internal SRAM
 * directly into the target framebuffer at exact (X, Y) coordinates.
 * Eliminates PSRAM unaligned vector store jitter and coordinate creep.
 */
static int jpeg_draw_cb(JPEGDRAW *pDraw)
{
    uint16_t *dst_fb = (uint16_t *)pDraw->pUser;
    const uint16_t *src = pDraw->pPixels;
    int w = pDraw->iWidthUsed;
    int h = pDraw->iHeight;
    int x = pDraw->x;
    int y = pDraw->y;
    int stride = pDraw->iWidth;

    if (!dst_fb || x >= LCD_WIDTH || y >= LCD_HEIGHT) return 1;
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;

    for (int r = 0; r < h; r++) {
        memcpy(&dst_fb[(y + r) * LCD_WIDTH + x], &src[r * stride], w * sizeof(uint16_t));
    }
    return 1;
}

bool fast_jpeg_init(void)
{
    ESP_LOGI(TAG, "Hardware/SIMD accelerated JPEG initialized (C engine + draw callback)");
    return true;
}

bool fast_jpeg_decode(const uint8_t *jpeg_data, size_t jpeg_len,
                      uint16_t *out_fb, int *out_w, int *out_h)
{
    if (!jpeg_data || jpeg_len == 0 || !out_fb) {
        return false;
    }

    if (!JPEG_openRAM(&s_jpeg, (uint8_t *)jpeg_data, (int)jpeg_len, jpeg_draw_cb)) {
        ESP_LOGE(TAG, "JPEG_openRAM failed (err=%d)", JPEG_getLastError(&s_jpeg));
        return false;
    }

    if (out_w) *out_w = JPEG_getWidth(&s_jpeg);
    if (out_h) *out_h = JPEG_getHeight(&s_jpeg);

    s_jpeg.pUser = out_fb;
    s_jpeg.ucPixelType = RGB565_BIG_ENDIAN;

    int rc = JPEG_decode(&s_jpeg, 0, 0, 0);
    JPEG_close(&s_jpeg);

    if (rc != 1) {
        ESP_LOGE(TAG, "JPEG_decode failed (err=%d)", JPEG_getLastError(&s_jpeg));
        return false;
    }

    return true;
}
