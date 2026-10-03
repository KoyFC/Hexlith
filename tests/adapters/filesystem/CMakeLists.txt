hexlith_add_test(
    hexlith_adapters_filesystem_test
    directory_blob_sink_test.cpp
    file_rom_source_test.cpp
)

target_link_libraries(
    hexlith_adapters_filesystem_test PRIVATE Hexlith::Adapters Hexlith::TestSupport
)
