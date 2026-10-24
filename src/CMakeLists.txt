cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS
        "main.c"
        "audio_player.c"
        "display_ili9341.c"
        "sdcard.c"
        "input_diag.c"
        "gfx.c"
        "fast_jpeg.c"
        "jpegdec/s3_simd_420.S"
        "jpegdec/s3_simd_444.S"
        "jpegdec/s3_simd_dequant.S"
    INCLUDE_DIRS
        "."
        "../include"
        "jpegdec"
    REQUIRES
        driver
        esp_driver_i2s
        esp_driver_spi
        esp_adc
        esp_rom
        fatfs
        sdmmc
        log
        freertos
        spi_flash
        heap
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-shift-negative-value)
