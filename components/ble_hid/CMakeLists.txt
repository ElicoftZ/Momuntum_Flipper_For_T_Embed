idf_component_register(
    SRCS
        "ble_hid.c"
    INCLUDE_DIRS "."
    REQUIRES bt nimble_glue
    PRIV_REQUIRES nvs_flash
)
