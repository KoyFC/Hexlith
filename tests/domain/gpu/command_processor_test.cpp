#include "domain/gpu/command_processor.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace hexlith::domain::gpu {
namespace {

// Builds command lists the way GBATEK's Command Lists chapter lays them out:
// the first parameter, the header, any further parameters, padded to eight
// bytes.
class ListBuilder {
public:
    ListBuilder& Write(std::uint16_t id,
                       const std::vector<std::uint32_t>& values,
                       std::uint32_t byte_mask = 0xF,
                       bool consecutive = false) {
        const auto extra = static_cast<std::uint32_t>(values.size() - 1);
        words_.push_back(values.front());
        words_.push_back(id | (byte_mask << 16) | (extra << 20) | (consecutive ? 1U << 31 : 0));
        for (std::size_t index = 1; index < values.size(); ++index) {
            words_.push_back(values[index]);
        }
        if (words_.size() % 2 != 0) {
            words_.push_back(0);
        }
        return *this;
    }

    [[nodiscard]] std::vector<std::uint8_t> Bytes() const {
        std::vector<std::uint8_t> bytes;
        for (const std::uint32_t word : words_) {
            for (unsigned shift = 0; shift < 32; shift += 8) {
                bytes.push_back(static_cast<std::uint8_t>(word >> shift));
            }
        }
        return bytes;
    }

private:
    std::vector<std::uint32_t> words_;
};

// Remembers what it was handed.
struct RecordingExecutor {
    void Draw(PicaState& state, bool indexed) {
        draws.push_back({.vertices = state.Register(reg::VertexCount), .indexed = indexed});
    }

    void ImmediateVertex(PicaState& state) {
        immediate.push_back(state.ImmediateVertex());
    }

    void RestartPrimitive(PicaState& /*state*/) {
        ++restarts;
    }

    void Finalize() {
        ++finalized;
    }

    struct DrawCall {
        std::uint32_t vertices = 0;
        bool indexed = false;
    };

    std::vector<DrawCall> draws;
    std::vector<std::array<Vec4, PicaState::VertexInputs>> immediate;
    int restarts = 0;
    int finalized = 0;
};

struct Fixture {
    core::Result<CommandListRun> Run(const ListBuilder& list) {
        CommandProcessor processor{state, memory, executor};
        return processor.Run(list.Bytes());
    }

    PicaState state;
    GpuMemory memory;
    RecordingExecutor executor;
};

constexpr std::uint16_t Scratch = 0x100;

TEST(CommandProcessor, WritesOnlyTheBytesTheMaskNames) {
    Fixture fixture;
    ListBuilder list;
    list.Write(Scratch, {0x11223344}).Write(Scratch, {0xAABBCCDD}, 0b0101);
    ASSERT_TRUE(fixture.Run(list).Ok());
    EXPECT_EQ(fixture.state.Register(Scratch), 0x11BB33DDU);
}

TEST(CommandProcessor, WritesConsecutiveRegistersOrOneRegisterRepeatedly) {
    Fixture fixture;
    ListBuilder list;
    list.Write(Scratch, {1, 2, 3}, 0xF, true).Write(Scratch + 8, {4, 5, 6});
    const core::Result<CommandListRun> run = fixture.Run(list);
    ASSERT_TRUE(run.Ok());
    EXPECT_EQ(fixture.state.Register(Scratch), 1U);
    EXPECT_EQ(fixture.state.Register(Scratch + 1), 2U);
    EXPECT_EQ(fixture.state.Register(Scratch + 2), 3U);
    EXPECT_EQ(fixture.state.Register(Scratch + 8), 6U) << "the last of three writes";
    EXPECT_EQ(run.Value().writes, 6U);
}

TEST(CommandProcessor, SkipsThePaddingAfterAnOddCommand) {
    Fixture fixture;
    ListBuilder list;
    list.Write(Scratch, {1, 2}).Write(Scratch + 1, {7});
    ASSERT_TRUE(fixture.Run(list).Ok());
    EXPECT_EQ(fixture.state.Register(Scratch + 1), 7U) << "the padding word is not a command";
}

TEST(CommandProcessor, DrawsWithTheStateOfTheMoment) {
    Fixture fixture;
    ListBuilder list;
    list.Write(reg::VertexCount, {3})
        .Write(reg::DrawArrays, {1})
        .Write(reg::VertexCount, {6})
        .Write(reg::DrawElements, {1})
        .Write(reg::PrimitiveConfig, {0x100});
    ASSERT_TRUE(fixture.Run(list).Ok());
    ASSERT_EQ(fixture.executor.draws.size(), 2U);
    EXPECT_EQ(fixture.executor.draws[0].vertices, 3U);
    EXPECT_FALSE(fixture.executor.draws[0].indexed);
    EXPECT_EQ(fixture.executor.draws[1].vertices, 6U);
    EXPECT_TRUE(fixture.executor.draws[1].indexed);
    EXPECT_EQ(fixture.executor.restarts, 1);
}

TEST(CommandProcessor, FollowsAJumpThroughMemory) {
    Fixture fixture;
    constexpr std::uint32_t Target = 0x18001000;
    ListBuilder second;
    second.Write(Scratch, {0x22}).Write(reg::DrawArrays, {1});
    const std::vector<std::uint8_t> bytes = second.Bytes();
    ASSERT_TRUE(fixture.memory.Write(Target, bytes));

    ListBuilder first;
    first.Write(reg::CommandBufferSize0, {static_cast<std::uint32_t>(bytes.size()) >> 3})
        .Write(reg::CommandBufferAddress0, {Target >> 3})
        .Write(reg::CommandBufferJump0, {1})
        .Write(Scratch, {0x11});
    const core::Result<CommandListRun> run = fixture.Run(first);
    ASSERT_TRUE(run.Ok()) << run.Failure().Message();
    EXPECT_EQ(fixture.state.Register(Scratch), 0x22U) << "the first list does not go on";
    EXPECT_EQ(fixture.executor.draws.size(), 1U);
    EXPECT_EQ(run.Value().jumps, 1U);
}

TEST(CommandProcessor, StopsAtAFinalizeThatMatches) {
    Fixture fixture;
    ListBuilder list;
    list.Write(reg::IrqRequestFirst, {0})
        .Write(reg::IrqRequestFirst, {0x12345678})
        .Write(Scratch, {1});
    const core::Result<CommandListRun> run = fixture.Run(list);
    ASSERT_TRUE(run.Ok());
    EXPECT_TRUE(run.Value().finalized);
    EXPECT_EQ(fixture.executor.finalized, 1) << "zero matches no byte of 12345678h";
    EXPECT_EQ(fixture.state.Register(Scratch), 0U) << "autostop ends the list";
}

// 1.0 in float24: exponent 63, mantissa 0. 2.0: exponent 64.
constexpr std::uint32_t One24 = 0x3F0000;
constexpr std::uint32_t Two24 = 0x400000;
constexpr std::uint32_t Three24 = 0x408000;
constexpr std::uint32_t Four24 = 0x410000;

// x, y, z and w as GBATEK's fixed attribute layout packs them: 96 bits with
// x lowest, sent highest word first.
std::vector<std::uint32_t>
Packed(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t w) {
    return {(w << 8) | (z >> 16), ((z & 0xFFFF) << 16) | (y >> 8), ((y & 0xFF) << 24) | x};
}

TEST(CommandProcessor, LoadsFloatUniformsInBothFormats) {
    Fixture fixture;
    const std::uint16_t index = reg::VertexShaderBase + reg::shader::FloatUniformIndex;
    const std::uint16_t data = reg::VertexShaderBase + reg::shader::FloatUniformDataFirst;
    ListBuilder list;
    list.Write(index, {5}).Write(data, Packed(One24, Two24, Three24, Four24));
    // Float32 mode, w first.
    list.Write(index, {(1U << 31) | 6})
        .Write(data, {0x40800000, 0x40400000, 0x40000000, 0x3F800000});
    ASSERT_TRUE(fixture.Run(list).Ok());
    EXPECT_EQ(fixture.state.VertexShader().float_uniforms.at(5), (Vec4{1, 2, 3, 4}));
    EXPECT_EQ(fixture.state.VertexShader().float_uniforms.at(6), (Vec4{1, 2, 3, 4}));
    EXPECT_EQ(fixture.state.Register(index) & 0x7F, 7U) << "the index steps on";
}

TEST(CommandProcessor, DropsAUniformPastTheLast) {
    Fixture fixture;
    const std::uint16_t index = reg::VertexShaderBase + reg::shader::FloatUniformIndex;
    ListBuilder list;
    list.Write(index, {0x7F}).Write(index + 1, Packed(One24, One24, One24, One24));
    ASSERT_TRUE(fixture.Run(list).Ok());
    EXPECT_EQ(fixture.state.Register(index), 0x7FU) << "an index out of range does not step";
}

TEST(CommandProcessor, UploadsCodeAndDescriptorsAndMirrorsThemToTheGeometryShader) {
    Fixture fixture;
    const std::uint16_t code_index = reg::VertexShaderBase + reg::shader::CodeTransferIndex;
    const std::uint16_t code_data = reg::VertexShaderBase + reg::shader::CodeTransferDataFirst;
    const std::uint16_t descriptor_index = reg::VertexShaderBase + reg::shader::DescriptorIndex;
    ListBuilder list;
    list.Write(code_index, {10}).Write(code_data, {0xA, 0xB}).Write(descriptor_index, {3});
    list.Write(reg::VertexShaderBase + reg::shader::DescriptorDataFirst, {0xD});
    ASSERT_TRUE(fixture.Run(list).Ok());
    EXPECT_EQ(fixture.state.VertexShader().code.at(11), 0xBU);
    EXPECT_EQ(fixture.state.VertexShader().descriptors.at(3), 0xDU);
    EXPECT_EQ(fixture.state.Register(code_index), 12U);
    EXPECT_EQ(fixture.state.GeometryShader().code.at(10), 0xAU)
        << "with no geometry shader in use and the configurations shared";
}

TEST(CommandProcessor, KeepsTheUnitsApartWhenTheModeSaysSo) {
    Fixture fixture;
    ListBuilder list;
    list.Write(reg::VertexShaderCommonMode, {1})
        .Write(reg::VertexShaderBase + reg::shader::BoolUniform, {0x8001});
    ASSERT_TRUE(fixture.Run(list).Ok());
    EXPECT_EQ(fixture.state.VertexShader().bool_uniforms, 0x8001U);
    EXPECT_EQ(fixture.state.GeometryShader().bool_uniforms, 0U);
}

TEST(CommandProcessor, GivesAVertexInImmediateMode) {
    Fixture fixture;
    ListBuilder list;
    // Two inputs: the attribute count less one is 1.
    list.Write(reg::VertexShaderAttributeCount, {1}).Write(reg::FixedAttributeIndex, {0xF});
    list.Write(reg::FixedAttributeDataFirst, Packed(One24, One24, One24, One24));
    list.Write(reg::FixedAttributeDataFirst, Packed(Two24, Two24, Two24, Two24));
    ASSERT_TRUE(fixture.Run(list).Ok());
    ASSERT_EQ(fixture.executor.immediate.size(), 1U);
    EXPECT_EQ(fixture.executor.immediate.front().at(1), (Vec4{2, 2, 2, 2}));
}

TEST(CommandProcessor, SetsAFixedAttributeAndStepsOn) {
    Fixture fixture;
    ListBuilder list;
    list.Write(reg::FixedAttributeIndex, {2})
        .Write(reg::FixedAttributeDataFirst, Packed(One24, Two24, Three24, Four24));
    ASSERT_TRUE(fixture.Run(list).Ok());
    EXPECT_EQ(fixture.state.FixedAttributeValues().at(2), (Vec4{1, 2, 3, 4}));
    EXPECT_EQ(fixture.state.Register(reg::FixedAttributeIndex), 3U);
    EXPECT_TRUE(fixture.executor.immediate.empty());
}

TEST(CommandProcessor, FillsALightingTableEntryByEntry) {
    Fixture fixture;
    ListBuilder list;
    list.Write(reg::LightingLutIndex, {(3U << 8) | 0xFE})
        .Write(reg::LightingLutDataFirst, {7, 8, 9});
    ASSERT_TRUE(fixture.Run(list).Ok());
    EXPECT_EQ(fixture.state.LightingLutData().at(3).at(0xFE), 7U);
    EXPECT_EQ(fixture.state.LightingLutData().at(3).at(0xFF), 8U);
    EXPECT_EQ(fixture.state.LightingLutData().at(3).at(0), 9U) << "the entry wraps";
}

}  // namespace
}  // namespace hexlith::domain::gpu
