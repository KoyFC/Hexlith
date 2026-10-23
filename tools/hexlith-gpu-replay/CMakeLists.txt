add_executable(hexlith-gpu-replay main.cpp)

target_link_libraries(hexlith-gpu-replay PRIVATE Hexlith::Adapters Hexlith::Application Hexlith::Domain Hexlith::Warnings)
