idf_component_register(
    SRCS "ddp_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES ddp network_manager ultraled_manager
    PRIV_REQUIRES nvs_flash esp_app_format esp_hw_support
)
