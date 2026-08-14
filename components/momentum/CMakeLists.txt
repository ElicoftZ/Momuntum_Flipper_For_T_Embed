idf_component_register(
    SRCS
        "../../lib/momentum/settings.c"
        "../../lib/momentum/settings_core.c"
    INCLUDE_DIRS "../../lib"
    REQUIRES furi storage flipper_format
)
