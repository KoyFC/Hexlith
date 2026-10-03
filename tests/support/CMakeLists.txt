# The synthetic image builder is test infrastructure rather than part of the
# project's own code, so it lives under tests and is never linked into a
# tool. Nothing generated here is written to disk or checked in.
add_library(
    hexlith_test_support STATIC
    synthetic_ctr_image.cpp
    synthetic_ctr_image.h
)

add_library(Hexlith::TestSupport ALIAS hexlith_test_support)

target_include_directories(hexlith_test_support PUBLIC "${PROJECT_SOURCE_DIR}/tests")

target_link_libraries(hexlith_test_support PUBLIC Hexlith::Core)

target_link_libraries(hexlith_test_support PRIVATE Hexlith::Warnings)

hexlith_add_test(hexlith_test_support_test synthetic_ctr_image_test.cpp)

target_link_libraries(hexlith_test_support_test PRIVATE Hexlith::TestSupport)
