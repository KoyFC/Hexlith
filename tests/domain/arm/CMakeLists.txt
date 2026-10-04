hexlith_add_test(hexlith_domain_arm_test decode_arm_test.cpp decode_thumb_test.cpp decode_vfp_test.cpp
                 instruction_test.cpp integer_primitives_test.cpp)

target_link_libraries(hexlith_domain_arm_test PRIVATE Hexlith::Domain)
