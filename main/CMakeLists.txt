cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS
        "main.c"
        "boot/atlas_boot.c"
        "core/atlas_event.c"
        "core/atlas_service.c"
        "core/atlas_sysinfo.c"
        "drivers/atlas_display.c"
        "drivers/atlas_input.c"
        "drivers/atlas_audio.c"
        "drivers/atlas_sdcard.c"
        "system/atlas_init.c"
        "system/app_registry.c"
        "ui/atlas_gfx.c"
        "ui/screen_home.c"
        "ui/screen_recovery.c"
    INCLUDE_DIRS
        "include"
        "boot"
        "core"
        "drivers"
        "system"
        "ui"
    REQUIRES
        driver
        esp_driver_spi
        esp_driver_i2s
        esp_driver_gpio
        esp_driver_sdspi
        esp_adc
        esp_rom
        esp_psram
        esp_timer
        esp_event
        esp_hw_support
        fatfs
        sdmmc
        nvs_flash
        freertos
        log
        heap
        app_update
        esp_partition
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-format-truncation)
