# The domain holds pure logic and no I/O, which is what lets every parser be
# tested against synthetic data. One library, with each subdirectory adding
# its own sources.
add_library(hexlith_domain STATIC)

add_library(Hexlith::Domain ALIAS hexlith_domain)

target_link_libraries(hexlith_domain PUBLIC Hexlith::Core)

target_link_libraries(hexlith_domain PRIVATE Hexlith::Warnings)

add_subdirectory(ctr)
add_subdirectory(profile)
