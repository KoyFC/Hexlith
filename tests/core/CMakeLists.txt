hexlith_add_test(
    hexlith_core_test
    byte_reader_test.cpp
    result_test.cpp
    sha256_test.cpp
)

target_link_libraries(hexlith_core_test PRIVATE Hexlith::Core)
