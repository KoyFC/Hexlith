hexlith_add_test(
    hexlith_application_test
    extract_test.cpp
    manifest_test.cpp
    workspace_test.cpp
)

target_link_libraries(hexlith_application_test PRIVATE Hexlith::Application Hexlith::TestSupport)
