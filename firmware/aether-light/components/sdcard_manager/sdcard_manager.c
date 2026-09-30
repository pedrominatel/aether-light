#include "sdcard_manager.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "bsp/esp32_p4_function_ev_board.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sdcard_manager";

static SemaphoreHandle_t s_lock;
static bool s_mounted;
static esp_err_t s_last_error = ESP_ERR_INVALID_STATE;

static void lock_manager(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock_manager(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

esp_err_t sdcard_manager_init(void)
{
    lock_manager();
    if (s_mounted) {
        unlock_manager();
        return ESP_OK;
    }

    s_last_error = bsp_sdcard_mount();
    s_mounted = s_last_error == ESP_OK;
    if (s_mounted) {
        sdmmc_card_t *card = bsp_sdcard_get_handle();
        ESP_LOGI(TAG, "Mounted microSD card%s%s at %s",
                 card != NULL ? " " : "", card != NULL ? card->cid.name : "", BSP_SD_MOUNT_POINT);
    } else {
        ESP_LOGW(TAG, "microSD card is unavailable: %s", esp_err_to_name(s_last_error));
        /* The BSP allocates its LDO handle before probing the card. Clean it up
         * after a failed probe so a later page refresh can retry mounting. */
        bsp_sdcard_unmount();
    }
    esp_err_t result = s_last_error;
    unlock_manager();
    return result;
}

bool sdcard_manager_is_mounted(void)
{
    lock_manager();
    bool mounted = s_mounted;
    unlock_manager();
    return mounted;
}

void sdcard_manager_get_status(sdcard_manager_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    lock_manager();
    status->mounted = s_mounted;
    status->last_error = s_last_error;
    if (s_mounted) {
        sdmmc_card_t *card = bsp_sdcard_get_handle();
        if (card != NULL) {
            snprintf(status->card_name, sizeof(status->card_name), "%s", card->cid.name);
        }
        esp_err_t info_error = esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &status->total_bytes, &status->free_bytes);
        if (info_error != ESP_OK) {
            ESP_LOGW(TAG, "Could not read microSD capacity: %s", esp_err_to_name(info_error));
        }
    }
    unlock_manager();
}

bool sdcard_manager_is_valid_filename(const char *filename)
{
    if (filename == NULL) {
        return false;
    }

    size_t length = strnlen(filename, SDCARD_MANAGER_MAX_FILENAME + 1);
    if (length == 0 || length > SDCARD_MANAGER_MAX_FILENAME ||
        strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0 ||
        strcasecmp(filename, ".aether-upload.tmp") == 0 ||
        filename[length - 1] == ' ' || filename[length - 1] == '.') {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        unsigned char character = (unsigned char)filename[i];
        if (character < 0x20 || character == 0x7f || character == '/' || character == '\\' ||
            character == ':' || character == '*' || character == '?' || character == '"' ||
            character == '<' || character == '>' || character == '|') {
            return false;
        }
    }
    return true;
}

esp_err_t sdcard_manager_make_path(const char *filename, char *path, size_t path_size)
{
    if (path == NULL || !sdcard_manager_is_valid_filename(filename)) {
        return ESP_ERR_INVALID_ARG;
    }
    int required = snprintf(path, path_size, "%s/%s", BSP_SD_MOUNT_POINT, filename);
    return required < 0 || (size_t)required >= path_size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

const char *sdcard_manager_mount_path(void)
{
    return BSP_SD_MOUNT_POINT;
}
