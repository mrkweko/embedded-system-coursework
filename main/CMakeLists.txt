idf_component_register(
    SRCS
        "main.c"
        "console.c"
        "admin.c"
        "access.c"
        "rfid.c"
        "storage.c"
        "state.c"
    INCLUDE_DIRS "."
    REQUIRES driver fatfs sdmmc nvs_flash
)