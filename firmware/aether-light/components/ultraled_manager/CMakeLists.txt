idf_component_register(
    SRCS "ultraled_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES ultraled
    PRIV_REQUIRES nvs_flash esp_driver_gpio
)
