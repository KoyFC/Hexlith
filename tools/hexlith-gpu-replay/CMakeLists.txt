add_executable(hexlith-gpu-replay main.cpp)

target_link_libraries(hexlith-gpu-replay PRIVATE Hexlith::ToolMain Hexlith::Adapters Hexlith::Vulkan Hexlith::Application Hexlith::Domain Hexlith::Warnings)
