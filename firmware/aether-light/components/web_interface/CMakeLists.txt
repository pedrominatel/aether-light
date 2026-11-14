idf_component_register(
    SRCS "web_interface.c"
    INCLUDE_DIRS "include"
    REQUIRES board_hal firmware_version network_manager sdcard_manager ultraled_manager ddp_manager esp_http_server esp_system esp_timer heap
    EMBED_FILES "logo.jpg"
)
