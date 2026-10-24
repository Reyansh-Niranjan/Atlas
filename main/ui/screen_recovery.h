/**
 * @file screen_recovery.h
 * @brief Failsafe Safe-Mode Recovery ROM UI for Atlas OS
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run the interactive Safe Mode Recovery screen.
 * Blocks and handles user diagnostic controls.
 */
void screen_recovery_run(void);

#ifdef __cplusplus
}
#endif
