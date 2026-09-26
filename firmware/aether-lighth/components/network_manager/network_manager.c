#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stddef.h>
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
#include "network_manager.h"

static const char *TAG = "network_manager";

#define NVS_NAMESPACE     "netmgr"
#define NVS_KEY_WIFI_SSID "ssid"
#define NVS_KEY_WIFI_PASS "pass"

#define ETH_CONNECTED_BIT  (1 << 0)
#define WIFI_CONNECTED_BIT (1 << 1)

static EventGroupHandle_t s_network_event_group;
static esp_timer_handle_t s_eth_fallback_timer;
static esp_eth_handle_t s_eth_handle;
static volatile network_manager_state_t s_state = NETWORK_MANAGER_DISCONNECTED;
static bool s_wifi_started = false;
static int s_wifi_retry_count = 0;

static void start_wifi_fallback(void);

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
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

static void start_wifi_fallback(void)
{
    if (s_wifi_started) {
        return;
    }
    s_wifi_started = true;

    char ssid[33] = {0};
    char password[65] = {0};
    if (load_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password)) != ESP_OK) {
        if (strlen(CONFIG_NETWORK_MANAGER_WIFI_DEFAULT_SSID) == 0) {
            ESP_LOGE(TAG, "No WiFi credentials stored in NVS, cannot fall back to WiFi");
            s_wifi_started = false;
            return;
        }
        strlcpy(ssid, CONFIG_NETWORK_MANAGER_WIFI_DEFAULT_SSID, sizeof(ssid));
        strlcpy(password, CONFIG_NETWORK_MANAGER_WIFI_DEFAULT_PASSWORD, sizeof(password));
        network_manager_set_wifi_credentials(ssid, password);
    }

    ESP_LOGI(TAG, "Falling back to WiFi, connecting to SSID '%s'", ssid);
    s_state = NETWORK_MANAGER_CONNECTING;

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = strlen(password) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
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
    return ESP_OK;
}

esp_err_t network_manager_start(void)
{
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_config);

    s_state = NETWORK_MANAGER_CONNECTING;

    if (eth_init(&s_eth_handle) != ESP_OK) {
        ESP_LOGW(TAG, "Ethernet init failed, falling back to WiFi");
        start_wifi_fallback();
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(s_eth_handle)));
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
