add_executable(hexlith-arm-run main.cpp)

target_link_libraries(hexlith-arm-run PRIVATE Hexlith::Domain Hexlith::Warnings)
