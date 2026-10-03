add_library(
    hexlith_core STATIC
    byte_reader.cpp
    byte_reader.h
    result.h
)

add_library(Hexlith::Core ALIAS hexlith_core)

target_include_directories(hexlith_core PUBLIC "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(hexlith_core PRIVATE Hexlith::Warnings)
