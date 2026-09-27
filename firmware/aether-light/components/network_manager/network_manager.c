#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_phy_ip101.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "network_manager.h"

static const char *TAG = "network_manager";

#define NVS_NAMESPACE     "netmgr"
#define NVS_KEY_WIFI_SSID "ssid"
#define NVS_KEY_WIFI_PASS "pass"
#define NVS_KEY_STATIC_IP "static_ip"
#define NVS_KEY_IP_ADDR   "ip_addr"
#define NVS_KEY_GATEWAY   "gateway"
#define NVS_KEY_NETMASK   "netmask"
#define NVS_KEY_DNS       "dns"

#define ETH_CONNECTED_BIT  (1 << 0)
#define WIFI_CONNECTED_BIT (1 << 1)

static EventGroupHandle_t s_network_event_group;
static esp_timer_handle_t s_eth_fallback_timer;
static esp_eth_handle_t s_eth_handle;
static esp_netif_t *s_eth_netif;
static esp_netif_t *s_wifi_netif;
static SemaphoreHandle_t s_wifi_mutex;
static volatile network_manager_state_t s_state = NETWORK_MANAGER_DISCONNECTED;
static bool s_wifi_initialized = false;
static bool s_wifi_handlers_registered = false;
static bool s_wifi_started = false;
static int s_wifi_retry_count = 0;

static void start_wifi_fallback(void);

static esp_err_t ensure_wifi_initialized(void)
{
    if (s_wifi_initialized) {
        return ESP_OK;
    }

    s_wifi_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&config);
    bool driver_initialized = err == ESP_OK;
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(err));
        if (driver_initialized) {
            esp_wifi_deinit();
        }
        esp_netif_destroy_default_wifi(s_wifi_netif);
        s_wifi_netif = NULL;
        return err;
    }

    s_wifi_initialized = true;
    return ESP_OK;
}

static bool netmask_is_contiguous(uint32_t netmask)
{
    uint32_t host_order = __builtin_bswap32(netmask);
    uint32_t inverted = ~host_order;
    return inverted != UINT32_MAX && (inverted & (inverted + 1U)) == 0;
}

static esp_err_t parse_ipv4_config(const network_manager_ipv4_config_t *config,
                                   esp_netif_ip_info_t *ip_info, esp_netif_dns_info_t *dns_info)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->use_static_ip) {
        return ESP_OK;
    }

    esp_netif_ip_info_t parsed = {0};
    esp_netif_dns_info_t parsed_dns = {0};
    if (esp_netif_str_to_ip4(config->ip, &parsed.ip) != ESP_OK ||
        esp_netif_str_to_ip4(config->gateway, &parsed.gw) != ESP_OK ||
        esp_netif_str_to_ip4(config->netmask, &parsed.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(config->dns, &parsed_dns.ip.u_addr.ip4) != ESP_OK ||
        parsed.ip.addr == 0 || parsed.gw.addr == 0 || parsed_dns.ip.u_addr.ip4.addr == 0 ||
        !netmask_is_contiguous(parsed.netmask.addr) ||
        (parsed.ip.addr & parsed.netmask.addr) != (parsed.gw.addr & parsed.netmask.addr)) {
        return ESP_ERR_INVALID_ARG;
    }
    parsed_dns.ip.type = ESP_IPADDR_TYPE_V4;

    if (ip_info != NULL) {
        *ip_info = parsed;
    }
    if (dns_info != NULL) {
        *dns_info = parsed_dns;
    }
    return ESP_OK;
}

static esp_err_t apply_ipv4_config(esp_netif_t *netif)
{
    network_manager_ipv4_config_t config;
    esp_err_t err = network_manager_get_ipv4_config(&config);
    if (err != ESP_OK || !config.use_static_ip) {
        return err;
    }

    esp_netif_ip_info_t ip_info;
    esp_netif_dns_info_t dns_info;
    err = parse_ipv4_config(&config, &ip_info, &dns_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stored static IPv4 configuration is invalid; using DHCP");
        return err;
    }

    err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "Failed to stop DHCP client: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_netif_set_ip_info(netif, &ip_info);
    if (err == ESP_OK) {
        err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Using static IPv4 address %s", config.ip);
    } else {
        esp_netif_dhcpc_start(netif);
    }
    return err;
}

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
        if (apply_ipv4_config(s_eth_netif) != ESP_OK) {
            ESP_LOGW(TAG, "Could not apply static IPv4 settings to Ethernet; using DHCP");
        }
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet link down");
        start_wifi_fallback();
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

static void eth_got_ip_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Ethernet got IP:" IPSTR, IP2STR(&event->ip_info.ip));

    if (s_eth_fallback_timer) {
        esp_timer_stop(s_eth_fallback_timer);
    }
    s_state = NETWORK_MANAGER_CONNECTED_ETH;
    xEventGroupSetBits(s_network_event_group, ETH_CONNECTED_BIT);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        if (apply_ipv4_config(s_wifi_netif) != ESP_OK) {
            ESP_LOGW(TAG, "Could not apply static IPv4 settings to WiFi; using DHCP");
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_count < CONFIG_NETWORK_MANAGER_WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_wifi_retry_count++;
            ESP_LOGW(TAG, "Retrying WiFi connection (%d/%d)", s_wifi_retry_count, CONFIG_NETWORK_MANAGER_WIFI_MAX_RETRY);
        } else {
            ESP_LOGE(TAG, "WiFi connection failed, no network available");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_count = 0;
        s_state = NETWORK_MANAGER_CONNECTED_WIFI;
        xEventGroupSetBits(s_network_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t load_wifi_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = ssid_len;
    err = nvs_get_str(handle, NVS_KEY_WIFI_SSID, ssid, &len);
    if (err == ESP_OK) {
        len = pass_len;
        err = nvs_get_str(handle, NVS_KEY_WIFI_PASS, password, &len);
    }
    nvs_close(handle);
    return err;
}

esp_err_t network_manager_set_wifi_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_WIFI_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_WIFI_PASS, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t network_manager_get_wifi_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return load_wifi_credentials(ssid, ssid_len, password, pass_len);
}

static int compare_wifi_ap_rssi(const void *left, const void *right)
{
    const wifi_ap_record_t *left_ap = left;
    const wifi_ap_record_t *right_ap = right;
    return (int)right_ap->rssi - (int)left_ap->rssi;
}

esp_err_t network_manager_scan_wifi(network_manager_wifi_ap_t *results, size_t max_results, size_t *result_count)
{
    if (results == NULL || result_count == NULL || max_results == 0 ||
        max_results > NETWORK_MANAGER_MAX_SCAN_RESULTS || s_wifi_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *result_count = 0;

    if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ensure_wifi_initialized();
    bool temporary_start = false;
    if (err == ESP_OK && !s_wifi_started) {
        err = esp_wifi_start();
        temporary_start = err == ESP_OK;
    }

    wifi_ap_record_t *records = NULL;
    if (err == ESP_OK) {
        records = calloc(max_results, sizeof(*records));
        if (records == NULL) {
            err = ESP_ERR_NO_MEM;
        }
    }

    bool scan_completed = false;
    if (err == ESP_OK) {
        wifi_scan_config_t scan_config = {
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        };
        err = esp_wifi_scan_start(&scan_config, true);
        scan_completed = err == ESP_OK;
    }

    uint16_t record_count = (uint16_t)max_results;
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&record_count, records);
    }
    if (scan_completed && err != ESP_OK) {
        esp_wifi_clear_ap_list();
    }
    if (err == ESP_OK) {
        qsort(records, record_count, sizeof(*records), compare_wifi_ap_rssi);
        for (uint16_t index = 0; index < record_count; ++index) {
            network_manager_wifi_ap_t *result = &results[*result_count];
            strlcpy(result->ssid, (const char *)records[index].ssid, sizeof(result->ssid));
            result->rssi = records[index].rssi;
            result->channel = records[index].primary;
            result->secured = records[index].authmode != WIFI_AUTH_OPEN;
            (*result_count)++;
        }
    }

    free(records);
    if (temporary_start) {
        esp_err_t stop_err = esp_wifi_stop();
        if (err == ESP_OK && stop_err != ESP_OK) {
            err = stop_err;
        }
    }
    xSemaphoreGive(s_wifi_mutex);
    return err;
}

esp_err_t network_manager_set_ipv4_config(const network_manager_ipv4_config_t *config)
{
    esp_err_t err = parse_ipv4_config(config, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, NVS_KEY_STATIC_IP, config->use_static_ip ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_IP_ADDR, config->ip);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_GATEWAY, config->gateway);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_NETMASK, config->netmask);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_DNS, config->dns);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t network_manager_get_ipv4_config(network_manager_ipv4_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(config, 0, sizeof(*config));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t use_static_ip = 0;
    err = nvs_get_u8(handle, NVS_KEY_STATIC_IP, &use_static_ip);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    config->use_static_ip = use_static_ip != 0;

    size_t length = sizeof(config->ip);
    err = nvs_get_str(handle, NVS_KEY_IP_ADDR, config->ip, &length);
    if (err == ESP_OK) {
        length = sizeof(config->gateway);
        err = nvs_get_str(handle, NVS_KEY_GATEWAY, config->gateway, &length);
    }
    if (err == ESP_OK) {
        length = sizeof(config->netmask);
        err = nvs_get_str(handle, NVS_KEY_NETMASK, config->netmask, &length);
    }
    if (err == ESP_OK) {
        length = sizeof(config->dns);
        err = nvs_get_str(handle, NVS_KEY_DNS, config->dns, &length);
    }
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND && !config->use_static_ip) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    return parse_ipv4_config(config, NULL, NULL);
}

static void start_wifi_fallback(void)
{
    if (s_wifi_mutex == NULL || xSemaphoreTake(s_wifi_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_wifi_started) {
        xSemaphoreGive(s_wifi_mutex);
        return;
    }

    char ssid[33] = {0};
    char password[65] = {0};
    if (load_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password)) != ESP_OK) {
        if (strlen(CONFIG_NETWORK_MANAGER_WIFI_DEFAULT_SSID) == 0) {
            ESP_LOGE(TAG, "No WiFi credentials stored in NVS, cannot fall back to WiFi");
            xSemaphoreGive(s_wifi_mutex);
            return;
        }
        strlcpy(ssid, CONFIG_NETWORK_MANAGER_WIFI_DEFAULT_SSID, sizeof(ssid));
        strlcpy(password, CONFIG_NETWORK_MANAGER_WIFI_DEFAULT_PASSWORD, sizeof(password));
        network_manager_set_wifi_credentials(ssid, password);
    }

    ESP_LOGI(TAG, "Falling back to WiFi, connecting to SSID '%s'", ssid);
    s_state = NETWORK_MANAGER_CONNECTING;

    esp_err_t err = ensure_wifi_initialized();
    if (err != ESP_OK) {
        xSemaphoreGive(s_wifi_mutex);
        return;
    }

    if (!s_wifi_handlers_registered) {
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
        if (err == ESP_OK) {
            err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register WiFi event handlers: %s", esp_err_to_name(err));
            xSemaphoreGive(s_wifi_mutex);
            return;
        }
        s_wifi_handlers_registered = true;
    }

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = strlen(password) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err == ESP_OK) {
        s_wifi_started = true;
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        s_wifi_started = false;
        ESP_LOGE(TAG, "Failed to start WiFi fallback: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_wifi_mutex);
}

static void eth_fallback_timer_cb(void *arg)
{
    if (!(xEventGroupGetBits(s_network_event_group) & ETH_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "Ethernet did not connect within %d ms, falling back to WiFi", CONFIG_NETWORK_MANAGER_ETH_CONNECT_TIMEOUT_MS);
        start_wifi_fallback();
    }
}

/* ESP32-P4-Function-EV-Board: internal EMAC + IP101 PHY over RMII */
static esp_err_t eth_init(esp_eth_handle_t *eth_handle_out)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.smi_gpio.mdc_num = CONFIG_NETWORK_MANAGER_ETH_MDC_GPIO;
    esp32_emac_config.smi_gpio.mdio_num = CONFIG_NETWORK_MANAGER_ETH_MDIO_GPIO;
    esp32_emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    esp32_emac_config.clock_config.rmii.clock_gpio = CONFIG_NETWORK_MANAGER_ETH_RMII_CLK_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.tx_en_num = CONFIG_NETWORK_MANAGER_ETH_RMII_TX_EN_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.txd0_num = CONFIG_NETWORK_MANAGER_ETH_RMII_TXD0_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.txd1_num = CONFIG_NETWORK_MANAGER_ETH_RMII_TXD1_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.crs_dv_num = CONFIG_NETWORK_MANAGER_ETH_RMII_CRS_DV_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.rxd0_num = CONFIG_NETWORK_MANAGER_ETH_RMII_RXD0_GPIO;
    esp32_emac_config.emac_dataif_gpio.rmii.rxd1_num = CONFIG_NETWORK_MANAGER_ETH_RMII_RXD1_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    if (mac == NULL) {
        ESP_LOGE(TAG, "create MAC instance failed");
        return ESP_FAIL;
    }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_NETWORK_MANAGER_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_NETWORK_MANAGER_ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "create PHY instance failed");
        mac->del(mac);
        return ESP_FAIL;
    }

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    if (esp_eth_driver_install(&config, &eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed");
        return ESP_FAIL;
    }

    *eth_handle_out = eth_handle;
    return ESP_OK;
}

esp_err_t network_manager_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_network_event_group = xEventGroupCreate();
    s_wifi_mutex = xSemaphoreCreateMutex();
    return s_network_event_group != NULL && s_wifi_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t network_manager_start(void)
{
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_config);
    if (s_eth_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_state = NETWORK_MANAGER_CONNECTING;

    if (eth_init(&s_eth_handle) != ESP_OK) {
        ESP_LOGW(TAG, "Ethernet init failed, falling back to WiFi");
        start_wifi_fallback();
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_got_ip_handler, NULL));

    const esp_timer_create_args_t timer_args = {
        .callback = &eth_fallback_timer_cb,
        .name = "eth_fallback",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_eth_fallback_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(s_eth_fallback_timer,
                                          (uint64_t)CONFIG_NETWORK_MANAGER_ETH_CONNECT_TIMEOUT_MS * 1000));

    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));
    return ESP_OK;
}

network_manager_state_t network_manager_get_state(void)
{
    return s_state;
}
