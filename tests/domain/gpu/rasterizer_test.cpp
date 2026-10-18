#include "domain/gpu/rasterizer.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::gpu {
namespace {

// -1.0 and 8.0 in float24: exponent 63 with the sign, and 66.
constexpr std::uint32_t MinusOne24 = 0xBF0000;
constexpr std::uint32_t Eight24 = 0x420000;

constexpr std::uint32_t Side = 16;
constexpr std::uint32_t ColorAddress = GpuMemory::VramBase;
constexpr std::uint32_t DepthAddress = GpuMemory::VramBase + 0x10000;

// A 16x16 RGBA8 colour buffer and a D24S8 depth buffer, the viewport over
// them, every combiner stage passing the vertex colour, and the colour
// written as it comes.
struct Fixture {
    Fixture() {
        Set(0x041, Eight24);     // viewport half width
        Set(0x043, Eight24);     // viewport half height
        Set(0x04D, MinusOne24);  // depth scale: z from 0 to -w gives 0 to 1
        Set(0x107, 0xF00);       // colour write mask
        Set(0x102, 0x3);         // logic operation: copy
        Set(0x113, 0xF);
        Set(0x115, 0x3);
        Set(0x116, 0x3);  // D24S8
        Set(0x117, 0x0);  // RGBA8
        Set(0x11C, DepthAddress >> 3);
        Set(0x11D, ColorAddress >> 3);
        Set(0x11E, Side | ((Side - 1) << 12));
        Set(0x1C6, 1);  // lighting off
    }

    void Set(std::uint16_t id, std::uint32_t value) {
        state.Write(id, value, 0xF);
    }

    // A vertex at (x, y) in clip space with w 1, and a colour.
    static OutputVertex At(float x, float y, float z = 0.5F, Vec4 color = {1, 1, 1, 1}) {
        OutputVertex vertex;
        vertex.values[0] = x;
        vertex.values[1] = y;
        vertex.values[2] = -z;
        vertex.values[3] = 1.0F;
        for (std::size_t channel = 0; channel < 4; ++channel) {
            vertex.values.at(OutputVertex::Color + channel) = color.at(channel);
        }
        return vertex;
    }

    void Draw(const OutputVertex& a, const OutputVertex& b, const OutputVertex& c) {
        rasterizer.DrawTriangle(state, {a, b, c});
    }

    // Pixel (x, y) with y counted from the bottom, as the screen is.
    [[nodiscard]] Color Pixel(std::uint32_t x, std::uint32_t y) const {
        const std::size_t offset = TiledOffset(x, Side - 1 - y, Side, 4);
        return DecodePixel(PixelFormat::Rgba8,
                           memory.Bytes(ColorAddress + static_cast<std::uint32_t>(offset), 4));
    }

    [[nodiscard]] std::uint32_t Covered() const {
        std::uint32_t covered = 0;
        for (std::uint32_t y = 0; y < Side; ++y) {
            for (std::uint32_t x = 0; x < Side; ++x) {
                covered += Pixel(x, y).alpha != 0 ? 1U : 0U;
            }
        }
        return covered;
    }

    PicaState state;
    GpuMemory memory;
    Rasterizer rasterizer{memory, Sampling::AzaharSoftware};
};

TEST(Rasterizer, FillsTwoTrianglesThatShareAnEdgeOnceEach) {
    Fixture fixture;
    // Additive blending of a colour of 1/255: a pixel drawn twice shows 2.
    fixture.Set(0x100, 1U << 8);
    fixture.Set(0x101, (1U << 16) | (1U << 20) | (1U << 24) | (1U << 28));
    const Vec4 one{1.0F / 255.0F, 1.0F / 255.0F, 1.0F / 255.0F, 1.0F / 255.0F};
    fixture.Draw(Fixture::At(-1, -1, 0.5F, one),
                 Fixture::At(1, -1, 0.5F, one),
                 Fixture::At(1, 1, 0.5F, one));
    fixture.Draw(Fixture::At(-1, -1, 0.5F, one),
                 Fixture::At(1, 1, 0.5F, one),
                 Fixture::At(-1, 1, 0.5F, one));
    for (std::uint32_t y = 0; y < Side; ++y) {
        for (std::uint32_t x = 0; x < Side; ++x) {
            EXPECT_EQ(fixture.Pixel(x, y).red, 1) << x << ", " << y;
        }
    }
}

TEST(Rasterizer, CullsTheWindingItIsToldTo) {
    Fixture fixture;
    fixture.Set(0x040, 2);  // keep counter-clockwise
    fixture.Draw(Fixture::At(-1, -1), Fixture::At(-1, 1), Fixture::At(1, -1));
    EXPECT_EQ(fixture.Covered(), 0U) << "clockwise on the screen";
    fixture.Draw(Fixture::At(-1, -1), Fixture::At(1, -1), Fixture::At(-1, 1));
    EXPECT_EQ(fixture.Covered(), (Side * (Side + 1) / 2) - Side)
        << "the diagonal goes to the other side";
}

TEST(Rasterizer, ClipsToTheViewVolume) {
    Fixture fixture;
    fixture.Draw(Fixture::At(-3, -3), Fixture::At(3, -3), Fixture::At(0, 3));
    EXPECT_GT(fixture.Covered(), 0U);
    EXPECT_EQ(fixture.rasterizer.Statistics().fragments, fixture.Covered())
        << "nothing outside the buffer";
}

TEST(Rasterizer, KeepsTheNearerFragmentWithADepthTest) {
    Fixture fixture;
    fixture.Set(0x107, 0xF00 | 1U | (4U << 4) | (1U << 12));  // less, write depth
    ASSERT_TRUE(fixture.memory.Write(
        DepthAddress, std::vector<std::uint8_t>(std::size_t{Side} * Side * 4, 0xFF)));
    const Vec4 red{1, 0, 0, 1};
    const Vec4 blue{0, 0, 1, 1};
    fixture.Draw(Fixture::At(-1, -1, 0.25F, red),
                 Fixture::At(3, -1, 0.25F, red),
                 Fixture::At(-1, 3, 0.25F, red));
    fixture.Draw(Fixture::At(-1, -1, 0.75F, blue),
                 Fixture::At(3, -1, 0.75F, blue),
                 Fixture::At(-1, 3, 0.75F, blue));
    EXPECT_EQ(fixture.Pixel(4, 4), (Color{.red = 255, .green = 0, .blue = 0, .alpha = 255}));
}

TEST(Rasterizer, DrawsOnlyInsideAnIncludingScissor) {
    Fixture fixture;
    fixture.Set(0x065, 3);
    fixture.Set(0x066, 2 | (2U << 16));
    fixture.Set(0x067, 5 | (5U << 16));
    fixture.Draw(Fixture::At(-1, -1), Fixture::At(3, -1), Fixture::At(-1, 3));
    EXPECT_EQ(fixture.Covered(), 16U) << "the second corner is inclusive";
    EXPECT_NE(fixture.Pixel(5, 5).alpha, 0);
    EXPECT_EQ(fixture.Pixel(6, 5).alpha, 0);
}

TEST(Rasterizer, InterpolatesTheColourAcrossTheTriangle) {
    Fixture fixture;
    const Vec4 black{0, 0, 0, 1};
    const Vec4 white{1, 1, 1, 1};
    fixture.Draw(Fixture::At(-1, -1, 0.5F, black),
                 Fixture::At(1, -1, 0.5F, white),
                 Fixture::At(-1, 1, 0.5F, black));
    // Pixel centres at x + 0.5 of 16: red is (x + 0.5) / 16 of 255.
    EXPECT_EQ(fixture.Pixel(0, 0).red, 8);
    EXPECT_EQ(fixture.Pixel(7, 0).red, 120);
}

}  // namespace
}  // namespace hexlith::domain::gpu
