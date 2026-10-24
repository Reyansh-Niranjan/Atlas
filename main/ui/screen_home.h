/**
 * @file screen_home.h
 * @brief 2x2 Grid Home Launcher & Application Dispatcher for Atlas OS
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run the interactive 2x2 Grid Home Launcher loop.
 * Renders the primary UI, handles joystick edge navigation, and processes button events.
 */
void screen_home_run(void);

#ifdef __cplusplus
}
#endif
