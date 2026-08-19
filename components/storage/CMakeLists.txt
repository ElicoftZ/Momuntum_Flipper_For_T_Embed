idf_component_register(
    # Header shim only. The storage service itself is built from
    # applications/services/storage by the fam registry.
    INCLUDE_DIRS "."
    REQUIRES furi furi_hal toolbox
)
