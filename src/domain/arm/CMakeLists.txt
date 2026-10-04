target_sources(
    hexlith_domain
    PRIVATE condition.cpp
            condition.h
            cpu_state.h
            decode_arm.cpp
            decode_arm.h
            decode_thumb.cpp
            decode_thumb.h
            decode_vfp.cpp
            decode_vfp.h
            instruction.cpp
            instruction.h
            instruction_set.cpp
            instruction_set.h
            operation.cpp
            operation.h
            primitives/integer.h
            primitives/multiply.h
            primitives/packing.h
            primitives/parallel.h
            primitives/saturate.h
            primitives/vfp.h
            registers.cpp
            registers.h
)

# VFPv2's multiply-accumulate rounds the product before the sum. A host
# compiler allowed to contract a multiplication and an addition into one fused
# operation would round once, and the floating point primitives would stop
# matching the hardware the moment the build targeted a host with FMA. PUBLIC,
# because generated code includes the primitives and needs the same guarantee.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hexlith_domain PUBLIC -ffp-contract=off)
endif()
