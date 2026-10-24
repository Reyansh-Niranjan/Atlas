/**
 * @file main.c
 * @brief ATLAS Handheld Terminal - Dedicated Tame Impala "Dracula" AV Player
 *
 * Architecture:
 *   Core 0: audio_task (pri 22)         -> 44.1kHz 16-bit stereo PCM streaming to MAX98357A I2S
 *   Core 0: video_decode_task (pri 5)   -> Pre-fetches MJPEG from SD & decodes directly into PSRAM Ring-Buffer
 *   Core 1: video_display_task (pri 6)  -> Pulls pre-decoded frames from PSRAM queue & pushes via 80MHz SPI DMA
 *   Core 1: control_task (pri 4)        -> 20ms debounce button scanner (SW1..SW4) with 1% volume stepping
 *
 * Hardware Map:
 *   ILI9341 SPI TFT : CS=5, DC=7, RST=6, MOSI=11, SCK=12, MISO=13 (80MHz Overclock)
 *   MicroSD (SDSPI) : CS=10, MOSI=11, SCK=12, MISO=13
 *   MAX98357A I2S   : DIN=21, BCLK=17, WS=18, SD_MODE=47 (Active-High)
 *   Controls        : SW1=42, SW2=41, SW3=40, SW4=39, VRx=1, VRy=4
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_chip_info.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "fast_jpeg.h"
#include "sdcard.h"
#include "audio_player.h"
#include "display_ili9341.h"
#include "input_diag.h"
#include "gfx.h"

static const char *TAG = "atlas_av";

/* ── Video Asset & Parameters ────────────────────────────────────────────── */
#define VIDEO_MJPEG_PATH     "/sdcard/assets/DraculaSong/InterceptorEdit/InterceptorEdit.mjpeg"
#define VIDEO_MJPEG_FALLBACK "/sdcard/assets/DraculaSong/InterceptorEdit.mjpeg"
#define VIDEO_FPS            30                      /* Target 30 FPS (exact 1470 samples/frame) */
#define FRAME_PERIOD_US     (1000000 / VIDEO_FPS)   /* 33 333 µs per frame */
#define SYNC_DROP_THRESH_US FRAME_PERIOD_US         /* 33 333 µs drop threshold (1 frame) */

/* ── PSRAM Pre-Fetch Circular Ring-Buffer Queue ──────────────────────────── */
#define FRAME_QUEUE_SIZE     8                       /* 8 x 153.6KB ≈ 1.2MB PSRAM pre-fetch cushion */
#define JPEG_MAX_FRAME_BYTES (128 * 1024)

typedef struct {
    uint16_t *buffer;        /* Pointer to 320x240 RGB565 PSRAM buffer */
    uint16_t  width;         /* Decoded image width */
    uint16_t  height;        /* Decoded image height */
    uint64_t  video_samples; /* Target audio sample timestamp */
    uint32_t  frame_number;  /* Monotonic video frame index (0, 1, 2...) */
} frame_slot_t;

typedef struct {
    frame_slot_t      slots[FRAME_QUEUE_SIZE];
    int               head;         /* Producer write index (Core 0) */
    int               tail;         /* Consumer read index (Core 1) */
    volatile int      count;        /* Ready frames currently queued */
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t sem_items;    /* Counts ready items for display */
    SemaphoreHandle_t sem_spaces;   /* Counts empty slots for decoder */
} frame_queue_t;

static frame_queue_t s_frame_queue;
static uint8_t      *s_jpeg_buf = NULL;
static uint16_t     *s_splash_fb = NULL;

/* ── Frame Queue Management ──────────────────────────────────────────────── */
static void frame_queue_init(void)
{
    for (int i = 0; i < FRAME_QUEUE_SIZE; i++) {
        s_frame_queue.slots[i].buffer = (uint16_t *)heap_caps_aligned_alloc(64, LCD_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_frame_queue.slots[i].width  = LCD_WIDTH;
        s_frame_queue.slots[i].height = LCD_HEIGHT;
        s_frame_queue.slots[i].video_samples = 0;
        s_frame_queue.slots[i].frame_number  = 0;
        if (!s_frame_queue.slots[i].buffer) {
            ESP_LOGE(TAG, "Failed to allocate PSRAM frame slot %d", i);
        }
    }
    s_frame_queue.head = 0;
    s_frame_queue.tail = 0;
    s_frame_queue.count = 0;
    s_frame_queue.mutex = xSemaphoreCreateMutex();
    s_frame_queue.sem_items  = xSemaphoreCreateCounting(FRAME_QUEUE_SIZE, 0);
    s_frame_queue.sem_spaces = xSemaphoreCreateCounting(FRAME_QUEUE_SIZE, FRAME_QUEUE_SIZE);
}

static void frame_queue_flush(void)
{
    if (!s_frame_queue.mutex) return;
    xSemaphoreTake(s_frame_queue.mutex, portMAX_DELAY);
    s_frame_queue.head = 0;
    s_frame_queue.tail = 0;
    s_frame_queue.count = 0;
    while (xSemaphoreTake(s_frame_queue.sem_items, 0) == pdTRUE) {}
    while (uxSemaphoreGetCount(s_frame_queue.sem_spaces) < FRAME_QUEUE_SIZE) {
        xSemaphoreGive(s_frame_queue.sem_spaces);
    }
    xSemaphoreGive(s_frame_queue.mutex);
}

/* ── Hybrid MJPEG Frame Reader (Supports Both Raw MJPEG and Size-Prefixed) ─ */
static bool read_mjpeg_frame(FILE *f, uint8_t *dst, size_t max_len, size_t *out_len)
{
    uint8_t header[4];
    if (fread(header, 1, 4, f) != 4) return false;

    /* If not pointing to SOI marker 0xFF 0xD8, check for Size-Prefixed Mode B first */
    if (header[0] != 0xFF || header[1] != 0xD8) {
        uint32_t size = (uint32_t)header[0] | ((uint32_t)header[1] << 8) |
                        ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);

        if (size > 100 && size <= max_len) {
            if (fread(dst, 1, size, f) == size) {
                if (dst[0] == 0xFF && dst[1] == 0xD8) {
                    *out_len = size;
                    return true;
                }
            }
        }

        /* Auto-recovery: scan forward byte-by-byte up to 4096 bytes to re-align with next SOI */
        int b;
        uint8_t prev = 0;
        int skipped = 0;
        while ((b = fgetc(f)) != EOF && skipped < 4096) {
            skipped++;
            if (prev == 0xFF && (uint8_t)b == 0xD8) {
                header[0] = 0xFF;
                header[1] = 0xD8;
                header[2] = (uint8_t)fgetc(f);
                header[3] = (uint8_t)fgetc(f);
                break;
            }
            prev = (uint8_t)b;
        }
        if (header[0] != 0xFF || header[1] != 0xD8) {
            return false;
        }
    }

    /* Mode A: Standard Raw MJPEG stream (starts with SOI marker 0xFF 0xD8) */
    memcpy(dst, header, 4);
    size_t len = 4;

    /* Scan in 4096-byte blocks for EOI marker (0xFF 0xD9) */
    while (len < max_len) {
        size_t chunk = (max_len - len > 4096) ? 4096 : (max_len - len);
        size_t n = fread(dst + len, 1, chunk, f);
        if (n == 0) break;

        size_t start = (len >= 1) ? (len - 1) : 0;
        size_t end = len + n;
        for (size_t i = start; i + 1 < end; i++) {
            if (dst[i] == 0xFF && dst[i + 1] == 0xD9) {
                size_t frame_bytes = i + 2;
                long leftover = (long)(end - frame_bytes);
                if (leftover > 0) {
                    fseek(f, -leftover, SEEK_CUR);
                }
                *out_len = frame_bytes;
                return true;
            }
        }
        len += n;
    }
    return false;
}

/* ── OSD (On-Screen Display) State ──────────────────────────────────────── */
static char s_osd_text[32] = {0};
static int  s_osd_frames   = 0;

static void set_osd(const char *text, int duration_frames)
{
    snprintf(s_osd_text, sizeof(s_osd_text), "%s", text);
    s_osd_frames = duration_frames;
}

static void render_osd_overlay(uint16_t *fb, uint16_t vid_h, bool paused, uint32_t v_frame, uint32_t a_frame)
{
    int vol = atomic_load_explicit(&g_audio_sync.volume_pct, memory_order_relaxed);

    /* 1. If paused: show centered warning banner */
    if (paused) {
        const int pw = 160, ph = 36;
        const int px = (LCD_W - pw) / 2;
        const int py = (vid_h > ph) ? (vid_h - ph) / 2 : 0;
        gfx_fill_rect(fb, px, py, pw, ph, C_PANEL_BG);
        gfx_draw_rect(fb, px, py, pw, ph, C_YELLOW);
        gfx_draw_str(fb, px + 48, py + 8, "PAUSED", C_YELLOW, C_PANEL_BG, 1);
        gfx_draw_str(fb, px + 12, py + 22, "SW1:PLAY  SW4:RESTART", C_TEXT_DIM, C_PANEL_BG, 1);
    }

    /* 2. Volume / Action Toast Banner (Top Right) or Sync Badge */
    if (s_osd_frames > 0) {
        s_osd_frames--;
        const int tw = 120, th = 24;
        const int tx = LCD_W - tw - 8;
        const int ty = 8;
        gfx_fill_rect(fb, tx, ty, tw, th, C_PANEL_BG);
        gfx_draw_rect(fb, tx, ty, tw, th, C_CYAN);
        gfx_draw_str(fb, tx + 8, ty + 7, s_osd_text, C_CYAN, C_PANEL_BG, 1);
    } else if (!paused) {
        /* Subtle On-Screen Frame Sync Indicator (Top Right) */
        char sync_str[32];
        int32_t delta = (int32_t)a_frame - (int32_t)v_frame;
        snprintf(sync_str, sizeof(sync_str), "V:%lu A:%lu (%+ld)", (unsigned long)v_frame, (unsigned long)a_frame, (long)delta);
        const int sw = 130, sh = 16;
        const int sx = LCD_W - sw - 6;
        const int sy = 6;
        gfx_fill_rect(fb, sx, sy, sw, sh, C_PANEL_BG);
        gfx_draw_rect(fb, sx, sy, sw, sh, C_BORDER);
        uint16_t sync_col = (delta >= -1 && delta <= 1) ? C_GREEN : ((delta > 0) ? C_YELLOW : C_MAGENTA);
        gfx_draw_str(fb, sx + 4, sy + 4, sync_str, sync_col, C_PANEL_BG, 1);
    }

    /* 3. Small Mini Volume Indicator (Top Left 50x8 Bar) */
    const int vx = 8, vy = 8, vw = 50, vh = 6;
    gfx_fill_rect(fb, vx, vy, vw, vh, C_DARK_BG);
    gfx_draw_rect(fb, vx, vy, vw, vh, C_BORDER);
    int fill_w = (vol * (vw - 2)) / AUDIO_UI_MAX_VOL;
    if (fill_w > 0) {
        gfx_fill_rect(fb, vx + 1, vy + 1, fill_w, vh - 2, C_GREEN);
    }
}

/* ── Control & Button Scanner Task (Core 1, 20 ms interval) ──────────────── */
static void control_task(void *pvParam)
{
    (void)pvParam;
    bool last_sw1 = false, last_sw2 = false, last_sw3 = false, last_sw4 = false;

    while (true) {
        input_state_t in = input_diag_read();

        /* SW1 (G42): Play / Pause Toggle */
        if (in.sw1 && !last_sw1) {
            bool now_playing = !atomic_load_explicit(&g_audio_sync.playing, memory_order_relaxed);
            atomic_store_explicit(&g_audio_sync.playing, now_playing, memory_order_relaxed);
            set_osd(now_playing ? "PLAYING" : "PAUSED", 45);
            ESP_LOGI(TAG, "SW1 pressed -> %s", now_playing ? "PLAY" : "PAUSE");
        }

        /* SW2 (G41): Volume Down (1% Stepping) */
        if (in.sw2 && !last_sw2) {
            int vol = atomic_load_explicit(&g_audio_sync.volume_pct, memory_order_relaxed);
            vol = (vol >= AUDIO_VOL_STEP) ? (vol - AUDIO_VOL_STEP) : 0;
            atomic_store_explicit(&g_audio_sync.volume_pct, vol, memory_order_relaxed);
            char msg[32];
            snprintf(msg, sizeof(msg), "VOL: %d%%", vol);
            set_osd(msg, 45);
            ESP_LOGI(TAG, "SW2 pressed -> Vol: %d%%", vol);
        }

        /* SW3 (G40): Volume Up (1% Stepping, 0% to 100% UI range, hard-capped at 100% UI = 50% HW) */
        if (in.sw3 && !last_sw3) {
            int vol = atomic_load_explicit(&g_audio_sync.volume_pct, memory_order_relaxed);
            vol = (vol + AUDIO_VOL_STEP <= AUDIO_UI_MAX_VOL) ? (vol + AUDIO_VOL_STEP) : AUDIO_UI_MAX_VOL;
            atomic_store_explicit(&g_audio_sync.volume_pct, vol, memory_order_relaxed);
            char msg[32];
            if (vol >= AUDIO_UI_MAX_VOL) {
                snprintf(msg, sizeof(msg), "VOL: %d%% MAX", AUDIO_UI_MAX_VOL);
            } else {
                snprintf(msg, sizeof(msg), "VOL: %d%%", vol);
            }
            set_osd(msg, 45);
            ESP_LOGI(TAG, "SW3 pressed -> Vol: %d%%%s", vol, (vol >= AUDIO_UI_MAX_VOL) ? " (MAX)" : "");
        }

        /* SW4 (G39): Restart Playback from Beginning */
        if (in.sw4 && !last_sw4) {
            atomic_store_explicit(&g_audio_sync.restart_req_audio, true, memory_order_relaxed);
            atomic_store_explicit(&g_audio_sync.restart_req_video, true, memory_order_relaxed);
            atomic_store_explicit(&g_audio_sync.playing, true, memory_order_relaxed);
            set_osd("RESTART", 45);
            ESP_LOGI(TAG, "SW4 pressed -> RESTART TRACK");
        }

        /* Joystick UP / DOWN (VRy): Live Toggle Display Inversion (0x20 INVOFF vs 0x21 INVON) */
        static bool last_joy_y = false;
        bool joy_y = (in.norm_y > 60 || in.norm_y < -60);
        if (joy_y && !last_joy_y) {
            static bool s_cur_inv = false; /* starts false (INVOFF) */
            s_cur_inv = !s_cur_inv;
            display_set_inversion(s_cur_inv);
            char msg[32];
            snprintf(msg, sizeof(msg), "INV: %s", s_cur_inv ? "ON (0x21)" : "OFF (0x20)");
            set_osd(msg, 60);
            ESP_LOGI(TAG, "Joystick -> Display Inversion %s", s_cur_inv ? "ON (0x21)" : "OFF (0x20)");
        }
        last_joy_y = joy_y;

        last_sw1 = in.sw1;
        last_sw2 = in.sw2;
        last_sw3 = in.sw3;
        last_sw4 = in.sw4;

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ── Core 0: Video Pre-Fetch & Decode Task (Producer) ────────────────────── */
static int64_t  s_decode_total_us = 0;
static uint32_t s_decode_count    = 0;

static void video_decode_task(void *pvParam)
{
    (void)pvParam;

    while (!g_audio_sync.audio_ready) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    uint32_t v_frame_idx = 0;

    while (true) {
        ESP_LOGI(TAG, "Opening video asset: %s", VIDEO_MJPEG_PATH);
        FILE *mjpeg = sdcard_open(VIDEO_MJPEG_PATH, "rb");
        if (!mjpeg) {
            ESP_LOGW(TAG, "Not found at %s, trying fallback %s", VIDEO_MJPEG_PATH, VIDEO_MJPEG_FALLBACK);
            mjpeg = sdcard_open(VIDEO_MJPEG_FALLBACK, "rb");
        }
        if (!mjpeg) {
            ESP_LOGE(TAG, "Cannot open video asset, retrying in 2s...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        while (true) {
            /* 1. If paused: yield CPU */
            if (!atomic_load_explicit(&g_audio_sync.playing, memory_order_relaxed)) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            /* 2. Handle Restart Request */
            if (atomic_load_explicit(&g_audio_sync.restart_req_video, memory_order_relaxed)) {
                fseek(mjpeg, 0, SEEK_SET);
                v_frame_idx = 0;
                frame_queue_flush();
                atomic_store_explicit(&g_audio_sync.video_primed, false, memory_order_relaxed);
                atomic_store_explicit(&g_audio_sync.restart_req_video, false, memory_order_relaxed);
                ESP_LOGI(TAG, "Decoder rewound to start");
            }

            /* 3. Wait for space in the PSRAM pre-fetch queue */
            if (xSemaphoreTake(s_frame_queue.sem_spaces, pdMS_TO_TICKS(50)) != pdTRUE) {
                continue;
            }

            /* 4. Read Next JPEG Frame from SD Card */
            size_t frame_size = 0;
            if (!read_mjpeg_frame(mjpeg, s_jpeg_buf, JPEG_MAX_FRAME_BYTES, &frame_size)) {
                /* EOF of MJPEG stream reached: wait for display to finish showing remaining frames */
                ESP_LOGI(TAG, "MJPEG EOF reached — draining queue for seamless loop");
                xSemaphoreGive(s_frame_queue.sem_spaces);

                /* Wait for Consumer (display task) to show the final frames */
                while (true) {
                    xSemaphoreTake(s_frame_queue.mutex, portMAX_DELAY);
                    int remaining = s_frame_queue.count;
                    xSemaphoreGive(s_frame_queue.mutex);
                    if (remaining == 0) break;
                    vTaskDelay(pdMS_TO_TICKS(15));
                }

                /* Trigger synchronized loop restart of both audio and video */
                atomic_store_explicit(&g_audio_sync.restart_req_audio, true, memory_order_release);
                atomic_store_explicit(&g_audio_sync.restart_req_video, true, memory_order_release);
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            /* 5. Check if decoder is severely lagging behind audio playback.
             * If audio is already ahead by > 4 frames, skip decompressing this frame! */
            int64_t cur_a_us = audio_get_playback_time_us();
            uint32_t cur_a_frame = (uint32_t)((cur_a_us * VIDEO_FPS) / 1000000LL);
            if (cur_a_us > 0 && cur_a_frame > v_frame_idx + 4) {
                v_frame_idx++;
                xSemaphoreGive(s_frame_queue.sem_spaces);
                continue;
            }

            /* 6. Decode JPEG directly into the next PSRAM queue slot with S3 SIMD acceleration */
            uint64_t video_samples = (uint64_t)v_frame_idx * AUDIO_SAMPLE_RATE / VIDEO_FPS;
            int slot = s_frame_queue.head;

            int dec_w = 0, dec_h = 0;
            int64_t t0 = esp_timer_get_time();
            bool ok = fast_jpeg_decode(s_jpeg_buf, frame_size, s_frame_queue.slots[slot].buffer, &dec_w, &dec_h);
            int64_t t1 = esp_timer_get_time();
            s_decode_total_us += (t1 - t0);
            s_decode_count++;

            uint16_t vid_h = (ok && dec_h > 0) ? (uint16_t)dec_h : 180;
            if (vid_h > LCD_HEIGHT) vid_h = LCD_HEIGHT;
            s_frame_queue.slots[slot].width         = (ok && dec_w > 0) ? (uint16_t)dec_w : LCD_WIDTH;
            s_frame_queue.slots[slot].height        = vid_h;
            s_frame_queue.slots[slot].video_samples = video_samples;
            s_frame_queue.slots[slot].frame_number  = v_frame_idx;

            v_frame_idx++;

            /* 7. Publish slot to Consumer */
            xSemaphoreTake(s_frame_queue.mutex, portMAX_DELAY);
            s_frame_queue.head = (s_frame_queue.head + 1) % FRAME_QUEUE_SIZE;
            s_frame_queue.count++;
            int cur_q = s_frame_queue.count;
            xSemaphoreGive(s_frame_queue.mutex);

            /* Signal audio task to commence streaming once at least 2 frames are queued */
            if (cur_q >= 2) {
                atomic_store_explicit(&g_audio_sync.video_primed, true, memory_order_release);
            }

            xSemaphoreGive(s_frame_queue.sem_items);
        }

        fclose(mjpeg);
    }
}

/* ── Core 1: Video Display & DMA Task (Consumer) ─────────────────────────── */
static void video_display_task(void *pvParam)
{
    (void)pvParam;

    uint16_t s_last_vid_h = 0;
    int s_consecutive_drops = 0;

    /* FPS & Hardware Diagnostics */
    uint32_t s_fps_frames = 0;
    int64_t  s_fps_last_us = esp_timer_get_time();
    int64_t  s_dma_total_us = 0;

    while (true) {
        bool paused = !atomic_load_explicit(&g_audio_sync.playing, memory_order_relaxed);

        /* 1. If paused: render paused OSD and yield */
        if (paused) {
            uint16_t vid_h = (s_last_vid_h > 0) ? s_last_vid_h : LCD_HEIGHT;
            uint16_t y_offset = (LCD_HEIGHT > vid_h) ? (LCD_HEIGHT - vid_h) / 2 : 0;
            uint16_t *cur_fb = s_frame_queue.slots[s_frame_queue.tail].buffer;
            if (cur_fb) {
                render_osd_overlay(cur_fb, vid_h, true, 0, 0);
                display_push_region(y_offset, vid_h, (const uint8_t *)cur_fb);
            }
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        /* 2. Wait for next pre-decoded frame from the PSRAM queue */
        if (xSemaphoreTake(s_frame_queue.sem_items, pdMS_TO_TICKS(20)) != pdTRUE) {
            continue;
        }

        int slot = s_frame_queue.tail;
        uint16_t *fb        = s_frame_queue.slots[slot].buffer;
        uint16_t  vid_h     = s_frame_queue.slots[slot].height;
        uint32_t  v_frame   = s_frame_queue.slots[slot].frame_number;

        /* 3. Wait for audio master clock to start before presenting initial frame */
        int64_t audio_us = audio_get_playback_time_us();
        while (audio_us == 0 && atomic_load_explicit(&g_audio_sync.playing, memory_order_relaxed)) {
            vTaskDelay(pdMS_TO_TICKS(1));
            audio_us = audio_get_playback_time_us();
        }

        /* 4. PTS Pacing against Master Audio Clock */
        const int64_t DMA_LEAD_US = 9000; /* DMA push takes ~18ms; visual scan midpoint is ~9ms */
        int64_t target_pts_us   = (int64_t)v_frame * 1000000LL / VIDEO_FPS;
        int64_t target_start_us = target_pts_us - DMA_LEAD_US;
        if (target_start_us < 0) target_start_us = 0;

        int64_t time_diff_us = target_start_us - audio_us;

        /* If video frame is severely behind audio (> 2 frames / 66ms), drop it to catch up */
        if (time_diff_us < -66000LL && s_consecutive_drops < 3) {
            s_consecutive_drops++;
            xSemaphoreTake(s_frame_queue.mutex, portMAX_DELAY);
            s_frame_queue.tail = (s_frame_queue.tail + 1) % FRAME_QUEUE_SIZE;
            s_frame_queue.count--;
            xSemaphoreGive(s_frame_queue.mutex);
            xSemaphoreGive(s_frame_queue.sem_spaces);
            continue;
        }
        s_consecutive_drops = 0;

        /* If frame is early, precisely pace BEFORE pushing to screen */
        if (time_diff_us > 500) {
            if (time_diff_us >= 2000) {
                int64_t sleep_ms = (time_diff_us / 1000) - 1;
                if (sleep_ms > 0) {
                    vTaskDelay(pdMS_TO_TICKS((uint32_t)sleep_ms));
                }
            }
            /* Sub-millisecond spin-wait to hit exact microsecond start target */
            while (true) {
                if (!atomic_load_explicit(&g_audio_sync.playing, memory_order_relaxed) ||
                    atomic_load_explicit(&g_audio_sync.restart_req_video, memory_order_relaxed)) {
                    break;
                }
                int64_t cur_a = audio_get_playback_time_us();
                if (cur_a >= target_start_us) break;
                int64_t rem = target_start_us - cur_a;
                if (rem > 1000) {
                    esp_rom_delay_us(500);
                } else if (rem > 50) {
                    esp_rom_delay_us((uint32_t)(rem / 2));
                } else {
                    break;
                }
            }
        }

        /* 5. Active Letterbox Clearing if Dimensions Changed */
        uint16_t y_offset = (LCD_HEIGHT > vid_h) ? (LCD_HEIGHT - vid_h) / 2 : 0;
        if (vid_h != s_last_vid_h) {
            s_last_vid_h = vid_h;
            display_clear_letterbox(y_offset, vid_h);
            ESP_LOGI(TAG, "Video format: %dx%d (16:9 Letterbox Y=%d..%d, active scan)",
                     s_frame_queue.slots[slot].width, vid_h, y_offset, y_offset + vid_h - 1);
        }

        /* 6. Render On-Screen HUD Overlay with Live Frame Numbers (Referenced to Visual Midpoint) */
        int64_t live_audio_us = audio_get_playback_time_us();
        int64_t visual_audio_us = live_audio_us + DMA_LEAD_US;
        uint32_t a_frame = (uint32_t)(((visual_audio_us * VIDEO_FPS) + 500000LL) / 1000000LL);
        render_osd_overlay(fb, vid_h, false, v_frame, a_frame);

        /* 7. Push Active Video Scanlines via 80MHz SPI DMA */
        int64_t t_dma0 = esp_timer_get_time();
        display_push_region(y_offset, vid_h, (const uint8_t *)fb);
        int64_t t_dma1 = esp_timer_get_time();
        s_dma_total_us += (t_dma1 - t_dma0);

        /* 8. Release Slot back to Producer */
        xSemaphoreTake(s_frame_queue.mutex, portMAX_DELAY);
        s_frame_queue.tail = (s_frame_queue.tail + 1) % FRAME_QUEUE_SIZE;
        s_frame_queue.count--;
        xSemaphoreGive(s_frame_queue.mutex);
        xSemaphoreGive(s_frame_queue.sem_spaces);

        /* 9. Frame Number & FPS Diagnostics Serial Output (Every 500ms) */
        s_fps_frames++;
        int64_t now_us = esp_timer_get_time();
        if (now_us - s_fps_last_us >= 500000LL) {
            float elapsed_s = (float)(now_us - s_fps_last_us) / 1000000.0f;
            float cur_fps   = (float)s_fps_frames / elapsed_s;
            float avg_dec   = (s_decode_count > 0) ? ((float)s_decode_total_us / (s_decode_count * 1000.0f)) : 0;
            float avg_dma   = (s_fps_frames > 0) ? ((float)s_dma_total_us / (s_fps_frames * 1000.0f)) : 0;
            int32_t frame_delta = (int32_t)a_frame - (int32_t)v_frame;
            int64_t delta_us = visual_audio_us - target_pts_us;

            ESP_LOGI(TAG, "[SYNC LOG] V_FRAME: %5lu | A_FRAME: %5lu | Delta: %+4ld frames (%+5.1f ms) | FPS: %4.1f | Dec: %4.1fms | DMA: %4.1fms | Vol: %2d%%",
                     (unsigned long)v_frame, (unsigned long)a_frame, (long)frame_delta, (float)delta_us / 1000.0f,
                     cur_fps, avg_dec, avg_dma,
                     atomic_load_explicit(&g_audio_sync.volume_pct, memory_order_relaxed));

            s_fps_frames = 0;
            s_decode_total_us = 0;
            s_decode_count = 0;
            s_dma_total_us = 0;
            s_fps_last_us = now_us;
        }
    }
}

/* ── Firmware Entry Point ────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " ATLAS Handheld Terminal - Dracula AV Player    ");
    ESP_LOGI(TAG, " Platform: ESP32-S3 | Display: ILI9341 320x240  ");
    ESP_LOGI(TAG, " Audio: MAX98357A I2S | SD: SDSPI FatFS         ");
    ESP_LOGI(TAG, " Controls: SW1:Play/Pause SW2:Vol- SW3:Vol+ SW4:Restart");
    ESP_LOGI(TAG, "=================================================");

    /* Runtime Hardware Spec Logger */
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    size_t psram_size = esp_psram_get_size();
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "---------------- HARDWARE SPEC LOG ----------------");
    ESP_LOGI(TAG, " Model: ESP32-S3 (Revision v%d.%d, %d Cores)",
             chip_info.revision / 100, chip_info.revision % 100, chip_info.cores);
    ESP_LOGI(TAG, " Flash: %lu MB (Mode: QIO @ 80MHz)", (unsigned long)(flash_size / (1024 * 1024)));
    ESP_LOGI(TAG, " PSRAM: %u MB (Mode: Octal @ 80MHz)", (unsigned int)(psram_size / (1024 * 1024)));
    ESP_LOGI(TAG, " Free Internal SRAM: %u KB (Min: %u KB)",
             (unsigned int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
             (unsigned int)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
    ESP_LOGI(TAG, " Free PSRAM: %u KB",
             (unsigned int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    ESP_LOGI(TAG, "---------------------------------------------------");

    /* 1. Initialize Display Subsystem First (Sets up SPI2 Bus) */
    ESP_ERROR_CHECK(display_init());

    /* 2. Allocate PSRAM Pre-Fetch Queue & Decoding Buffers */
    frame_queue_init();
    fast_jpeg_init();
    s_splash_fb = (uint16_t *)heap_caps_aligned_alloc(64, LCD_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_jpeg_buf  = (uint8_t *)heap_caps_aligned_alloc(64, JPEG_MAX_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!s_splash_fb || !s_jpeg_buf) {
        ESP_LOGE(TAG, "CRITICAL: Memory allocation failed for splash/decode buffers");
        return;
    }

    /* 3. Initialize Input and Audio amp subsystems */
    ESP_ERROR_CHECK(input_diag_init());
    ESP_ERROR_CHECK(audio_init());

    /* 4. Mount SD Card with Non-Blocking Display Feedback */
    esp_err_t sd_err = sdcard_init();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD Card not ready (0x%03X). Displaying prompt and waiting...", sd_err);

        gfx_clear(s_splash_fb, C_DARK_BG);
        gfx_fill_rect(s_splash_fb, 0, 0, LCD_W, 26, C_PANEL_BG);
        gfx_draw_str(s_splash_fb, 10, 8, "ATLAS - SDCARD ERROR", C_RED, C_PANEL_BG, 1);

        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "Status: 0x%03X (%s)", sd_err, esp_err_to_name(sd_err));
        gfx_draw_str(s_splash_fb, 15, 42, err_msg, C_WHITE, C_DARK_BG, 1);
        gfx_draw_str(s_splash_fb, 15, 70, "1. Insert FAT32 MicroSD card", C_CYAN, C_DARK_BG, 1);
        gfx_draw_str(s_splash_fb, 15, 90, "2. File: .../InterceptorEdit.wav", C_TEXT_DIM, C_DARK_BG, 1);
        gfx_draw_str(s_splash_fb, 15, 110, "3. File: .../InterceptorEdit.mjpeg", C_TEXT_DIM, C_DARK_BG, 1);
        gfx_draw_str(s_splash_fb, 15, 150, "Waiting for SD card insertion...", C_YELLOW, C_DARK_BG, 1);
        display_push_frame((const uint8_t *)s_splash_fb);

        while (sd_err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            sd_err = sdcard_init();
            if (sd_err == ESP_OK) {
                ESP_LOGI(TAG, "SD Card detected and mounted!");
                break;
            }
        }
    }

    /* 5. Splash screen while starting playback */
    gfx_clear(s_splash_fb, C_DARK_BG);
    gfx_fill_rect(s_splash_fb, 0, 0, LCD_W, 26, C_PANEL_BG);
    gfx_draw_str(s_splash_fb, 10, 8, "ATLAS - TAME IMPALA: DRACULA", C_CYAN, C_PANEL_BG, 1);
    gfx_draw_str(s_splash_fb, 20, 100, "Starting AV Playback...", C_GREEN, C_DARK_BG, 1);
    gfx_draw_str(s_splash_fb, 20, 130, "SW1:Play/Pause SW2:Vol- SW3:Vol+ SW4:Restart", C_TEXT_DIM, C_DARK_BG, 1);
    display_push_frame((const uint8_t *)s_splash_fb);

    /* 6. Launch Tasks: Dual-Core Pipelined Architecture */
    /* Core 0: High-priority Audio Task (pri 22) + Background Pre-Fetch/Decode Task (pri 5) */
    xTaskCreatePinnedToCore(audio_task, "audio_task", 8192, NULL, 22, NULL, 0);
    xTaskCreatePinnedToCore(video_decode_task, "vid_dec_task", 8192, NULL, 5, NULL, 0);

    /* Core 1: Display DMA Consumer Task (pri 6) + Button Control Task (pri 4) */
    xTaskCreatePinnedToCore(video_display_task, "vid_disp_task", 8192, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(control_task, "control_task", 3072, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "ATLAS AV Player running: Core 0 [Audio + Pre-Fetch Decode], Core 1 [Display DMA + Controls].");
}
