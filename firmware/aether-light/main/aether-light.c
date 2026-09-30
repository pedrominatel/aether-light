#include "esp_log.h"
#include "ddp_manager.h"
#include "firmware_version.h"
#include "network_manager.h"
#include "sdcard_manager.h"
#include "ultraled_manager.h"
#include "web_interface.h"

static const char *TAG = "al";

void app_main(void)
{
    firmware_version_info_t firmware;
    firmware_version_get_info(&firmware);
    ESP_LOGI(TAG, "Bringing up %s firmware %s (SHA-256 %.12s)",
             firmware.project_name, firmware.version, firmware.app_sha256);

    esp_err_t sdcard_err = sdcard_manager_init();
    if (sdcard_err != ESP_OK) {
        ESP_LOGW(TAG, "SD card is not available: %s", esp_err_to_name(sdcard_err));
    }

    ESP_ERROR_CHECK(network_manager_init());

    esp_err_t ultraled_err = ultraled_manager_init();
    if (ultraled_err != ESP_OK) {
        ESP_LOGW(TAG, "UltraLED is not active: %s", esp_err_to_name(ultraled_err));
    }

    esp_err_t ddp_err = ddp_manager_init();
    if (ddp_err != ESP_OK) {
        ESP_LOGW(TAG, "DDP is not active: %s", esp_err_to_name(ddp_err));
    }

    ESP_ERROR_CHECK(network_manager_start());
    if (ddp_err == ESP_OK) {
        ddp_manager_start_when_network_ready();
    }
    web_interface_start_when_network_ready();
}
