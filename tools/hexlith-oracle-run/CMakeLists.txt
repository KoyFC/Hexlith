add_executable(hexlith-oracle-run main.cpp)

target_link_libraries(hexlith-oracle-run PRIVATE Hexlith::Domain Hexlith::Warnings)
