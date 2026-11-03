target_sources(hexlith_adapters PRIVATE lifted_library.cpp lifted_library.h modules_library.cpp
                                        modules_library.h)

#dlopen and dlsym.
    target_link_libraries(hexlith_adapters PUBLIC ${CMAKE_DL_LIBS})
