add_executable(hexlith-lift-verify main.cpp)

target_link_libraries(hexlith-lift-verify PRIVATE Hexlith::Adapters Hexlith::Warnings)

# The lifted library calls into this binary for the domain's code, as it does
# into hexlith-boot, so the symbols have to be visible to it.
set_target_properties(hexlith-lift-verify PROPERTIES ENABLE_EXPORTS ON)
