idf_component_register(
    SRCS
        "main.c"
        "console.c"
        "admin.c"
        "access.c"
        "rfid.c"
        "storage.c"
        "state.c"
        "dashboard.c"
        "energy.c"

    INCLUDE_DIRS "."

    REQUIRES
        esp_common
        driver
        esp_timer
        fatfs
        freertos
        lighting
        log
        nvs_flash
        sdmmc
)