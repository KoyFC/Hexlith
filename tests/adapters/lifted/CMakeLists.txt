hexlith_add_test(hexlith_adapters_lifted_test lifted_library_test.cpp modules_library_test.cpp)

target_link_libraries(hexlith_adapters_lifted_test PRIVATE Hexlith::Adapters Hexlith::TestSupport)

# The library calls back into the test binary for the domain's code, as it does
# into hexlith-boot.
set_target_properties(hexlith_adapters_lifted_test PROPERTIES ENABLE_EXPORTS ON)

target_include_directories(hexlith_adapters_lifted_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../domain/lift)
target_compile_definitions(
    hexlith_adapters_lifted_test
    PRIVATE HEXLITH_SAMPLE_LIBRARY="$<TARGET_FILE:hexlith_lift_sample_library>"
            HEXLITH_STALE_MODULES_LIBRARY="$<TARGET_FILE:hexlith_lift_stale_modules_library>")
add_dependencies(hexlith_adapters_lifted_test hexlith_lift_sample_library
                 hexlith_lift_stale_modules_library)
