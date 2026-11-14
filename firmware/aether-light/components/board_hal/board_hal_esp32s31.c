#include "board_hal.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_eth_mac_esp.h"
#include "sdkconfig.h"

static const int s_led_gpios[BOARD_HAL_LED_CHANNEL_COUNT] = {
    CONFIG_AETHER_HAL_S31_LED_GPIO_0,
    CONFIG_AETHER_HAL_S31_LED_GPIO_1,
    CONFIG_AETHER_HAL_S31_LED_GPIO_2,
    CONFIG_AETHER_HAL_S31_LED_GPIO_3,
    CONFIG_AETHER_HAL_S31_LED_GPIO_4,
    CONFIG_AETHER_HAL_S31_LED_GPIO_5,
    CONFIG_AETHER_HAL_S31_LED_GPIO_6,
    CONFIG_AETHER_HAL_S31_LED_GPIO_7,
    CONFIG_AETHER_HAL_S31_LED_GPIO_8,
    CONFIG_AETHER_HAL_S31_LED_GPIO_9,
    CONFIG_AETHER_HAL_S31_LED_GPIO_10,
    CONFIG_AETHER_HAL_S31_LED_GPIO_11,
    CONFIG_AETHER_HAL_S31_LED_GPIO_12,
    CONFIG_AETHER_HAL_S31_LED_GPIO_13,
    CONFIG_AETHER_HAL_S31_LED_GPIO_14,
    CONFIG_AETHER_HAL_S31_LED_GPIO_15,
};

const char *board_hal_get_name(void)
{
    return "ESP32-S31";
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

static esp_err_t write_extended_phy_register(esp_eth_handle_t handle, uint32_t address,
                                              uint32_t clear_mask, uint32_t set_mask)
{
    uint32_t value = address;
    esp_eth_phy_reg_rw_data_t register_data = {
        .reg_addr = 0x1e,
        .reg_value_p = &value,
    };
    esp_err_t error = esp_eth_ioctl(handle, ETH_CMD_WRITE_PHY_REG, &register_data);
    if (error != ESP_OK) {
        return error;
    }

    register_data.reg_addr = 0x1f;
    error = esp_eth_ioctl(handle, ETH_CMD_READ_PHY_REG, &register_data);
    if (error != ESP_OK) {
        return error;
    }
    value = (value & ~clear_mask) | set_mask;
    return esp_eth_ioctl(handle, ETH_CMD_WRITE_PHY_REG, &register_data);
}

static esp_err_t initialize_yt8531(esp_eth_handle_t handle)
{
    bool auto_negotiation = true;
    esp_err_t error = esp_eth_ioctl(handle, ETH_CMD_S_AUTONEGO, &auto_negotiation);
    if (error != ESP_OK) {
        return error;
    }

    error = write_extended_phy_register(handle, 0xa001, 0, 1U << 8);
    if (error != ESP_OK) {
        return error;
    }
    return write_extended_phy_register(handle, 0xa003, 0xffU, (13U << 4) | 13U);
}

esp_err_t board_hal_ethernet_install(esp_eth_handle_t *handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *handle = NULL;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = CONFIG_AETHER_HAL_S31_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = CONFIG_AETHER_HAL_S31_ETH_MDIO_GPIO;
    emac_config.interface = EMAC_DATA_INTERFACE_RGMII;
    emac_config.clock_config.rgmii.clock_rx_gpio = CONFIG_AETHER_HAL_S31_ETH_RGMII_RX_CLK_GPIO;
    emac_config.clock_config.rgmii.clock_tx_gpio = CONFIG_AETHER_HAL_S31_ETH_RGMII_TX_CLK_GPIO;
    emac_config.clock_config.rgmii.clock_phy_ref_gpio = -1;
    emac_config.emac_dataif_gpio.rgmii.tx_ctl_num = CONFIG_AETHER_HAL_S31_ETH_RGMII_TX_CTL_GPIO;
    emac_config.emac_dataif_gpio.rgmii.txd0_num = CONFIG_AETHER_HAL_S31_ETH_RGMII_TXD0_GPIO;
    emac_config.emac_dataif_gpio.rgmii.txd1_num = CONFIG_AETHER_HAL_S31_ETH_RGMII_TXD1_GPIO;
    emac_config.emac_dataif_gpio.rgmii.txd2_num = CONFIG_AETHER_HAL_S31_ETH_RGMII_TXD2_GPIO;
    emac_config.emac_dataif_gpio.rgmii.txd3_num = CONFIG_AETHER_HAL_S31_ETH_RGMII_TXD3_GPIO;
    emac_config.emac_dataif_gpio.rgmii.rx_ctl_num = CONFIG_AETHER_HAL_S31_ETH_RGMII_RX_CTL_GPIO;
    emac_config.emac_dataif_gpio.rgmii.rxd0_num = CONFIG_AETHER_HAL_S31_ETH_RGMII_RXD0_GPIO;
    emac_config.emac_dataif_gpio.rgmii.rxd1_num = CONFIG_AETHER_HAL_S31_ETH_RGMII_RXD1_GPIO;
    emac_config.emac_dataif_gpio.rgmii.rxd2_num = CONFIG_AETHER_HAL_S31_ETH_RGMII_RXD2_GPIO;
    emac_config.emac_dataif_gpio.rgmii.rxd3_num = CONFIG_AETHER_HAL_S31_ETH_RGMII_RXD3_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (mac == NULL) {
        return ESP_ERR_NO_MEM;
    }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_AETHER_HAL_S31_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_AETHER_HAL_S31_ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (phy == NULL) {
        mac->del(mac);
        return ESP_ERR_NO_MEM;
    }

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t error = esp_eth_driver_install(&config, handle);
    if (error != ESP_OK) {
        phy->del(phy);
        mac->del(mac);
        return error;
    }

    error = initialize_yt8531(*handle);
    if (error != ESP_OK) {
        esp_eth_driver_uninstall(*handle);
        *handle = NULL;
        phy->del(phy);
        mac->del(mac);
    }
    return error;
}

bool board_hal_sdcard_supported(void)
{
    return false;
}

esp_err_t board_hal_sdcard_mount(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void board_hal_sdcard_unmount(void)
{
}

sdmmc_card_t *board_hal_sdcard_get_handle(void)
{
    return NULL;
}

const char *board_hal_sdcard_mount_path(void)
{
    return "/sdcard";
}
