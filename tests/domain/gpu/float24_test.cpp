#include "domain/gpu/float24.h"

#include <cmath>
#include <gtest/gtest.h>

namespace hexlith::domain::gpu {
namespace {

TEST(Float24ToFloat32, ConvertsNormalNumbers) {
    EXPECT_EQ(Float24ToFloat32(0x3F0000), 1.0F) << "exponent 63 is two to the zero";
    EXPECT_EQ(Float24ToFloat32(0xBF8000), -1.5F);
    EXPECT_EQ(Float24ToFloat32(0x3E0000), 0.5F);
}

TEST(Float24ToFloat32, KeepsTheSignOfZero) {
    EXPECT_EQ(Float24ToFloat32(0x000000), 0.0F);
    EXPECT_TRUE(std::signbit(Float24ToFloat32(0x800000)));
}

TEST(Float24ToFloat32, KeepsADenormalsValue) {
    // The smallest denormal is two to the power 1 - 63 - 16.
    EXPECT_EQ(Float24ToFloat32(0x000001), std::ldexp(1.0F, -78));
    EXPECT_EQ(Float24ToFloat32(0x008000), std::ldexp(1.0F, -63));
}

TEST(Float24ToFloat32, KeepsInfinitiesAndNaNs) {
    EXPECT_EQ(Float24ToFloat32(0x7F0000), INFINITY);
    EXPECT_EQ(Float24ToFloat32(0xFF0000), -INFINITY);
    EXPECT_TRUE(std::isnan(Float24ToFloat32(0x7F0001)));
}

}  // namespace
}  // namespace hexlith::domain::gpu
