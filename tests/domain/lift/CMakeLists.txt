hexlith_add_test(hexlith_domain_lift_test control_flow_test.cpp differential.cpp differential.h
                 lower_test.cpp random_test.cpp sample_native_test.cpp)

target_link_libraries(hexlith_domain_lift_test PRIVATE Hexlith::Domain Hexlith::TestSupport)

# C++ for a few sample functions, written by the emitter at build time and built
# into the test that runs it against the interpreter. The generated code is not
# checked in, and is not held to the warning policy: it is long, repetitive and
# written by a program.
add_executable(hexlith_lift_sample_generator sample_generator.cpp)
target_link_libraries(hexlith_lift_sample_generator PRIVATE Hexlith::Domain Hexlith::Warnings)

set(sample_lifted ${CMAKE_CURRENT_BINARY_DIR}/sample_lifted.cpp)
add_custom_command(
    OUTPUT ${sample_lifted}
    COMMAND hexlith_lift_sample_generator ${sample_lifted}
    DEPENDS hexlith_lift_sample_generator
    COMMENT "Generating lifted code for the samples")
set_source_files_properties(${sample_lifted} PROPERTIES COMPILE_OPTIONS "-w")
target_sources(hexlith_domain_lift_test PRIVATE ${sample_lifted})
