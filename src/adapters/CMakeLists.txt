# Adapters implement the ports. Nothing in src/application or below may
# depend on anything here, which is what the layering is for.
add_library(hexlith_adapters STATIC)

add_library(Hexlith::Adapters ALIAS hexlith_adapters)

target_link_libraries(hexlith_adapters PUBLIC Hexlith::Application Hexlith::Ports)

target_link_libraries(hexlith_adapters PRIVATE Hexlith::Warnings)

add_subdirectory(filesystem)
