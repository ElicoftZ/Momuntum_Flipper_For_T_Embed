idf_component_register(
    # The storage service itself is built from applications/services/storage by
    # the fam registry; this component is the header shim plus the first-boot
    # asset installer, which needs esp_partition to read the `assets` partition.
    SRCS "asset_installer.c"
    INCLUDE_DIRS "."
    REQUIRES furi furi_hal toolbox esp_partition
)
