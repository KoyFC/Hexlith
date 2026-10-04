add_library(
    hexlith_application STATIC
    analysis_tables.cpp
    analysis_tables.h
    analyze.cpp
    analyze.h
    analyze_modules.cpp
    analyze_modules.h
    command_line.cpp
    command_line.h
    extract.cpp
    extract.h
    manifest.cpp
    manifest.h
    workspace.cpp
    workspace.h
)

add_library(Hexlith::Application ALIAS hexlith_application)

target_link_libraries(hexlith_application PUBLIC Hexlith::Core Hexlith::Domain Hexlith::Ports)

target_link_libraries(hexlith_application PRIVATE Hexlith::Warnings)
