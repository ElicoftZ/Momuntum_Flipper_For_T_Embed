idf_component_register(SRCS "multiboot.c" "layout.c" "launcher_bridge.c" INCLUDE_DIRS "."
    REQUIRES furi storage app_update esp_partition bootloader_support spi_flash esp_rom
             esp_hw_support nvs_flash)
