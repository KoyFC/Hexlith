# One interface target carries the warning policy, so every Hexlith target
# gets the same diagnostics by linking against it. Third-party code is never
# linked against this target and keeps its own upstream warning level.
#
# -Wconversion and -Wsign-conversion are deliberately on. The container
# parsers and the ARM semantics both spend their time narrowing and widening
# integers, which is exactly where a silent conversion becomes a wrong value.

add_library(hexlith_warnings INTERFACE)
add_library(Hexlith::Warnings ALIAS hexlith_warnings)

set(hexlith_gnu_like_warnings
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wold-style-cast
    -Wcast-align
    -Wcast-qual
    -Wunused
    -Woverloaded-virtual
    -Wnon-virtual-dtor
    -Wdouble-promotion
    -Wimplicit-fallthrough
    -Wformat=2
    -Wundef
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hexlith_warnings INTERFACE ${hexlith_gnu_like_warnings})
    if(HEXLITH_WERROR)
        target_compile_options(hexlith_warnings INTERFACE -Werror)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(hexlith_warnings INTERFACE /W4 /permissive-)
    if(HEXLITH_WERROR)
        target_compile_options(hexlith_warnings INTERFACE /WX)
    endif()
else()
    message(WARNING
            "Hexlith: no warning policy for compiler ${CMAKE_CXX_COMPILER_ID}")
endif()
