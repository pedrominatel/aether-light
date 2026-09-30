#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FIRMWARE_VERSION_TEXT_LENGTH 32
#define FIRMWARE_VERSION_SHA256_LENGTH 65

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} firmware_semver_t;

typedef struct {
    char version[FIRMWARE_VERSION_TEXT_LENGTH];
    char project_name[FIRMWARE_VERSION_TEXT_LENGTH];
    char idf_version[FIRMWARE_VERSION_TEXT_LENGTH];
    char build_date[16];
    char build_time[16];
    char app_sha256[FIRMWARE_VERSION_SHA256_LENGTH];
    uint32_t secure_version;
} firmware_version_info_t;

/** Read the version and immutable metadata embedded in the running image. */
void firmware_version_get_info(firmware_version_info_t *info);

/** Parse a strict MAJOR.MINOR.PATCH version. */
esp_err_t firmware_version_parse(const char *text, firmware_semver_t *version);

/** Compare two parsed versions; returns less than, equal to, or greater than zero. */
int firmware_version_compare(const firmware_semver_t *left, const firmware_semver_t *right);

/** Return true only when candidate is valid and newer than the running firmware. */
bool firmware_version_is_newer(const char *candidate);

#ifdef __cplusplus
}
#endif
