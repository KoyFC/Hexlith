add_executable(hexlith-lift main.cpp)

target_link_libraries(hexlith-lift PRIVATE Hexlith::ToolMain Hexlith::Adapters Hexlith::Warnings)
