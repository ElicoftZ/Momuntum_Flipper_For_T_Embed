idf_component_register(
    SRCS
        "hotspot_arcade_runtime.c"
    INCLUDE_DIRS "."
    REQUIRES
        furi
        storage
        btshim
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        lwip
    WHOLE_ARCHIVE
)
