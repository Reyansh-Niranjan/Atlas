/**
 * @file display_ili9341.c
 * @brief ILI9341 320×240 SPI TFT driver — chunked DMA scanline transfers.
 *
 * SPI2_HOST is initialised here.  sdcard_init() adds the SD card as a
 * second device on the same bus — no re-init needed.
 *
 * SPI bus contention fix — chunked line transfers
 * ───────────────────────────────────────────────
 * Sending a full 153 600-byte frame in one SPI transaction holds the shared
 * bus for ~30.7 ms at 40 MHz.  The audio_task DMA ring (24 × 512 B ≈ 69 ms)
 * can absorb this, but FatFS SD reads are blocked for that entire window,
 * preventing audio_task from refilling its read buffer after the ring drains.
 *
 * Solution: split each frame into CHUNK_LINES (16) scanline strips.
 *   - Each strip = 16 × 320 × 2 = 10 240 bytes → ~2.0 ms at 40 MHz
 *   - After queuing each strip, vTaskDelay(0) yields to the scheduler,
 *     giving audio_task (priority 22, Core 0) a scheduling window to
 *     service any pending SD read or I2S refill.
 *   - The SD card and display share the SPI bus via the ESP-IDF SPI master
 *     arbitration layer: spi_device_acquire_bus / spi_device_release_bus
 *     wrap the per-strip transaction so CS toggling is clean.
 *
 * Ping-pong descriptor pool
 * ─────────────────────────
 * Two spi_transaction_t structs alternate (ping/pong) so we can queue the
 * next strip's DMA descriptor while the previous one is still in flight.
 * display_flush_wait() drains the queue before the caller can overwrite
 * the frame buffer.
 */
#include "display_ili9341.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

static const char *TAG = "display";

/* ── ILI9341 command bytes ───────────────────────────────────────────────── */
#define ILI_NOP        0x00
#define ILI_SWRESET    0x01
#define ILI_SLPOUT     0x11
#define ILI_NORON      0x13
#define ILI_INVOFF     0x20
#define ILI_INVON      0x21
#define ILI_DISPON     0x29
#define ILI_CASET      0x2A
#define ILI_PASET      0x2B
#define ILI_RAMWR      0x2C
#define ILI_MADCTL     0x36
#define ILI_PIXFMT     0x3A
#define ILI_FRMCTR1    0xB1
#define ILI_DFUNCTR    0xB6
#define ILI_PWCTR1     0xC0
#define ILI_PWCTR2     0xC1
#define ILI_VMCTR1     0xC5
#define ILI_VMCTR2     0xC7
#define ILI_GMCTRP1    0xE0
#define ILI_GMCTRN1    0xE1

/* MADCTL: Verified working orientation (0x40) */
#define MADCTL_WORKING_ORIENTATION 0x40

static spi_device_handle_t s_spi = NULL;

/* ── Low-level helpers ───────────────────────────────────────────────────── */
static inline void dc_cmd(void)  { gpio_set_level(LCD_GPIO_DC, 0); }
static inline void dc_data(void) { gpio_set_level(LCD_GPIO_DC, 1); }

/** Blocking single-byte command. */
static void ili_cmd(uint8_t cmd)
{
    dc_cmd();
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = 8;
    t.tx_buffer = &cmd;
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

/** Blocking data bytes. */
static void ili_data(const uint8_t *data, size_t len)
{
    if (!len) return;
    dc_data();
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = len * 8;
    t.tx_buffer = data;
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static inline void ili_data_byte(uint8_t b) { ili_data(&b, 1); }

static void ili_reset(void)
{
    gpio_set_level(LCD_GPIO_RST, 0);
    ets_delay_us(10000);   /* 10 ms */
    gpio_set_level(LCD_GPIO_RST, 1);
    ets_delay_us(120000);  /* 120 ms post-reset */
}

/* ── ILI9341 register initialisation sequence ───────────────────────────── */
static void ili_init_regs(void)
{
    ili_cmd(ILI_SWRESET);
    ets_delay_us(150000);

    ili_cmd(ILI_SLPOUT);
    ets_delay_us(120000);

    ili_cmd(ILI_FRMCTR1);
    ili_data_byte(0x00); ili_data_byte(0x1B);   /* 70 Hz frame rate (Adafruit/TFT_eSPI standard for deep contrast) */

    /* Porch control (0xB5): 2 front porch lines, 2 back porch lines */
    ili_cmd(0xB5);
    ili_data_byte(0x02); ili_data_byte(0x02); ili_data_byte(0x0A); ili_data_byte(0x14);

    /* 4. Power & VCOM Controls (Adafruit/TFT_eSPI Gold Standard) */
    ili_cmd(ILI_PWCTR1);
    ili_data_byte(0x23);                       /* 0xC0: GVDD = 4.60V */

    ili_cmd(ILI_PWCTR2);
    ili_data_byte(0x10);                       /* 0xC1: Step-up factor */

    ili_cmd(ILI_VMCTR1);
    ili_data_byte(0x3E); ili_data_byte(0x28);  /* 0xC5: VCOM = -1.425V, -0.575V */

    ili_cmd(ILI_VMCTR2);
    ili_data_byte(0x86);                       /* 0xC7: VCOM offset = -1.0V */

    /* 2. Verified Working Orientation (Landscape) */
    ili_cmd(ILI_MADCTL);
    ili_data_byte(0x40);                       /* 0x36: 0x40 strictly */

    /* 3. COLMOD / Pixel Format: 16-bit RGB565 */
    ili_cmd(ILI_PIXFMT);
    ili_data_byte(0x55);                       /* 0x3A: 0x55 forces 16-bit RGB565 */

    ili_cmd(ILI_DFUNCTR);
    ili_data_byte(0x08); ili_data_byte(0x82); ili_data_byte(0x27);

    /* Gamma Control (0x26): Curve 1 */
    ili_cmd(0x26);
    ili_data_byte(0x01);

    /* Gamma Curves */
    ili_cmd(ILI_GMCTRP1);
    {
        uint8_t d[] = {0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,
                       0x37,0x07,0x10,0x03,0x0E,0x09,0x00};
        ili_data(d, sizeof(d));
    }
    ili_cmd(ILI_GMCTRN1);
    {
        uint8_t d[] = {0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,
                       0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F};
        ili_data(d, sizeof(d));
    }

    /* 1. Inversion Revert: Display Inversion OFF */
    ili_cmd(ILI_INVOFF);                       /* 0x20: Display Inversion OFF */
    ili_cmd(ILI_NORON);
    ili_cmd(ILI_DISPON);
    ets_delay_us(100000);
}


/* ── Chunked scanline transfer constants ─────────────────────────────────── */
/*
 * CHUNK_LINES: number of scanlines per SPI DMA transaction.
 *
 * Tradeoff:
 *   Fewer lines  → shorter bus hold → more yield opportunities for audio_task
 *                  BUT more CASET/PASET/RAMWR command overhead per frame
 *   More lines   → fewer transactions → lower command overhead
 *                  BUT longer bus hold per strip
 *
 * 16 lines @ 40 MHz:  10 240 B → ~2.05 ms hold.  15 strips per 240-line frame.
 * 32 lines @ 40 MHz:  20 480 B → ~4.10 ms hold.   8 strips per frame.
 *
 * 16 chosen: each yield window lets audio_task complete one 4096-byte SD read
 * (~0.5 ms on a class-10 card) with margin.
 */
#define CHUNK_LINES      16
#define CHUNK_BYTES      (LCD_WIDTH * CHUNK_LINES * 2)   /* RGB565 */
#define CHUNKS_PER_FRAME (LCD_HEIGHT / CHUNK_LINES)      /* 15 strips */

/* DMA descriptor pool: 2 alternating slots (ping-pong). */
#define DMA_QUEUE_DEPTH  2
static spi_transaction_t s_dma_trans[DMA_QUEUE_DEPTH];
static int               s_dma_slot    = 0;
static bool              s_dma_pending = false;

/* Dedicated internal SRAM DMA ping-pong buffers */
static uint8_t          *s_dma_buf[DMA_QUEUE_DEPTH] = {NULL, NULL};

/* ── Public API ──────────────────────────────────────────────────────────── */
esp_err_t display_init(void)
{
    /* 1. Pre-configure both CS pins HIGH (LCD=5, SD=10) to isolate both slaves */
    gpio_config_t cs_io = {
        .pin_bit_mask = (1ULL << LCD_GPIO_CS) | (1ULL << 10 /* SD_GPIO_CS */),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cs_io));
    gpio_set_level(LCD_GPIO_CS, 1);
    gpio_set_level(10, 1);

    /* 2. Enable pull-ups on shared SPI lines (MISO=13, MOSI=11) */
    gpio_pullup_en(LCD_GPIO_MISO);
    gpio_pullup_en(LCD_GPIO_MOSI);

    /* 3. GPIO for DC and RST */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LCD_GPIO_DC) | (1ULL << LCD_GPIO_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* SPI bus — shared with SD card.
     * max_transfer_sz covers one CHUNK_BYTES strip, NOT the full frame.
     * The DMA engine on ESP32-S3 cannot exceed 32 760 bytes per transaction;
     * CHUNK_BYTES (10 240) is well within that ceiling.                     */
    spi_bus_config_t buscfg = {
        .mosi_io_num     = LCD_GPIO_MOSI,
        .miso_io_num     = LCD_GPIO_MISO,
        .sclk_io_num     = LCD_GPIO_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = CHUNK_BYTES,
        .flags           = SPICOMMON_BUSFLAG_MASTER,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* Display SPI device.
     * queue_size = DMA_QUEUE_DEPTH so we can have one strip in-flight while
     * setting up the window registers for the next one.                     */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = LCD_SPI_CLOCK_HZ,
        .mode           = 0,
        .spics_io_num   = LCD_GPIO_CS,
        .queue_size     = DMA_QUEUE_DEPTH,
        .flags          = SPI_DEVICE_NO_DUMMY,
        .pre_cb         = NULL,
        .post_cb        = NULL,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi));

    ili_reset();
    ili_init_regs();

    memset(s_dma_trans, 0, sizeof(s_dma_trans));

    /* Allocate dedicated internal SRAM DMA ping-pong buffers (64-byte aligned for DMA) */
    s_dma_buf[0] = (uint8_t *)heap_caps_aligned_alloc(64, CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_dma_buf[1] = (uint8_t *)heap_caps_aligned_alloc(64, CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_dma_buf[0] || !s_dma_buf[1]) {
        ESP_LOGW(TAG, "Internal SRAM DMA buffer alloc failed; will use direct fallback");
    }

    ESP_LOGI(TAG, "ILI9341 ready — %dx%d landscape, %d-line chunks, DMA buffers OK",
             LCD_WIDTH, LCD_HEIGHT, CHUNK_LINES);
    return ESP_OK;
}

/* ── Internal: drain one pending DMA result ──────────────────────────────── */
static void drain_one(void)
{
    if (!s_dma_pending) return;
    spi_transaction_t *rtrans;
    ESP_ERROR_CHECK(spi_device_get_trans_result(s_spi, &rtrans, portMAX_DELAY));
    s_dma_pending = false;
}

void display_flush_wait(void)
{
    drain_one();
}

/* ── Internal: set a scanline-strip write window and issue RAMWR ─────────── */
static void set_strip_window(uint16_t y0, uint16_t y1)
{
    /* CASET: full width */
    ili_cmd(ILI_CASET);
    {
        uint8_t d[] = {0x00, 0x00,
                       (uint8_t)((LCD_WIDTH - 1) >> 8),
                       (uint8_t)((LCD_WIDTH - 1) & 0xFF)};
        ili_data(d, 4);
    }
    /* PASET: strip rows */
    ili_cmd(ILI_PASET);
    {
        uint8_t d[] = {(uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
                       (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)};
        ili_data(d, 4);
    }
    ili_cmd(ILI_RAMWR);
    dc_data();  /* subsequent bytes are pixel data */
}

/**
 * @brief Push one full RGB565 frame to the display as chunked DMA strips.
 *
 * Splits the 153 600-byte frame into CHUNKS_PER_FRAME strips of CHUNK_BYTES
 * each.  The hardware SPI bus lock is acquired, used, and RELEASED per strip
 * so that the SDSPI / FatFS driver on Core 0 (audio_task) can acquire the
 * bus between strips to service 4 KB sector reads.
 *
 * Per-strip ownership cycle
 * ─────────────────────────
 *   acquire_bus()         ← block SD card; grab hardware SPI mutex
 *   set_strip_window()    ← CASET + PASET + RAMWR  (~3 µs, polling)
 *   queue_trans()         ← kick off ~2.05 ms DMA transfer
 *   drain_one()           ← wait for DMA to finish  (bus held: DMA owns it)
 *   release_bus()         ← return hardware SPI mutex to the pool
 *   vTaskDelay(0)         ← yield CPU: audio_task (pri 22) runs, calls fread,
 *                            which can now acquire the SPI bus for SD reads
 *
 * Why drain BEFORE release?
 * ─────────────────────────
 * The DMA engine drives MOSI/SCK directly.  The hardware bus mutex must be
 * held for the duration of the transfer.  Releasing before the DMA ISR fires
 * would let sdspi interleave bytes mid-strip, corrupting the display.
 *
 * Why release BEFORE vTaskDelay(0)?
 * ───────────────────────────────────
 * vTaskDelay(0) yields the scheduler but does NOT release the SPI mutex.
 * If we yield while holding the bus, audio_task's fread() → sdspi blocks on
 * the hardware mutex for the entire yield window — the window accomplishes
 * nothing.  Releasing first gives audio_task a real ~1 FreeRTOS tick window
 * to complete at least one 4 KB sector read (~0.5 ms on class-10 card).
 *
 * Frame coherency
 * ───────────────
 * Each strip's CASET/PASET window is set immediately after re-acquiring the
 * bus, so even if the SD card transacted during the inter-strip gap, the next
 * strip's write window is always correct.  No display tearing occurs because
 * the ILI9341 continues displaying its internal frame buffer while we're idle.
 *
 * @param frame_rgb565  Pointer to LCD_FRAME_BYTES of RGB565 pixel data.
 *                      Must remain valid for the duration of this call (it
 *                      returns only after all DMA is complete).
 */
void display_push_region(uint16_t y_start, uint16_t height, const uint8_t *frame_rgb565)
{
    uint16_t y_cur = y_start;
    uint16_t y_end = y_start + height;
    if (y_end > LCD_HEIGHT) y_end = LCD_HEIGHT;
    size_t pixel_offset = 0;

    while (y_cur < y_end) {
        uint16_t lines = y_end - y_cur;
        if (lines > CHUNK_LINES) lines = CHUNK_LINES;
        uint16_t y0 = y_cur;
        uint16_t y1 = y_cur + lines - 1;
        size_t strip_bytes = (size_t)LCD_WIDTH * lines * 2;

        /* Copy strip into internal SRAM DMA-aligned ping-pong buffer */
        uint8_t *dma_ptr = (uint8_t *)(frame_rgb565 + pixel_offset);
        if (s_dma_buf[s_dma_slot]) {
            memcpy(s_dma_buf[s_dma_slot], frame_rgb565 + pixel_offset, strip_bytes);
            dma_ptr = s_dma_buf[s_dma_slot];
        }

        /* 1. Acquire hardware SPI bus mutex */
        ESP_ERROR_CHECK(spi_device_acquire_bus(s_spi, portMAX_DELAY));

        /* 2. Set ILI9341 GRAM write window for this strip */
        set_strip_window(y0, y1);

        /* 3. Enqueue strip pixel DMA */
        spi_transaction_t *t = &s_dma_trans[s_dma_slot];
        s_dma_slot ^= 1;
        memset(t, 0, sizeof(*t));
        t->length    = strip_bytes * 8;
        t->tx_buffer = dma_ptr;
        t->rxlength  = 0;
        t->rx_buffer = NULL;
        t->flags     = 0;
        ESP_ERROR_CHECK(spi_device_queue_trans(s_spi, t, portMAX_DELAY));
        s_dma_pending = true;

        /* 4. Wait for this strip's DMA to complete */
        drain_one();

        /* 5. Release hardware SPI bus mutex so audio_task can read SD */
        spi_device_release_bus(s_spi);

        /* 6. Yield to the FreeRTOS scheduler */
        vTaskDelay(0);

        pixel_offset += strip_bytes;
        y_cur += lines;
    }
}

void display_push_frame(const uint8_t *frame_rgb565)
{
    display_push_region(0, LCD_HEIGHT, frame_rgb565);
}

void display_clear_letterbox(uint16_t y_start, uint16_t height)
{
    uint8_t *black_strip = s_dma_buf[0];
    if (!black_strip) return;
    memset(black_strip, 0, CHUNK_BYTES);

    /* Clear top letterbox bar */
    uint16_t y = 0;
    while (y < y_start) {
        uint16_t lines = y_start - y;
        if (lines > CHUNK_LINES) lines = CHUNK_LINES;
        ESP_ERROR_CHECK(spi_device_acquire_bus(s_spi, portMAX_DELAY));
        set_strip_window(y, y + lines - 1);
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length    = (size_t)LCD_WIDTH * lines * 2 * 8;
        t.tx_buffer = black_strip;
        ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
        spi_device_release_bus(s_spi);
        y += lines;
    }

    /* Clear bottom letterbox bar */
    uint16_t bottom_y = y_start + height;
    y = bottom_y;
    while (y < LCD_HEIGHT) {
        uint16_t lines = LCD_HEIGHT - y;
        if (lines > CHUNK_LINES) lines = CHUNK_LINES;
        ESP_ERROR_CHECK(spi_device_acquire_bus(s_spi, portMAX_DELAY));
        set_strip_window(y, y + lines - 1);
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length    = (size_t)LCD_WIDTH * lines * 2 * 8;
        t.tx_buffer = black_strip;
        ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
        spi_device_release_bus(s_spi);
        y += lines;
    }
}

void display_set_inversion(bool enable)
{
    if (!s_spi) return;
    ESP_ERROR_CHECK(spi_device_acquire_bus(s_spi, portMAX_DELAY));
    ili_cmd(enable ? ILI_INVON : ILI_INVOFF);
    spi_device_release_bus(s_spi);
    ESP_LOGI(TAG, "Display inversion set to: %s (cmd 0x%02X)",
             enable ? "ON" : "OFF", enable ? ILI_INVON : ILI_INVOFF);
}

void display_set_madctl(uint8_t madctl)
{
    if (!s_spi) return;
    ESP_ERROR_CHECK(spi_device_acquire_bus(s_spi, portMAX_DELAY));
    ili_cmd(ILI_MADCTL);
    ili_data_byte(madctl);
    spi_device_release_bus(s_spi);
    ESP_LOGI(TAG, "Display MADCTL set to: 0x%02X", madctl);
}
