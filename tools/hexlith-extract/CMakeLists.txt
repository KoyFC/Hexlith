add_executable(hexlith-extract main.cpp)

target_link_libraries(hexlith-extract PRIVATE Hexlith::ToolMain Hexlith::Adapters Hexlith::Warnings)
