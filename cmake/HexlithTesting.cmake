# GoogleTest is expected to come from the system. Downloading it is opt-in so
# that a plain build never reaches the network, which keeps offline builds and
# reproducible CI honest.

find_package(GTest QUIET)

if(NOT GTest_FOUND AND HEXLITH_FETCH_GOOGLETEST)
    message(STATUS "Hexlith: fetching GoogleTest")
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    set(GTest_FOUND TRUE)
endif()

if(NOT GTest_FOUND)
    message(FATAL_ERROR
            "GoogleTest was not found. Install it (gtest-devel on Fedora), or "
            "configure with -DHEXLITH_FETCH_GOOGLETEST=ON to download it, or "
            "with -DHEXLITH_BUILD_TESTS=OFF to skip the tests.")
endif()

include(GoogleTest)

# Registers one test executable with CTest. Every Hexlith test target goes
# through here so that the warning policy and test discovery stay uniform.
function(hexlith_add_test target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE Hexlith::Warnings GTest::gtest_main)
    gtest_discover_tests(${target})
endfunction()
