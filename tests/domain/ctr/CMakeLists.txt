hexlith_add_test(
    hexlith_domain_ctr_test
    backwards_lzss_test.cpp
    exefs_test.cpp
    exheader_test.cpp
    ncch_test.cpp
    ncsd_test.cpp
)

target_link_libraries(hexlith_domain_ctr_test PRIVATE Hexlith::Domain Hexlith::TestSupport)
