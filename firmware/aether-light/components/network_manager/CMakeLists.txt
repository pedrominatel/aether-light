idf_component_register(SRCS "network_manager.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_netif esp_event esp_eth esp_wifi nvs_flash esp_timer)
