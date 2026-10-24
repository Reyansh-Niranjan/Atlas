/**
 * @file input_diag.c
 * @brief ADC1 Joystick & Outemu Switches Driver for ATLAS Diagnostic Suite
 */
#include "input_diag.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "input_diag";

#define JOYSTICK_ADC_UNIT     ADC_UNIT_1
#define JOYSTICK_ADC_CH_X     ADC_CHANNEL_0  /* GPIO 1 */
#define JOYSTICK_ADC_CH_Y     ADC_CHANNEL_3  /* GPIO 4 */

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle_x = NULL;
static adc_cali_handle_t s_cali_handle_y = NULL;

esp_err_t input_diag_init(void)
{
    /* 1. Configure Switches SW1..SW4 as Active-Low Inputs with Internal Pull-Ups */
    gpio_config_t btn_io = {
        .pin_bit_mask = (1ULL << PIN_SW1) | (1ULL << PIN_SW2) |
                        (1ULL << PIN_SW3) | (1ULL << PIN_SW4),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_io));

    /* 2. Configure ADC1 Unit */
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = JOYSTICK_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &s_adc_handle));

    /* 3. Configure ADC1 Channels for VRx & VRy */
    adc_oneshot_chan_cfg_t chan_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, JOYSTICK_ADC_CH_X, &chan_config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, JOYSTICK_ADC_CH_Y, &chan_config));

    /* 4. Optional Curve-Fitting Calibration */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg_x = {
        .unit_id  = JOYSTICK_ADC_UNIT,
        .chan     = JOYSTICK_ADC_CH_X,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_cali_create_scheme_curve_fitting(&cali_cfg_x, &s_cali_handle_x);

    adc_cali_curve_fitting_config_t cali_cfg_y = {
        .unit_id  = JOYSTICK_ADC_UNIT,
        .chan     = JOYSTICK_ADC_CH_Y,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_cali_create_scheme_curve_fitting(&cali_cfg_y, &s_cali_handle_y);
#endif

    ESP_LOGI(TAG, "Input Diag Initialized: SW1(42), SW2(41), SW3(40), SW4(39), ADC1 CH0(1), CH3(4)");
    return ESP_OK;
}

input_state_t input_diag_read(void)
{
    input_state_t state = {0};

    /* Read buttons (Active LOW -> inverted so true = pressed) */
    state.sw1 = (gpio_get_level(PIN_SW1) == 0);
    state.sw2 = (gpio_get_level(PIN_SW2) == 0);
    state.sw3 = (gpio_get_level(PIN_SW3) == 0);
    state.sw4 = (gpio_get_level(PIN_SW4) == 0);

    /* Read ADC raw */
    int raw_x = 0, raw_y = 0;
    adc_oneshot_read(s_adc_handle, JOYSTICK_ADC_CH_X, &raw_x);
    adc_oneshot_read(s_adc_handle, JOYSTICK_ADC_CH_Y, &raw_y);

    state.raw_x = raw_x;
    state.raw_y = raw_y;

    /* Voltage conversion */
    if (s_cali_handle_x) {
        adc_cali_raw_to_voltage(s_cali_handle_x, raw_x, &state.voltage_x);
    } else {
        state.voltage_x = (raw_x * 3300) / 4095;
    }

    if (s_cali_handle_y) {
        adc_cali_raw_to_voltage(s_cali_handle_y, raw_y, &state.voltage_y);
    } else {
        state.voltage_y = (raw_y * 3300) / 4095;
    }

    /* Normalized centering (-100..+100) assuming center ~2048 */
    state.norm_x = ((raw_x - 2048) * 100) / 2048;
    state.norm_y = ((raw_y - 2048) * 100) / 2048;
    if (state.norm_x < -100) state.norm_x = -100;
    if (state.norm_x > 100)  state.norm_x = 100;
    if (state.norm_y < -100) state.norm_y = -100;
    if (state.norm_y > 100)  state.norm_y = 100;

    return state;
}
