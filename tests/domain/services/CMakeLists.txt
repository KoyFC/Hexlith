hexlith_add_test(
    hexlith_domain_services_test
    file_system_test.cpp
    service_harness.cpp
    service_harness.h
    service_manager_test.cpp
)

target_link_libraries(hexlith_domain_services_test PRIVATE Hexlith::Domain)
