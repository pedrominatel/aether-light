#include "esp_log.h"
#include "network_manager.h"
#include "ultraled_manager.h"
#include "web_interface.h"

static const char *TAG = "al";

void app_main(void)
{
    ESP_LOGI(TAG, "Bringing up ESP32-P4");

    ESP_ERROR_CHECK(network_manager_init());

    esp_err_t ultraled_err = ultraled_manager_init();
    if (ultraled_err != ESP_OK) {
        ESP_LOGW(TAG, "UltraLED is not active: %s", esp_err_to_name(ultraled_err));
    }

    ESP_ERROR_CHECK(network_manager_start());
    web_interface_start_when_network_ready();
}
