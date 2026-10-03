cmake_minimum_required(VERSION 3.25)

project(
    Hexlith
    VERSION 0.1.0
    DESCRIPTION "Static recompilation of Pokemon X for the PC"
    LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    # clang-tidy and editors read this.
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    endif()
endif()

option(HEXLITH_BUILD_TESTS "Build the unit tests" ON)
option(HEXLITH_WERROR "Treat compiler warnings as errors" OFF)
option(HEXLITH_FETCH_GOOGLETEST
       "Download GoogleTest when it is not installed on the system" OFF)

include(HexlithCompilerCache)
include(HexlithWarnings)

if(HEXLITH_BUILD_TESTS)
    include(HexlithTesting)
    enable_testing()
    add_subdirectory(tests)
endif()
