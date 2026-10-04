add_executable(hexlith-cro-dump main.cpp)

target_link_libraries(hexlith-cro-dump PRIVATE Hexlith::Domain Hexlith::Warnings)
