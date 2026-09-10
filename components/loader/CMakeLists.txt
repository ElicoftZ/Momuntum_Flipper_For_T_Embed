idf_component_register(
    SRCS
        "loader.c"
        "loader_queue.c"
        "loader_menu.c"
        "loader_applications.c"
        "menu_custom.c"
        "apps/about_app.c"
    INCLUDE_DIRS "."
    # ".." puts components/ on the path so loader_applications.c can reach
    # <dolphin/dolphin.h>, a shim forwarding into applications/. Same trick as
    # components/gui uses for the PS4 menu style.
    PRIV_INCLUDE_DIRS ".."
    REQUIRES furi furi_hal gui toolbox assets storage archive dialogs flipper_application
)
