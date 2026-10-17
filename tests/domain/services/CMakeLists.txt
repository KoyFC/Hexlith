hexlith_add_test(
    hexlith_domain_services_test
    apt_test.cpp
    configuration_test.cpp
    digital_signal_processor_test.cpp
    file_system_test.cpp
    friends_test.cpp
    graphics_test.cpp
    human_interface_test.cpp
    network_daemons_test.cpp
    power_manager_test.cpp
    relocatable_objects_test.cpp
    save_data_test.cpp
    service_harness.cpp
    service_harness.h
    service_manager_test.cpp
    stand_in_font_test.cpp
)

target_link_libraries(hexlith_domain_services_test PRIVATE Hexlith::Domain Hexlith::TestSupport)
