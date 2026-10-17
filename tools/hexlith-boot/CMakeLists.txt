add_executable(hexlith-boot main.cpp)

target_link_libraries(hexlith-boot PRIVATE Hexlith::Adapters Hexlith::Warnings)
