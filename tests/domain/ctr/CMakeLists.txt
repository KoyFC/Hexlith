hexlith_add_test(
    hexlith_domain_ctr_test
    exheader_test.cpp
    ncch_test.cpp
    ncsd_test.cpp
)

target_link_libraries(hexlith_domain_ctr_test PRIVATE Hexlith::Domain Hexlith::TestSupport)
