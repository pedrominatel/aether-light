idf_component_register(SRCS "aether-lighth.c" "http_server.c"
                    INCLUDE_DIRS "."
                    REQUIRES network_manager esp_http_server)
