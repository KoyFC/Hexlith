add_library(
    hexlith_core STATIC
    byte_reader.cpp
    byte_reader.h
    key_value_text.cpp
    key_value_text.h
    result.h
    sha256.cpp
    sha256.h
    text.cpp
    text.h
)

add_library(Hexlith::Core ALIAS hexlith_core)

target_include_directories(hexlith_core PUBLIC "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(hexlith_core PRIVATE Hexlith::Warnings)

# libcrypto is linked privately: the SHA-256 wrapper keeps every OpenSSL
# header inside its own translation unit, so nothing that uses core sees the
# dependency. ADR 0009 records why it is here at all.
target_link_libraries(hexlith_core PRIVATE OpenSSL::Crypto)
