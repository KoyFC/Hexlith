hexlith_add_test(hexlith_adapters_sdl_test sdl_window_test.cpp)

target_link_libraries(hexlith_adapters_sdl_test PRIVATE Hexlith::Sdl Hexlith::TestSupport)
