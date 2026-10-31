idf_component_register(
    SRCS
        "main.c"
        "console.c"
        "admin.c"
        "access.c"
        "rfid.c"
    INCLUDE_DIRS "."
    REQUIRES lighting esp_driver_gpio esp_driver_spi esp_driver_uart esp_timer log
)