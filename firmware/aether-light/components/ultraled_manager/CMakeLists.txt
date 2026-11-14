idf_component_register(
    SRCS "ultraled_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES board_hal ultraled
    PRIV_REQUIRES nvs_flash
)
