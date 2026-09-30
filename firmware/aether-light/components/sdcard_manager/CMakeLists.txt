idf_component_register(
    SRCS "sdcard_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp32_p4_function_ev_board fatfs
)
