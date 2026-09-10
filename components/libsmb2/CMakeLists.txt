# Vendored libsmb2 (SMB2/3 client), https://github.com/sahlberg/libsmb2
# ESP-IDF component wrapper. Uses the upstream ESP config (include/esp/config.h).
# krb5-wrapper.c and aes_apple.c are intentionally removed (external deps).

file(GLOB LIBSMB2_SRCS "lib/*.c")

idf_component_register(
    SRCS ${LIBSMB2_SRCS}
    INCLUDE_DIRS "include" "include/smb2"
    PRIV_INCLUDE_DIRS "lib" "include/esp"
    REQUIRES lwip
)

# Match upstream ESP_PLATFORM branch (root CMakeLists.txt).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAVE_CONFIG_H
    NEED_READV
    NEED_WRITEV
    NEED_GETLOGIN_R
    NEED_RANDOM
    NEED_SRANDOM
)

# libsmb2 is third-party; silence its warnings so -Werror builds stay clean.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
