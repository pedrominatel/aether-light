#include "esp_log.h"
#include "network_manager.h"
#include "http_server.h"

static const char *TAG = "al";

void app_main(void)
{
    ESP_LOGI(TAG, "Bringing up ESP32-P4");

    ESP_ERROR_CHECK(network_manager_init());
    ESP_ERROR_CHECK(network_manager_start());
    http_server_start_when_network_ready();
}
