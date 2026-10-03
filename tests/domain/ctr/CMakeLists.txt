hexlith_add_test(hexlith_domain_ctr_test ncsd_test.cpp)

target_link_libraries(hexlith_domain_ctr_test PRIVATE Hexlith::Domain Hexlith::TestSupport)
