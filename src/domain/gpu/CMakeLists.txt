target_sources(
    hexlith_domain
    PRIVATE command_processor.h
            float24.h
            gpu_memory.cpp
            gpu_memory.h
            pica_registers.h
            pica_state.cpp
            pica_state.h
            recording_index.cpp
            recording_index.h
            shader_arithmetic.h
            shader_interpreter.cpp
            shader_interpreter.h
            pixel_formats.cpp
            pixel_formats.h
)
