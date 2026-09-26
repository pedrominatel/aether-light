#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

#define NETWORK_MANAGER_IPV4_STRING_SIZE 16
#define NETWORK_MANAGER_WIFI_SSID_SIZE   33
#define NETWORK_MANAGER_MAX_SCAN_RESULTS 20

typedef struct {
    bool use_static_ip;
    char ip[NETWORK_MANAGER_IPV4_STRING_SIZE];
    char gateway[NETWORK_MANAGER_IPV4_STRING_SIZE];
    char netmask[NETWORK_MANAGER_IPV4_STRING_SIZE];
    char dns[NETWORK_MANAGER_IPV4_STRING_SIZE];
} network_manager_ipv4_config_t;

typedef struct {
    char ssid[NETWORK_MANAGER_WIFI_SSID_SIZE];
    int8_t rssi;
    uint8_t channel;
    bool secured;
} network_manager_wifi_ap_t;

/* Initializes NVS, esp_netif and the default event loop. Call once before network_manager_start(). */
esp_err_t network_manager_init(void);

/* Brings up Ethernet; falls back to WiFi (credentials read from NVS) if Ethernet doesn't get an IP in time. */
esp_err_t network_manager_start(void);

/* Persists WiFi credentials to NVS, used by the fallback WiFi connection. */
esp_err_t network_manager_set_wifi_credentials(const char *ssid, const char *password);

/* Reads WiFi credentials from NVS if available. */
esp_err_t network_manager_get_wifi_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len);

/* Performs a blocking scan and returns nearby networks ordered by signal strength. */
esp_err_t network_manager_scan_wifi(network_manager_wifi_ap_t *results, size_t max_results, size_t *result_count);

/* Validates and persists the IPv4 mode. Static settings apply to the active Ethernet or WiFi interface after restart. */
esp_err_t network_manager_set_ipv4_config(const network_manager_ipv4_config_t *config);

/* Reads the stored IPv4 mode. Missing settings default to DHCP. */
esp_err_t network_manager_get_ipv4_config(network_manager_ipv4_config_t *config);

network_manager_state_t network_manager_get_state(void);

#ifdef __cplusplus
}
#endif
