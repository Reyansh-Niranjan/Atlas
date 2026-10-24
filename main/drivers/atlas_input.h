/**
 * @file atlas_input.h
 * @brief Calibrated Input Driver for Atlas: Analog Joystick (X/Y) & Debounced Tactile Switches
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Unified input state snapshot for Atlas handheld controls.
 */
typedef struct {
    int16_t joy_x;     /**< X-axis coordinate: -100 (left) to +100 (right), 0 deadzone */
    int16_t joy_y;     /**< Y-axis coordinate: -100 (up) to +100 (down), 0 deadzone */
    bool    joy_click; /**< Joystick center click (false if not connected) */
    bool    btn_a;     /**< Mechanical Switch SW1 (GPIO 42, active-low debounced) */
    bool    btn_b;     /**< Mechanical Switch SW2 (GPIO 41, active-low debounced) */
    bool    btn_c;     /**< Mechanical Switch SW3 (GPIO 40, active-low debounced) */
    bool    btn_d;     /**< Mechanical Switch SW4 (GPIO 39, active-low debounced) */
    bool    btn_menu;  /**< Menu button alias (maps to SW3 / btn_c) */
} atlas_input_state_t;

/**
 * @brief Initialize ADC1 for joystick channels, configure GPIO pull-ups for tactile switches,
 * and spawn the high-priority background sampling task.
 *
 * @return ESP_OK on success.
 */
esp_err_t atlas_input_init(void);

/**
 * @brief Thread-safe instantaneous snapshot of debounced buttons and filtered joystick coordinates.
 *
 * @return Current atlas_input_state_t.
 */
atlas_input_state_t atlas_input_get_state(void);

#ifdef __cplusplus
}
#endif
