/**
 * @file atlas_display.c
 * @brief High-Throughput Bare-Metal ILI9341 Driver with Asynchronous DMA Ping-Pong Pipelining
 */
#include "atlas_display.h"
#include "atlas_pins.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

static const char *TAG = "atlas_display";

/* ── ILI9341 Command Set ─────────────────────────────────────────────────── */
#define CMD_SWRESET       0x01
#define CMD_SLPOUT        0x11
#define CMD_NORON         0x13
#define CMD_INVOFF        0x20
#define CMD_INVON         0x21
#define CMD_DISPON        0x29
#define CMD_CASET         0x2A
#define CMD_PASET         0x2B
#define CMD_RAMWR         0x2C
#define CMD_MADCTL        0x36
#define CMD_PIXFMT        0x3A
#define CMD_FRMCTR1       0xB1
#define CMD_DFUNCTR       0xB6
#define CMD_PWCTR1        0xC0
#define CMD_PWCTR2        0xC1
#define CMD_VMCTR1        0xC5
#define CMD_VMCTR2        0xC7
#define CMD_GMCTRP1       0xE0
#define CMD_GMCTRN1       0xE1

/* MADCTL: Verified Landscape Orientation (0x40: MV=0, MX=1, BGR=0) */
#define MADCTL_LANDSCAPE  0x40

/* SPI Clock: 80 MHz for maximum DMA throughput */
#define LCD_SPI_SPEED_HZ  (80 * 1000 * 1000)

/* Internal DMA Buffer Size: 32 scanline chunk (320 * 32 * 2 = 20,480 bytes) */
#define DMA_CHUNK_LINES   32
#define DMA_CHUNK_BYTES   (ATLAS_LCD_WIDTH * DMA_CHUNK_LINES * sizeof(uint16_t))
#define DMA_QUEUE_DEPTH   2

static spi_device_handle_t s_spi_handle = NULL;
static spi_transaction_t   s_dma_trans[DMA_QUEUE_DEPTH];
static uint8_t            *s_dma_buffer[DMA_QUEUE_DEPTH] = {NULL, NULL};
static int                 s_dma_slot = 0;
static int                 s_in_flight_count = 0;
static bool                s_ledc_initialized = false;

/* ── ISR Callback: Set DC line before transmission starts ────────────────── */
static void IRAM_ATTR lcd_spi_pre_transfer_callback(spi_transaction_t *t)
{
    int dc = (int)(intptr_t)t->user;
    gpio_set_level(ATLAS_PIN_LCD_DC, dc);
}

/* ── Blocking Low-Level Transmit Helpers for Register Init ───────────────── */
static void lcd_cmd_byte(uint8_t cmd)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = 8;
    t.tx_buffer = &cmd;
    t.user      = (void *)(intptr_t)0; /* DC = 0 for command */
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi_handle, &t));
}

static void lcd_data_bytes(const uint8_t *data, size_t len)
{
    if (!len) return;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = len * 8;
    t.tx_buffer = data;
    t.user      = (void *)(intptr_t)1; /* DC = 1 for data */
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi_handle, &t));
}

static inline void lcd_data_byte(uint8_t b)
{
    lcd_data_bytes(&b, 1);
}

/* ── Hardware Reset ──────────────────────────────────────────────────────── */
static void lcd_hardware_reset(void)
{
    gpio_set_level(ATLAS_PIN_LCD_RST, 0);
    ets_delay_us(10000); /* 10 ms low */
    gpio_set_level(ATLAS_PIN_LCD_RST, 1);
    ets_delay_us(120000); /* 120 ms post-reset stabilization */
}

/* ── Minimal Optimal ILI9341 Initialization Sequence ─────────────────────── */
static void lcd_init_registers(void)
{
    lcd_cmd_byte(CMD_SWRESET);
    ets_delay_us(120000);

    lcd_cmd_byte(CMD_SLPOUT);
    ets_delay_us(120000);

    /* 70 Hz frame rate */
    lcd_cmd_byte(CMD_FRMCTR1);
    lcd_data_byte(0x00);
    lcd_data_byte(0x1B);

    /* Porch control */
    lcd_cmd_byte(0xB5);
    lcd_data_byte(0x02);
    lcd_data_byte(0x02);
    lcd_data_byte(0x0A);
    lcd_data_byte(0x14);

    /* Power & VCOM */
    lcd_cmd_byte(CMD_PWCTR1);
    lcd_data_byte(0x23);
    lcd_cmd_byte(CMD_PWCTR2);
    lcd_data_byte(0x10);
    lcd_cmd_byte(CMD_VMCTR1);
    lcd_data_byte(0x3E);
    lcd_data_byte(0x28);
    lcd_cmd_byte(CMD_VMCTR2);
    lcd_data_byte(0x86);

    /* Orientation & Pixel Format (RGB565 16-bit) */
    lcd_cmd_byte(CMD_MADCTL);
    lcd_data_byte(MADCTL_LANDSCAPE);

    lcd_cmd_byte(CMD_PIXFMT);
    lcd_data_byte(0x55); /* 16-bit RGB565 */

    lcd_cmd_byte(CMD_DFUNCTR);
    lcd_data_byte(0x08);
    lcd_data_byte(0x82);
    lcd_data_byte(0x27);

    /* Gamma Control (Curve 1) */
    lcd_cmd_byte(0x26);
    lcd_data_byte(0x01);

    lcd_cmd_byte(CMD_GMCTRP1);
    {
        uint8_t d[] = {0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,
                       0x37,0x07,0x10,0x03,0x0E,0x09,0x00};
        lcd_data_bytes(d, sizeof(d));
    }
    lcd_cmd_byte(CMD_GMCTRN1);
    {
        uint8_t d[] = {0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,
                       0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F};
        lcd_data_bytes(d, sizeof(d));
    }

    lcd_cmd_byte(CMD_INVOFF);
    lcd_cmd_byte(CMD_NORON);
    lcd_cmd_byte(CMD_DISPON);
    ets_delay_us(100000);
}

/* ── Window Address Setting ──────────────────────────────────────────────── */
static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    lcd_cmd_byte(CMD_CASET);
    {
        uint8_t d[] = {(uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
                       (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)};
        lcd_data_bytes(d, 4);
    }
    lcd_cmd_byte(CMD_PASET);
    {
        uint8_t d[] = {(uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
                       (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)};
        lcd_data_bytes(d, 4);
    }
    lcd_cmd_byte(CMD_RAMWR);
}

/* ── Backlight LEDC PWM Setup ────────────────────────────────────────────── */
static void lcd_init_backlight(void)
{
    if ((int)ATLAS_PIN_LCD_BCKL < 0) {
        ESP_LOGI(TAG, "Backlight pin is tied to hardware power rail (no PWM dimming).");
        return;
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_8_BIT,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = 5000,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t chan_cfg = {
        .gpio_num       = ATLAS_PIN_LCD_BCKL,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .duty           = 255,
        .hpoint         = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&chan_cfg));
    s_ledc_initialized = true;
}

/* ── Public API Implementation ───────────────────────────────────────────── */

esp_err_t atlas_display_init(void)
{
    ESP_LOGI(TAG, "Initializing ILI9341 bare-metal display driver...");

    /* 1. Configure DC, RST, and CS GPIOs */
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << ATLAS_PIN_LCD_DC) | (1ULL << ATLAS_PIN_LCD_RST) | (1ULL << ATLAS_PIN_LCD_CS),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));
    gpio_set_level(ATLAS_PIN_LCD_CS, 1);
    gpio_set_level(ATLAS_PIN_LCD_DC, 1);
    gpio_set_level(ATLAS_PIN_LCD_RST, 1);

    /* 2. Configure SPI Master Bus (SPI2_HOST / FSPI) */
    spi_bus_config_t buscfg = {
        .mosi_io_num     = ATLAS_PIN_LCD_MOSI,
        .miso_io_num     = ATLAS_PIN_LCD_MISO,
        .sclk_io_num     = ATLAS_PIN_LCD_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = DMA_CHUNK_BYTES,
        .flags           = SPICOMMON_BUSFLAG_MASTER,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* 3. Attach Display Device with hardware CS & pre-callback */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = LCD_SPI_SPEED_HZ,
        .mode           = 0,
        .spics_io_num   = ATLAS_PIN_LCD_CS,
        .queue_size     = DMA_QUEUE_DEPTH,
        .flags          = SPI_DEVICE_NO_DUMMY,
        .pre_cb         = lcd_spi_pre_transfer_callback,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi_handle));

    /* 4. Reset & initialize ILI9341 controller registers */
    lcd_hardware_reset();
    lcd_init_registers();

    /* 5. Initialize optional backlight PWM */
    lcd_init_backlight();

    /* 6. Allocate DMA ping-pong buffers in internal SRAM */
    s_dma_buffer[0] = (uint8_t *)heap_caps_aligned_alloc(64, DMA_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_dma_buffer[1] = (uint8_t *)heap_caps_aligned_alloc(64, DMA_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_dma_buffer[0] || !s_dma_buffer[1]) {
        ESP_LOGE(TAG, "Failed to allocate internal SRAM DMA ping-pong buffers!");
        return ESP_ERR_NO_MEM;
    }

    memset(s_dma_trans, 0, sizeof(s_dma_trans));
    s_in_flight_count = 0;
    s_dma_slot = 0;

    ESP_LOGI(TAG, "ILI9341 initialized: %dx%d @ 80MHz SPI DMA (ping-pong %u KB)",
             ATLAS_LCD_WIDTH, ATLAS_LCD_HEIGHT, (unsigned int)(DMA_CHUNK_BYTES / 1024));
    return ESP_OK;
}

void atlas_display_set_brightness(uint8_t brightness)
{
    if (s_ledc_initialized) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
}

void atlas_display_wait_flush(void)
{
    while (s_in_flight_count > 0) {
        spi_transaction_t *completed = NULL;
        esp_err_t ret = spi_device_get_trans_result(s_spi_handle, &completed, portMAX_DELAY);
        if (ret == ESP_OK) {
            s_in_flight_count--;
        } else {
            ESP_LOGE(TAG, "spi_device_get_trans_result failed: %s", esp_err_to_name(ret));
            break;
        }
    }
}

void atlas_display_flush(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, const uint16_t *buffer)
{
    if (!buffer || x0 >= ATLAS_LCD_WIDTH || y0 >= ATLAS_LCD_HEIGHT || x1 >= ATLAS_LCD_WIDTH || y1 >= ATLAS_LCD_HEIGHT) {
        return;
    }
    if (x0 > x1 || y0 > y1) return;

    /* Ensure all previous transactions have finished before setting new window */
    atlas_display_wait_flush();

    /* Set GRAM target window */
    lcd_set_window(x0, y0, x1, y1);

    uint16_t width  = x1 - x0 + 1;
    uint16_t height = y1 - y0 + 1;
    size_t total_bytes = (size_t)width * height * sizeof(uint16_t);
    size_t bytes_sent  = 0;
    const uint8_t *src = (const uint8_t *)buffer;

    while (bytes_sent < total_bytes) {
        size_t chunk = total_bytes - bytes_sent;
        if (chunk > DMA_CHUNK_BYTES) {
            chunk = DMA_CHUNK_BYTES;
        }

        /* If previous slot is still in-flight in queue, wait for it */
        if (s_in_flight_count >= DMA_QUEUE_DEPTH) {
            spi_transaction_t *completed = NULL;
            ESP_ERROR_CHECK(spi_device_get_trans_result(s_spi_handle, &completed, portMAX_DELAY));
            s_in_flight_count--;
        }

        /* Copy chunk into internal DMA-capable SRAM buffer */
        memcpy(s_dma_buffer[s_dma_slot], src + bytes_sent, chunk);

        spi_transaction_t *t = &s_dma_trans[s_dma_slot];
        memset(t, 0, sizeof(*t));
        t->length    = chunk * 8;
        t->tx_buffer = s_dma_buffer[s_dma_slot];
        t->user      = (void *)(intptr_t)1; /* DC = 1 for pixel data */

        ESP_ERROR_CHECK(spi_device_queue_trans(s_spi_handle, t, portMAX_DELAY));
        s_in_flight_count++;
        s_dma_slot = (s_dma_slot + 1) % DMA_QUEUE_DEPTH;

        bytes_sent += chunk;
    }
}
