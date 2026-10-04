add_executable(hexlith-arm-dump main.cpp)

target_link_libraries(hexlith-arm-dump PRIVATE Hexlith::Domain Hexlith::Warnings)
