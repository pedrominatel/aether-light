#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_eth.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_HAL_LED_CHANNEL_COUNT 16

const char *board_hal_get_name(void);
const int *board_hal_get_led_gpios(size_t *count);
esp_err_t board_hal_validate_led_gpios(void);

esp_err_t board_hal_ethernet_install(esp_eth_handle_t *handle);

bool board_hal_sdcard_supported(void);
esp_err_t board_hal_sdcard_mount(void);
void board_hal_sdcard_unmount(void);
sdmmc_card_t *board_hal_sdcard_get_handle(void);
const char *board_hal_sdcard_mount_path(void);

#ifdef __cplusplus
}
#endif
