idf_component_register(SRCS "recovery.c" "../../components/multiboot/layout.c"
    INCLUDE_DIRS "../../components/multiboot" REQUIRES bootloader_support spi_flash)
