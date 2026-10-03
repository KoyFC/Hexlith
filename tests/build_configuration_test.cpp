#include <gtest/gtest.h>

// Hexlith's code assumes C++20 throughout. Asserting it here means a
// toolchain or preset that silently selects an older standard fails the test
// suite rather than producing confusing errors somewhere else.
namespace {

constexpr long Cpp20 = 202002L;

TEST(BuildConfiguration, CompilesAsCpp20OrLater) {
    static_assert(__cplusplus >= Cpp20, "Hexlith requires C++20");
    EXPECT_GE(__cplusplus, Cpp20);
}

TEST(BuildConfiguration, UsesTwosComplementFixedWidthIntegers) {
    // The ARM semantics and every on-disk layout depend on this.
    static_assert(-1 == ~0, "two's complement is required");
    static_assert(sizeof(int) == 4, "32-bit int is assumed by the parsers");
    EXPECT_EQ(-1, ~0);
}

}  // namespace
