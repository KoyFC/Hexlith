add_executable(hexlith-analyze main.cpp)

target_link_libraries(hexlith-analyze PRIVATE Hexlith::Adapters Hexlith::Warnings)
