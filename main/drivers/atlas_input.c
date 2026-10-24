/**
 * @file atlas_input.c
 * @brief Calibrated Input Driver: Multi-Sampled ADC Joystick & Bitmask Debounced Switches
 */
#include "atlas_input.h"
#include "atlas_pins.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "atlas_input";

/* ── Sampling & Filtering Constants ──────────────────────────────────────── */
#define INPUT_POLL_PERIOD_MS    10   /* 100 Hz polling rate */
#define JOY_SAMPLES_WINDOW      8    /* 8-sample moving average window */
#define ADC_MAX_RAW             4095
#define ADC_CENTER_RAW          2048
#define ADC_DEADZONE_RAW        102  /* ~5% deadzone around center */

/* ── Hardware Handles ────────────────────────────────────────────────────── */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_cali_x = NULL;
static adc_cali_handle_t         s_cali_y = NULL;
static SemaphoreHandle_t         s_state_mutex = NULL;
static atlas_input_state_t       s_current_state = {0};

/* Rolling Average History */
static int s_joy_x_history[JOY_SAMPLES_WINDOW] = {0};
static int s_joy_y_history[JOY_SAMPLES_WINDOW] = {0};
static int s_joy_hist_idx = 0;
static bool s_joy_hist_primed = false;

/* 8-Bit Shift-Register Debounce States */
static uint8_t s_deb_btn_a = 0xFF;
static uint8_t s_deb_btn_b = 0xFF;
static uint8_t s_deb_btn_c = 0xFF;
static uint8_t s_deb_btn_d = 0xFF;

/* ── ADC Calibration Initialization Helper ───────────────────────────────── */
static void init_adc_calibration(void)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg_x = {
        .unit_id  = ATLAS_ADC_UNIT_JOY,
        .chan     = ATLAS_ADC_CH_JOY_X,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg_x, &s_cali_x) != ESP_OK) {
        s_cali_x = NULL;
    }

    adc_cali_curve_fitting_config_t cali_cfg_y = {
        .unit_id  = ATLAS_ADC_UNIT_JOY,
        .chan     = ATLAS_ADC_CH_JOY_Y,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg_y, &s_cali_y) != ESP_OK) {
        s_cali_y = NULL;
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg_x = {
        .unit_id  = ATLAS_ADC_UNIT_JOY,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg_x, &s_cali_x) != ESP_OK) {
        s_cali_x = NULL;
    }

    adc_cali_line_fitting_config_t cali_cfg_y = {
        .unit_id  = ATLAS_ADC_UNIT_JOY,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg_y, &s_cali_y) != ESP_OK) {
        s_cali_y = NULL;
    }
#endif
}

/* ── Normalization with Deadzone Clamping ─────────────────────────────────── */
static int16_t normalize_axis(int raw_avg)
{
    int delta = raw_avg - ADC_CENTER_RAW;

    /* ±5% deadzone around center: suppress idle jitter */
    if (abs(delta) <= ADC_DEADZONE_RAW) {
        return 0;
    }

    int16_t norm = 0;
    if (delta > 0) {
        norm = (int16_t)(((delta - ADC_DEADZONE_RAW) * 100) / (ADC_MAX_RAW - ADC_CENTER_RAW - ADC_DEADZONE_RAW));
    } else {
        norm = (int16_t)(((delta + ADC_DEADZONE_RAW) * 100) / (ADC_CENTER_RAW - ADC_DEADZONE_RAW));
    }

    if (norm > 100)  norm = 100;
    if (norm < -100) norm = -100;
    return norm;
}

/* ── Dedicated High-Priority Input Worker Task ───────────────────────────── */
static void atlas_input_task(void *pvParam)
{
    (void)pvParam;
    TickType_t last_wake_time = xTaskGetTickCount();

    while (true) {
        /* 1. Read Analog Joystick X & Y via ADC1 */
        int raw_x = 0, raw_y = 0;
        adc_oneshot_read(s_adc_handle, ATLAS_ADC_CH_JOY_X, &raw_x);
        adc_oneshot_read(s_adc_handle, ATLAS_ADC_CH_JOY_Y, &raw_y);

        /* 2. Update 8-Sample Circular Moving Average Buffer */
        s_joy_x_history[s_joy_hist_idx] = raw_x;
        s_joy_y_history[s_joy_hist_idx] = raw_y;
        s_joy_hist_idx = (s_joy_hist_idx + 1) % JOY_SAMPLES_WINDOW;

        int sum_x = 0, sum_y = 0;
        int count = s_joy_hist_primed ? JOY_SAMPLES_WINDOW : s_joy_hist_idx;
        if (s_joy_hist_idx == 0) s_joy_hist_primed = true;

        for (int i = 0; i < (s_joy_hist_primed ? JOY_SAMPLES_WINDOW : count); i++) {
            sum_x += s_joy_x_history[i];
            sum_y += s_joy_y_history[i];
        }
        int div = s_joy_hist_primed ? JOY_SAMPLES_WINDOW : (count > 0 ? count : 1);
        int avg_x = sum_x / div;
        int avg_y = sum_y / div;

        int16_t norm_x = normalize_axis(avg_x);
        int16_t norm_y = (int16_t)(-normalize_axis(avg_y)); /* Inverted Y-axis polarity */

        /* 3. 8-Bit Shift-Register Debounce for Active-LOW Switches */
        int raw_btn_a = gpio_get_level(ATLAS_PIN_BTN_A);
        int raw_btn_b = gpio_get_level(ATLAS_PIN_BTN_B);
        int raw_btn_c = gpio_get_level(ATLAS_PIN_BTN_C);
        int raw_btn_d = gpio_get_level(ATLAS_PIN_BTN_D);

        s_deb_btn_a = (s_deb_btn_a << 1) | (raw_btn_a & 1);
        s_deb_btn_b = (s_deb_btn_b << 1) | (raw_btn_b & 1);
        s_deb_btn_c = (s_deb_btn_c << 1) | (raw_btn_c & 1);
        s_deb_btn_d = (s_deb_btn_d << 1) | (raw_btn_d & 1);
        if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) == pdTRUE) {
            /* Active-LOW: 0x00 = 8 consecutive samples low -> pressed */
            if (s_deb_btn_a == 0x00) s_current_state.btn_a = true;
            else if (s_deb_btn_a == 0xFF) s_current_state.btn_a = false;

            if (s_deb_btn_b == 0x00) s_current_state.btn_b = true;
            else if (s_deb_btn_b == 0xFF) s_current_state.btn_b = false;

            if (s_deb_btn_c == 0x00) s_current_state.btn_c = true;
            else if (s_deb_btn_c == 0xFF) s_current_state.btn_c = false;

            if (s_deb_btn_d == 0x00) s_current_state.btn_d = true;
            else if (s_deb_btn_d == 0xFF) s_current_state.btn_d = false;

            s_current_state.btn_menu  = s_current_state.btn_c;
            s_current_state.joy_x     = norm_x;
            s_current_state.joy_y     = norm_y;
            s_current_state.joy_click = false;

            xSemaphoreGive(s_state_mutex);
        }

        /* 4. Enforce strict 10ms execution cadence */
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(INPUT_POLL_PERIOD_MS));
    }
}

/* ── Public API Implementation ───────────────────────────────────────────── */

esp_err_t atlas_input_init(void)
{
    ESP_LOGI(TAG, "Initializing calibrated input driver (Joystick ADC1 + 4 MK Switches)...");

    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) {
        return ESP_ERR_NO_MEM;
    }

    /* 1. Configure Switches (SW1..SW4) with Internal Pull-Ups */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << ATLAS_PIN_BTN_A) | (1ULL << ATLAS_PIN_BTN_B) |
                        (1ULL << ATLAS_PIN_BTN_C) | (1ULL << ATLAS_PIN_BTN_D),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));

    /* 2. Configure ADC1 Oneshot Unit for Joystick VRx & VRy */
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id  = ATLAS_ADC_UNIT_JOY,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, ATLAS_ADC_CH_JOY_X, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, ATLAS_ADC_CH_JOY_Y, &chan_cfg));

    /* 3. Setup Hardware ADC Calibration */
    init_adc_calibration();

    /* 4. Spawn Dedicated FreeRTOS Input Task (Priority 10) */
    BaseType_t ret = xTaskCreate(atlas_input_task, "atlas_input", 3072, NULL, 10, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create input polling task!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Input driver initialized: SW1(%d), SW2(%d), SW3(%d), SW4(%d), Joy(ADC1 CH%d/CH%d)",
             ATLAS_PIN_BTN_A, ATLAS_PIN_BTN_B, ATLAS_PIN_BTN_C, ATLAS_PIN_BTN_D,
             ATLAS_ADC_CH_JOY_X, ATLAS_ADC_CH_JOY_Y);
    return ESP_OK;
}

atlas_input_state_t atlas_input_get_state(void)
{
    atlas_input_state_t snapshot = {0};
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        snapshot = s_current_state;
        xSemaphoreGive(s_state_mutex);
    }
    return snapshot;
}
