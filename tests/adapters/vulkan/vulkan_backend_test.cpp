#include "adapters/vulkan/vulkan_backend.h"
#include "application/gpu_render.h"
#include "domain/gpu/pica_registers.h"

#include <bit>
#include <cstdlib>
#include <format>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace hexlith::adapters::vulkan {
namespace {

namespace gpu = domain::gpu;
namespace reg = gpu::reg;

constexpr std::uint32_t Side = 32;
constexpr std::uint32_t ColorAddress = gpu::GpuMemory::VramBase;
constexpr std::uint32_t DepthAddress = gpu::GpuMemory::VramBase + 0x10000;
constexpr std::uint32_t VertexAddress = gpu::GpuMemory::VramBase + 0x20000;
// 16.0 in float24, half the buffer's side.
constexpr std::uint32_t Sixteen24 = 0x430000;
// -1.0 in float24, the depth scale that maps z from 0 to -w onto 0 to 1.
constexpr std::uint32_t MinusOne24 = 0xBF0000;

// MOV o0, v0; MOV o1, v1; END, and the descriptor that copies xyzw.
constexpr std::uint32_t Mov(std::uint32_t destination, std::uint32_t source) {
    return (0x13U << 26) | (destination << 21) | (source << 12);
}

constexpr std::uint32_t End = 0x22U << 26;
constexpr std::uint32_t Plain = 0xF | (0x1BU << 5) | (0x1BU << 14) | (0x1BU << 23);

// A command list in GBATEK's layout: each write a parameter and a header,
// which is eight bytes and needs no padding.
class List {
public:
    List& Write(std::uint16_t id, std::uint32_t value) {
        words_.insert(words_.end(), {value, id | (0xFU << 16)});
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

// Two overlapping triangles of flat colours with a depth test between them,
// and a combiner stage that modulates by a constant: everything an integer
// the reference and the shader compute alike, nothing blended.
List Scene() {
    List list;
    const std::uint16_t vs = reg::VertexShaderBase;
    list.Write(vs + reg::shader::CodeTransferIndex, 0);
    for (const std::uint32_t word : {Mov(0x0, 0x0), Mov(0x1, 0x1), End}) {
        list.Write(vs + reg::shader::CodeTransferDataFirst, word);
    }
    list.Write(vs + reg::shader::DescriptorIndex, 0)
        .Write(vs + reg::shader::DescriptorDataFirst, Plain);
    list.Write(vs + reg::shader::InputBufferConfig, 1)
        .Write(vs + reg::shader::AttributePermutationLow, 0x10)
        .Write(vs + reg::shader::OutputMask, 0x3)
        .Write(0x04F, 2)
        .Write(0x050, 0x03020100)
        .Write(0x051, 0x0B0A0908);
    list.Write(reg::AttributeBufferBase, VertexAddress >> 3)
        .Write(reg::AttributeFormatLow, 0xFF)
        .Write(reg::AttributeFormatHigh, 1U << 28)
        .Write(reg::AttributeBufferFirst + 1, 0x10)
        .Write(reg::AttributeBufferFirst + 2, (32U << 16) | (2U << 28));
    list.Write(0x041, Sixteen24).Write(0x043, Sixteen24).Write(0x04D, MinusOne24).Write(0x06D, 1);
    // Combiner stage 0: primary colour times a constant; the rest pass it on.
    list.Write(0x0C0, 0x00E000E0).Write(0x0C2, 0x00010001).Write(0x0C3, 0x80C0FFFF);
    for (const std::uint16_t stage :
         std::array<std::uint16_t, 5>{0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8}) {
        list.Write(stage, 0x00F000F0);
    }
    // Depth test less, depth and colour written, a logic copy.
    list.Write(0x107, 0x1F41).Write(0x102, 0x3).Write(0x113, 0xF).Write(0x115, 0x3);
    list.Write(0x116, 0x3).Write(0x117, 0x0);
    list.Write(0x11C, DepthAddress >> 3)
        .Write(0x11D, ColorAddress >> 3)
        .Write(0x11E, Side | ((Side - 1) << 12));
    list.Write(0x1C6, 1);
    list.Write(reg::VertexCount, 6).Write(reg::DrawArrays, 1);
    return list;
}

void AddVertex(
    std::vector<std::uint8_t>& bytes, float x, float y, float z, std::array<float, 4> color) {
    for (const float value : {x, y, -z, 1.0F, color[0], color[1], color[2], color[3]}) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::uint8_t>(bits >> shift));
        }
    }
}

std::unique_ptr<gpu::GpuMemory> Memory() {
    auto memory = std::make_unique<gpu::GpuMemory>();
    std::vector<std::uint8_t> vertices;
    AddVertex(vertices, -0.9F, -0.9F, 0.5F, {1, 0, 0, 1});
    AddVertex(vertices, 0.7F, -0.8F, 0.5F, {1, 0, 0, 1});
    AddVertex(vertices, -0.6F, 0.9F, 0.5F, {1, 0, 0, 1});
    AddVertex(vertices, -0.3F, -0.95F, 0.25F, {0, 1, 0.5F, 1});
    AddVertex(vertices, 0.95F, 0.1F, 0.25F, {0, 1, 0.5F, 1});
    AddVertex(vertices, 0.2F, 0.8F, 0.25F, {0, 1, 0.5F, 1});
    EXPECT_TRUE(memory->Write(VertexAddress, vertices));
    // Depth starts at its largest, so that the test lets the first through.
    EXPECT_TRUE(
        memory->Write(DepthAddress, std::vector<std::uint8_t>(std::size_t{Side} * Side * 4, 0xFF)));
    return memory;
}

std::unique_ptr<VulkanContext> Context() {
    auto context = VulkanContext::Create({});
    if (!context || !context.Value()->CanDraw()) {
        return nullptr;
    }
    return std::move(context.Value());
}

std::vector<std::uint8_t> Bytes(const gpu::GpuMemory& memory, std::uint32_t address) {
    const std::span<const std::uint8_t> bytes = memory.Bytes(address, Side * Side * 4);
    return {bytes.begin(), bytes.end()};
}

// How many bytes of a buffer differ, and the first few, or nothing.
std::string
Differences(const gpu::GpuMemory& first, const gpu::GpuMemory& second, std::uint32_t address) {
    const std::vector<std::uint8_t> a = Bytes(first, address);
    const std::vector<std::uint8_t> b = Bytes(second, address);
    std::string text;
    std::size_t count = 0;
    for (std::size_t at = 0; at < a.size(); ++at) {
        if (a[at] != b[at] && count++ < 6) {
            text += std::format("byte {}: {} against {}; ", at, a[at], b[at]);
        }
    }
    return count == 0 ? text : std::format("{} bytes differ: {}", count, text);
}

// Depths further apart than one unit of D24S8, and stencils that differ at
// all. The GPU interpolates depth in its own precision, so a depth that
// lands on a boundary between two integers can go either way (ADR 0021).
std::size_t DepthsApart(const gpu::GpuMemory& first, const gpu::GpuMemory& second) {
    const std::vector<std::uint8_t> a = Bytes(first, DepthAddress);
    const std::vector<std::uint8_t> b = Bytes(second, DepthAddress);
    std::size_t apart = 0;
    for (std::size_t at = 0; at < a.size(); at += 4) {
        const auto depth = [](const std::vector<std::uint8_t>& bytes, std::size_t pixel) {
            return static_cast<std::int64_t>(bytes[pixel] | (bytes[pixel + 1] << 8U) |
                                             (bytes[pixel + 2] << 16U));
        };
        if (std::abs(depth(a, at) - depth(b, at)) > 1 || a[at + 3] != b[at + 3]) {
            ++apart;
        }
    }
    return apart;
}

// The scene drawn into fresh memory by a backend.
template<typename MakeBackend>
std::unique_ptr<gpu::GpuMemory> Draw(const MakeBackend& make) {
    auto memory = Memory();
    const std::unique_ptr<ports::GpuBackend> backend = make(*memory);
    EXPECT_TRUE(backend != nullptr);
    if (backend) {
        EXPECT_TRUE(backend->RunCommandList(Scene().Bytes()).Ok());
        EXPECT_EQ(backend->Statistics().draws, 1U);
    }
    return memory;
}

TEST(VulkanGpuBackend, DrawsWhatTheReferenceDraws) {
    const std::unique_ptr<VulkanContext> context = Context();
    if (!context) {
        GTEST_SKIP() << "no Vulkan device that can draw";
    }
    const auto reference = Draw([](gpu::GpuMemory& memory) -> std::unique_ptr<ports::GpuBackend> {
        return std::make_unique<application::ReferenceGpuBackend>(memory, gpu::Sampling::Hardware);
    });
    const auto vulkan =
        Draw([&context](gpu::GpuMemory& memory) -> std::unique_ptr<ports::GpuBackend> {
            auto backend = VulkanGpuBackend::Create(*context, memory, gpu::Sampling::Hardware);
            return backend ? std::move(backend.Value()) : nullptr;
        });
    EXPECT_EQ(Differences(*vulkan, *reference, ColorAddress), "") << "colour";
    EXPECT_EQ(DepthsApart(*vulkan, *reference), 0U)
        << "a depth more than one unit of the format from the reference's";
    EXPECT_EQ(context->ValidationMessages(), 0U) << context->FirstValidationMessage();
}

}  // namespace
}  // namespace hexlith::adapters::vulkan
