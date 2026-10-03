# The synthetic image builder is test infrastructure rather than part of the
# project's own code, so it lives under tests and is never linked into a
# tool. Nothing generated here is written to disk or checked in.
add_library(
    hexlith_test_support STATIC
    crafted_lzss_stream.cpp
    crafted_lzss_stream.h
    memory_workspace.cpp
    memory_workspace.h
    synthetic_cro.cpp
    synthetic_cro.h
    synthetic_ctr_image.cpp
    synthetic_ctr_image.h
    synthetic_romfs.cpp
    synthetic_romfs.h
)

add_library(Hexlith::TestSupport ALIAS hexlith_test_support)

target_include_directories(hexlith_test_support PUBLIC "${PROJECT_SOURCE_DIR}/tests")

target_link_libraries(hexlith_test_support PUBLIC Hexlith::Domain Hexlith::Ports)

target_link_libraries(hexlith_test_support PRIVATE Hexlith::Warnings)

hexlith_add_test(
    hexlith_test_support_test
    crafted_lzss_stream_test.cpp
    synthetic_ctr_image_test.cpp
    synthetic_romfs_test.cpp
)

target_link_libraries(hexlith_test_support_test PRIVATE Hexlith::TestSupport)
