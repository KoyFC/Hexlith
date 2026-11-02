add_executable(hexlith-analyze main.cpp)

target_link_libraries(hexlith-analyze PRIVATE Hexlith::ToolMain Hexlith::Adapters Hexlith::Warnings)
