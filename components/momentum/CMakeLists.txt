idf_component_register(
    SRCS
        "../../lib/momentum/namespoof.c"
        "../../lib/momentum/settings.c"
        "../../lib/momentum/settings_core.c"
    INCLUDE_DIRS "../../lib"
    REQUIRES furi furi_hal storage flipper_format
)
