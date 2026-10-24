/**
 * @file input_diag.h
 * @brief ATLAS Hardware Diagnostic - ADC1 Joystick & Outemu Switches
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIN_SW1   42
#define PIN_SW2   41
#define PIN_SW3   40
#define PIN_SW4   39

typedef struct {
    int  raw_x;       // 0..4095
    int  raw_y;       // 0..4095
    int  voltage_x;   // mV
    int  voltage_y;   // mV
    int  norm_x;      // -100..+100
    int  norm_y;      // -100..+100
    bool sw1;         // true = pressed (LOW)
    bool sw2;         // true = pressed (LOW)
    bool sw3;         // true = pressed (LOW)
    bool sw4;         // true = pressed (LOW)
} input_state_t;

/**
 * @brief Initialize ADC1 for VRx (GPIO 1) / VRy (GPIO 4) and GPIO inputs for SW1-4.
 */
esp_err_t input_diag_init(void);

/**
 * @brief Read current state of joystick ADC and switches.
 */
input_state_t input_diag_read(void);

#ifdef __cplusplus
}
#endif
