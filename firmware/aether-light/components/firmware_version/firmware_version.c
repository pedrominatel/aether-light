#include "firmware_version.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"

static esp_err_t parse_part(const char **cursor, uint32_t *value, char separator)
{
    if (cursor == NULL || *cursor == NULL || value == NULL || **cursor < '0' || **cursor > '9') {
        return ESP_ERR_INVALID_ARG;
    }
    if (**cursor == '0' && (*cursor)[1] >= '0' && (*cursor)[1] <= '9') {
        return ESP_ERR_INVALID_ARG;
    }

    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(*cursor, &end, 10);
    if (errno == ERANGE || parsed > UINT32_MAX || end == *cursor || *end != separator) {
        return ESP_ERR_INVALID_ARG;
    }

    *value = (uint32_t)parsed;
    *cursor = separator == '\0' ? end : end + 1;
    return ESP_OK;
}

esp_err_t firmware_version_parse(const char *text, firmware_semver_t *version)
{
    if (text == NULL || version == NULL || *text == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    firmware_semver_t parsed = {0};
    const char *cursor = text;
    if (parse_part(&cursor, &parsed.major, '.') != ESP_OK ||
        parse_part(&cursor, &parsed.minor, '.') != ESP_OK ||
        parse_part(&cursor, &parsed.patch, '\0') != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    *version = parsed;
    return ESP_OK;
}

int firmware_version_compare(const firmware_semver_t *left, const firmware_semver_t *right)
{
    if (left == NULL || right == NULL) {
        return 0;
    }
    if (left->major != right->major) {
        return left->major < right->major ? -1 : 1;
    }
    if (left->minor != right->minor) {
        return left->minor < right->minor ? -1 : 1;
    }
    if (left->patch != right->patch) {
        return left->patch < right->patch ? -1 : 1;
    }
    return 0;
}

void firmware_version_get_info(firmware_version_info_t *info)
{
    if (info == NULL) {
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    memset(info, 0, sizeof(*info));
    snprintf(info->version, sizeof(info->version), "%s", app->version);
    snprintf(info->project_name, sizeof(info->project_name), "%s", app->project_name);
    snprintf(info->idf_version, sizeof(info->idf_version), "%s", app->idf_ver);
    snprintf(info->build_date, sizeof(info->build_date), "%s", app->date);
    snprintf(info->build_time, sizeof(info->build_time), "%s", app->time);
    info->secure_version = app->secure_version;

    for (size_t i = 0; i < sizeof(app->app_elf_sha256); ++i) {
        snprintf(&info->app_sha256[i * 2], 3, "%02x", app->app_elf_sha256[i]);
    }
}

bool firmware_version_is_newer(const char *candidate)
{
    firmware_semver_t candidate_version;
    firmware_semver_t current_version;
    const esp_app_desc_t *app = esp_app_get_description();

    return firmware_version_parse(candidate, &candidate_version) == ESP_OK &&
           firmware_version_parse(app->version, &current_version) == ESP_OK &&
           firmware_version_compare(&candidate_version, &current_version) > 0;
}
