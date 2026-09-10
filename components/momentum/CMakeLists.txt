idf_component_register(
    SRCS
        "../../lib/momentum/settings.c"
        "../../lib/momentum/settings_core.c"
        "../../lib/momentum/png_icon.c"
    INCLUDE_DIRS "../../lib"
    REQUIRES furi furi_hal storage flipper_format esp_rom
)
