/**
 * @file atlas_init.c
 * @brief Atlas OS Boot Engine Compatibility Layer
 *
 * Delegates to the structured boot manager (atlas_boot_run).
 * Preserves binary and API compatibility for existing callers.
 */
#include "atlas_init.h"
#include "atlas_boot.h"

esp_err_t atlas_system_init(atlas_system_diag_t *out_diag)
{
    return atlas_boot_run(out_diag);
}
