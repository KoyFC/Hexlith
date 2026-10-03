add_library(
    hexlith_application STATIC
    workspace.cpp
    workspace.h
)

add_library(Hexlith::Application ALIAS hexlith_application)

target_link_libraries(hexlith_application PUBLIC Hexlith::Core)

target_link_libraries(hexlith_application PRIVATE Hexlith::Warnings)
