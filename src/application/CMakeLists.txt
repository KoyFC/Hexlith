add_library(
    hexlith_application STATIC
    analysis_tables.cpp
    analysis_tables.h
    analyze.cpp
    analyze.h
    analyze_modules.cpp
    analyze_modules.h
    boot.cpp
    boot.h
    command_line.cpp
    command_line.h
    extract.cpp
    extract.h
    golden_trace.cpp
    golden_trace.h
    gpu_recording.cpp
    gpu_recording.h
    gpu_render.cpp
    gpu_render.h
    gpu_replay.cpp
    gpu_replay.h
    input_script.cpp
    input_script.h
    manifest.cpp
    manifest.h
    save_storage.cpp
    save_storage.h
    workspace.cpp
    workspace.h
)

add_library(Hexlith::Application ALIAS hexlith_application)

target_link_libraries(hexlith_application PUBLIC Hexlith::Core Hexlith::Domain Hexlith::Ports)

target_link_libraries(hexlith_application PRIVATE Hexlith::Warnings)
