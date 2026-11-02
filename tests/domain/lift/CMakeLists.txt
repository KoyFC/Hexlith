hexlith_add_test(hexlith_domain_lift_test control_flow_test.cpp differential.cpp differential.h
                 lower_test.cpp random_test.cpp)

target_link_libraries(hexlith_domain_lift_test PRIVATE Hexlith::Domain Hexlith::TestSupport)
