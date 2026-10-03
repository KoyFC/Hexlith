# Lifted code will eventually amount to millions of generated lines, so a
# compiler cache is part of the build rather than a local convenience. It is
# wired up here once instead of being configured per developer.

find_program(HEXLITH_CCACHE_PROGRAM ccache)

if(HEXLITH_CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${HEXLITH_CCACHE_PROGRAM}")
    message(STATUS "Hexlith: using ccache at ${HEXLITH_CCACHE_PROGRAM}")
else()
    message(STATUS "Hexlith: ccache not found, compiling without a cache")
endif()
