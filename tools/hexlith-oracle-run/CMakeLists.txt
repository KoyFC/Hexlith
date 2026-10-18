add_executable(hexlith-oracle-run main.cpp)

target_link_libraries(hexlith-oracle-run PRIVATE Hexlith::Adapters Hexlith::Application Hexlith::Domain Hexlith::Warnings)
