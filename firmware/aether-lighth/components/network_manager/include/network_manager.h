#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NETWORK_MANAGER_DISCONNECTED = 0,
    NETWORK_MANAGER_CONNECTING,
    NETWORK_MANAGER_CONNECTED_ETH,
    NETWORK_MANAGER_CONNECTED_WIFI,
} network_manager_state_t;

/* Initializes NVS, esp_netif and the default event loop. Call once before network_manager_start(). */
esp_err_t network_manager_init(void);

/* Brings up Ethernet; falls back to WiFi (credentials read from NVS) if Ethernet doesn't get an IP in time. */
esp_err_t network_manager_start(void);

/* Persists WiFi credentials to NVS, used by the fallback WiFi connection. */
esp_err_t network_manager_set_wifi_credentials(const char *ssid, const char *password);

/* Reads WiFi credentials from NVS if available. */
esp_err_t network_manager_get_wifi_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len);

network_manager_state_t network_manager_get_state(void);

#ifdef __cplusplus
}
#endif
