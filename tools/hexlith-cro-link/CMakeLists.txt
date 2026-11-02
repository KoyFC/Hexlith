add_executable(hexlith-cro-link main.cpp)

target_link_libraries(hexlith-cro-link PRIVATE Hexlith::ToolMain Hexlith::Domain Hexlith::Warnings)
