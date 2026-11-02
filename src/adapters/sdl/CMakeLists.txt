# The window the screens are shown in, a library of its own so that only what
# opens a window links SDL. ADR 0027.
add_library(hexlith_sdl STATIC sdl_window.cpp sdl_window.h)

add_library(Hexlith::Sdl ALIAS hexlith_sdl)

target_link_libraries(hexlith_sdl PUBLIC Hexlith::Ports SDL3::SDL3)

target_link_libraries(hexlith_sdl PRIVATE Hexlith::Warnings)
