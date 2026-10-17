# The ports are interfaces and nothing else, so the target carries no
# sources. Adapters implement them and the use cases depend on them, which is
# what keeps the dependency pointing inward.
add_library(hexlith_ports INTERFACE)

add_library(Hexlith::Ports ALIAS hexlith_ports)

target_link_libraries(hexlith_ports INTERFACE Hexlith::Core Hexlith::Domain)

target_sources(
    hexlith_ports
    INTERFACE FILE_SET HEADERS BASE_DIRS "${PROJECT_SOURCE_DIR}/src" FILES blob_sink.h
                                                                           gpu_recorder.h
                                                                           input_source.h
                                                                           rom_source.h
                                                                           tree_store.h
)
