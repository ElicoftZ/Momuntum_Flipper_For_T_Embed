idf_component_register(
    SRCS
        "hotspot_arcade_service.cpp"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "compat" "vendor/engine"
    REQUIRES hotspot_arcade_runtime
    PRIV_REQUIRES esp_timer esp_system heap freertos log furi storage
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)

