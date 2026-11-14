if(IDF_TARGET STREQUAL "esp32p4")
    set(BOARD_HAL_TARGET_SOURCE "board_hal_esp32p4.c")
elseif(IDF_TARGET STREQUAL "esp32s31")
    set(BOARD_HAL_TARGET_SOURCE "board_hal_esp32s31.c")
else()
    message(FATAL_ERROR "Aether Light supports only ESP32-P4 and ESP32-S31")
endif()

idf_component_register(
    SRCS "${BOARD_HAL_TARGET_SOURCE}"
    INCLUDE_DIRS "include"
    REQUIRES esp_eth fatfs
)
