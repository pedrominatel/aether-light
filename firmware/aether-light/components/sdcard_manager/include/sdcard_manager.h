#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SDCARD_MANAGER_MAX_FILENAME 255

typedef struct {
    bool mounted;
    esp_err_t last_error;
    uint64_t total_bytes;
    uint64_t free_bytes;
    char card_name[16];
} sdcard_manager_status_t;

/* Mounts the board's microSD card. Safe to call again to retry after insertion. */
esp_err_t sdcard_manager_init(void);

bool sdcard_manager_is_mounted(void);
void sdcard_manager_get_status(sdcard_manager_status_t *status);

/* Only plain filenames in the card root are accepted. */
bool sdcard_manager_is_valid_filename(const char *filename);
esp_err_t sdcard_manager_make_path(const char *filename, char *path, size_t path_size);
const char *sdcard_manager_mount_path(void);

#ifdef __cplusplus
}
#endif
