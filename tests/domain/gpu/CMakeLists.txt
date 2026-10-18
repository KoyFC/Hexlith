hexlith_add_test(
    hexlith_domain_gpu_test
    command_processor_test.cpp
    float24_test.cpp
    fragment_stage_test.cpp
    gpu_memory_test.cpp
    rasterizer_test.cpp
    recording_index_test.cpp
    reference_renderer_test.cpp
    shader_interpreter_test.cpp
    pixel_formats_test.cpp
    vertex_stage_test.cpp
)

target_link_libraries(hexlith_domain_gpu_test PRIVATE Hexlith::Domain)
