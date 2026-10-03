hexlith_add_test(
    hexlith_domain_profile_test
    profile_test.cpp
)

target_link_libraries(hexlith_domain_profile_test PRIVATE Hexlith::Domain Hexlith::TestSupport)
