/**
 * @file screen_onboarding.h
 * @brief First-Boot Onboarding & Wi-Fi Provisioning Wizard UI for ATLAS OS
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Execute the interactive onboarding setup sequence.
 * Blocks until the user establishes a Wi-Fi connection, chooses offline mode,
 * or triggers a factory reset.
 */
void screen_onboarding_run(void);

#ifdef __cplusplus
}
#endif
