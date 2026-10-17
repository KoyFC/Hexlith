hexlith_add_test(
    hexlith_application_test
    analyze_modules_test.cpp
    analyze_test.cpp
    boot_test.cpp
    command_line_test.cpp
    extract_test.cpp
    golden_trace_test.cpp
    input_script_test.cpp
    manifest_test.cpp
    save_storage_test.cpp
    workspace_test.cpp
)

target_link_libraries(hexlith_application_test PRIVATE Hexlith::Application Hexlith::TestSupport)
