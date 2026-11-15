#include "board_hal.h"

#include "bsp/esp32_p4_function_ev_board.h"
#include "driver/gpio.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy_ip101.h"
#include "sdkconfig.h"

static const int s_led_gpios[BOARD_HAL_LED_CHANNEL_COUNT] = {
    CONFIG_AETHER_HAL_P4_LED_GPIO_0,
    CONFIG_AETHER_HAL_P4_LED_GPIO_1,
    CONFIG_AETHER_HAL_P4_LED_GPIO_2,
    CONFIG_AETHER_HAL_P4_LED_GPIO_3,
    CONFIG_AETHER_HAL_P4_LED_GPIO_4,
    CONFIG_AETHER_HAL_P4_LED_GPIO_5,
    CONFIG_AETHER_HAL_P4_LED_GPIO_6,
    CONFIG_AETHER_HAL_P4_LED_GPIO_7,
    CONFIG_AETHER_HAL_P4_LED_GPIO_8,
    CONFIG_AETHER_HAL_P4_LED_GPIO_9,
    CONFIG_AETHER_HAL_P4_LED_GPIO_10,
    CONFIG_AETHER_HAL_P4_LED_GPIO_11,
    CONFIG_AETHER_HAL_P4_LED_GPIO_12,
    CONFIG_AETHER_HAL_P4_LED_GPIO_13,
    CONFIG_AETHER_HAL_P4_LED_GPIO_14,
    CONFIG_AETHER_HAL_P4_LED_GPIO_15,
};

const char *board_hal_get_name(void)
{
    return "ESP32-P4";
}

const int *board_hal_get_led_gpios(size_t *count)
{
    if (count != NULL) {
        *count = BOARD_HAL_LED_CHANNEL_COUNT;
    }
    return s_led_gpios;
}

esp_err_t board_hal_validate_led_gpios(void)
{
    for (size_t i = 0; i < BOARD_HAL_LED_CHANNEL_COUNT; ++i) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(s_led_gpios[i])) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t previous = 0; previous < i; ++previous) {
            if (s_led_gpios[i] == s_led_gpios[previous]) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

esp_err_t board_hal_ethernet_install(esp_eth_handle_t *handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *handle = NULL;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = CONFIG_AETHER_HAL_P4_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = CONFIG_AETHER_HAL_P4_ETH_MDIO_GPIO;
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = CONFIG_AETHER_HAL_P4_ETH_RMII_CLK_GPIO;
    emac_config.emac_dataif_gpio.rmii.tx_en_num = CONFIG_AETHER_HAL_P4_ETH_RMII_TX_EN_GPIO;
    emac_config.emac_dataif_gpio.rmii.txd0_num = CONFIG_AETHER_HAL_P4_ETH_RMII_TXD0_GPIO;
    emac_config.emac_dataif_gpio.rmii.txd1_num = CONFIG_AETHER_HAL_P4_ETH_RMII_TXD1_GPIO;
    emac_config.emac_dataif_gpio.rmii.crs_dv_num = CONFIG_AETHER_HAL_P4_ETH_RMII_CRS_DV_GPIO;
    emac_config.emac_dataif_gpio.rmii.rxd0_num = CONFIG_AETHER_HAL_P4_ETH_RMII_RXD0_GPIO;
    emac_config.emac_dataif_gpio.rmii.rxd1_num = CONFIG_AETHER_HAL_P4_ETH_RMII_RXD1_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (mac == NULL) {
        return ESP_ERR_NO_MEM;
    }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_AETHER_HAL_P4_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_AETHER_HAL_P4_ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    if (phy == NULL) {
        mac->del(mac);
        return ESP_ERR_NO_MEM;
    }

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t error = esp_eth_driver_install(&config, handle);
    if (error != ESP_OK) {
        phy->del(phy);
        mac->del(mac);
    }
    return error;
}

bool board_hal_sdcard_supported(void)
{
    return true;
}

esp_err_t board_hal_sdcard_mount(void)
{
    return bsp_sdcard_mount();
}

void board_hal_sdcard_unmount(void)
{
    bsp_sdcard_unmount();
}

sdmmc_card_t *board_hal_sdcard_get_handle(void)
{
    return bsp_sdcard_get_handle();
}

const char *board_hal_sdcard_mount_path(void)
{
    return BSP_SD_MOUNT_POINT;
}
