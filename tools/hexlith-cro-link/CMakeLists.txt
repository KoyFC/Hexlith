add_executable(hexlith-cro-link main.cpp)

target_link_libraries(hexlith-cro-link PRIVATE Hexlith::Domain Hexlith::Warnings)
