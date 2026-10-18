hexlith_add_test(
    hexlith_adapters_filesystem_test
    cartridge_image_test.cpp
    directory_blob_sink_test.cpp
    directory_tree_store_test.cpp
    gpu_recording_directory_test.cpp
    gpu_recording_files_test.cpp
    file_rom_source_test.cpp
    scripted_input_test.cpp
)

target_link_libraries(
    hexlith_adapters_filesystem_test PRIVATE Hexlith::Adapters Hexlith::TestSupport
)
