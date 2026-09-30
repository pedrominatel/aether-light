idf_component_register(SRCS "aether-light.c"
                    INCLUDE_DIRS "."
                    REQUIRES firmware_version network_manager sdcard_manager ultraled_manager ddp_manager web_interface)
