#include "adapters/vulkan/vulkan_backend.h"
#include "application/gpu_render.h"
#include "domain/gpu/pica_registers.h"

#include <bit>
#include <chrono>
#include <cstdlib>
#include <format>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace hexlith::adapters::vulkan {
namespace {

namespace gpu = domain::gpu;
namespace reg = gpu::reg;

constexpr std::uint32_t Side = 32;
constexpr std::uint32_t ColorBytes = Side * Side * 4;
constexpr std::uint32_t DepthBytes = Side * Side * 4;
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

TEST(VulkanGpuBackend, ShadesVerticesAlikeOnTheCpuAndTheGpu) {
    const std::unique_ptr<VulkanContext> context = Context();
    if (!context) {
        GTEST_SKIP() << "no Vulkan device that can draw";
    }
    const auto draw = [&context](VertexShading shading) {
        return Draw(
            [&context, shading](gpu::GpuMemory& memory) -> std::unique_ptr<ports::GpuBackend> {
                auto backend =
                    VulkanGpuBackend::Create(*context, memory, gpu::Sampling::Hardware, shading);
                return backend ? std::move(backend.Value()) : nullptr;
            });
    };
    const auto on_cpu = draw(VertexShading::Cpu);
    const auto on_gpu = draw(VertexShading::GpuWherePossible);
    EXPECT_EQ(Differences(*on_gpu, *on_cpu, ColorAddress), "");
    EXPECT_EQ(Differences(*on_gpu, *on_cpu, DepthAddress), "");
}

// Two draws to one target are one group: the buffers go up once, and come
// back only when something reads them, and the statistics say how much moved.
TEST(VulkanGpuBackend, CountsTheRoundTripOfEachGroupOfDraws) {
    const std::unique_ptr<VulkanContext> context = Context();
    if (!context) {
        GTEST_SKIP() << "no Vulkan device that can draw";
    }
    const auto memory = Memory();
    auto backend = VulkanGpuBackend::Create(*context, *memory, gpu::Sampling::Hardware);
    ASSERT_TRUE(backend);
    List twice = Scene();
    twice.Write(reg::VertexCount, 6).Write(reg::DrawArrays, 1);

    ASSERT_TRUE(backend.Value()->RunCommandList(twice.Bytes()).Ok());

    const ports::GpuBackendStatistics statistics = backend.Value()->Statistics();
    EXPECT_EQ(statistics.draws, 2U);
    EXPECT_EQ(statistics.groups, 1U);
    // A 4-byte colour buffer and a 4-byte depth buffer, each way.
    constexpr std::uint64_t BufferBytes = std::uint64_t{Side} * Side * 4 * 2;
    EXPECT_EQ(statistics.uploaded_bytes, BufferBytes);
    EXPECT_EQ(statistics.downloaded_bytes, 0U);
    // The staging, vertex, input, draw-word and texel buffers of the group.
    EXPECT_EQ(statistics.allocations, 5U);

    ASSERT_TRUE(backend.Value()->BeforeRead(ColorAddress, ColorBytes).Ok());
    ASSERT_TRUE(backend.Value()->BeforeRead(DepthAddress, DepthBytes).Ok());
    EXPECT_EQ(backend.Value()->Statistics().downloaded_bytes, BufferBytes);
}

// A list of draws that change what the fragment stage does between them: the
// combiner's operation and sources, its scale, and the alpha test.
List FragmentConfigurations() {
    List list = Scene();
    // Operations and sources of combiner stage 0 and its scale, each drawn.
    for (const std::uint32_t operation : {0x00000000U, 0x00020002U, 0x00030003U, 0x00050005U}) {
        list.Write(0x0C2, operation).Write(reg::VertexCount, 6).Write(reg::DrawArrays, 1);
    }
    list.Write(0x0C0, 0x00E00000).Write(reg::VertexCount, 6).Write(reg::DrawArrays, 1);
    list.Write(0x0C4, 0x00020002).Write(reg::VertexCount, 6).Write(reg::DrawArrays, 1);
    // The alpha test, enabled and never passed, then passed.
    list.Write(0x104, 0x1U | (0x4U << 4U) | (0x80U << 8U))
        .Write(reg::VertexCount, 6)
        .Write(reg::DrawArrays, 1);
    list.Write(0x104, 0x1U | (0x1U << 4U) | (0x80U << 8U))
        .Write(reg::VertexCount, 6)
        .Write(reg::DrawArrays, 1);
    return list;
}

// ADR 0031: a draw does not wait for the shader made for its configuration,
// it takes the generic one's pipeline until that is ready, and either draws
// what the other does, to the byte.
TEST(VulkanGpuBackend, DrawsWhatTheGenericFragmentShaderDrawsWhenSpecialised) {
    const std::unique_ptr<VulkanContext> context = Context();
    if (!context) {
        GTEST_SKIP() << "no Vulkan device that can draw";
    }
    // How many times the list is drawn, one backend after the other, which is
    // as many as it takes the specialised one to draw a whole list with the
    // shaders made for it.
    std::size_t rounds = 0;
    const auto draw = [&context, &rounds](FragmentShading shading) {
        auto memory = Memory();
        auto backend = VulkanGpuBackend::Create(
            *context, *memory, gpu::Sampling::Hardware, VertexShading::GpuWherePossible, shading);
        EXPECT_TRUE(backend);
        if (!backend) {
            return memory;
        }
        std::uint64_t fallbacks_before = 0;
        const auto begin = std::chrono::steady_clock::now();
        for (std::size_t round = 1;; ++round) {
            EXPECT_TRUE(backend.Value()->RunCommandList(FragmentConfigurations().Bytes()).Ok());
            const ports::GpuBackendStatistics statistics = backend.Value()->Statistics();
            EXPECT_EQ(statistics.draws, 9U * round);
            if (shading == FragmentShading::Generic) {
                EXPECT_EQ(statistics.fallback_draws, 0U);
                if (round >= rounds) {
                    break;
                }
            } else {
                if (round == 1) {
                    EXPECT_GT(statistics.fallback_draws, 0U) << "the first draws had to wait";
                    // Several configurations, or this would hold a shader to itself.
                    EXPECT_GE(statistics.fragment_configurations, 5U);
                }
                // A round that needed no fallback drew only with the shaders made.
                if (statistics.fallback_draws == fallbacks_before ||
                    std::chrono::steady_clock::now() - begin > std::chrono::seconds(120)) {
                    rounds = round;
                    EXPECT_EQ(statistics.fallback_draws, fallbacks_before)
                        << "the shaders were never ready";
                    break;
                }
                fallbacks_before = statistics.fallback_draws;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        EXPECT_TRUE(backend.Value()->BeforeRead(ColorAddress, ColorBytes).Ok());
        EXPECT_TRUE(backend.Value()->BeforeRead(DepthAddress, DepthBytes).Ok());
        return memory;
    };
    // The specialised backend first, for it decides how many rounds there are.
    const auto specialised = draw(FragmentShading::Specialised);
    const auto generic = draw(FragmentShading::Generic);
    EXPECT_EQ(Differences(*specialised, *generic, ColorAddress), "") << "colour";
    EXPECT_EQ(Differences(*specialised, *generic, DepthAddress), "") << "depth";
    EXPECT_EQ(context->ValidationMessages(), 0U) << context->FirstValidationMessage();
}

// What a fragment shader specialised for a draw would be specialised for: the
// combiners' choices, the textures' formats and modes, lighting and the alpha
// test. A draw that changes only the numbers those read, a constant colour,
// is the same configuration.
TEST(VulkanGpuBackend, CountsDrawsByTheConfigurationOfTheirFragmentStage) {
    const std::unique_ptr<VulkanContext> context = Context();
    if (!context) {
        GTEST_SKIP() << "no Vulkan device that can draw";
    }
    const auto memory = Memory();
    auto backend = VulkanGpuBackend::Create(*context, *memory, gpu::Sampling::Hardware);
    ASSERT_TRUE(backend);
    List draws = Scene();
    draws.Write(reg::VertexCount, 6).Write(reg::DrawArrays, 1);
    // Another constant for the combiner, the same configuration.
    draws.Write(0x0C3, 0x80C01234).Write(reg::VertexCount, 6).Write(reg::DrawArrays, 1);
    ASSERT_TRUE(backend.Value()->RunCommandList(draws.Bytes()).Ok());
    EXPECT_EQ(backend.Value()->Statistics().draws, 3U);
    EXPECT_EQ(backend.Value()->Statistics().fragment_configurations, 1U);

    // Another operation in the combiner, another configuration.
    List other;
    other.Write(0x0C2, 0x00020002).Write(reg::VertexCount, 6).Write(reg::DrawArrays, 1);
    ASSERT_TRUE(backend.Value()->RunCommandList(other.Bytes()).Ok());
    EXPECT_EQ(backend.Value()->Statistics().draws, 4U);
    EXPECT_EQ(backend.Value()->Statistics().fragment_configurations, 2U);
}

// ADR 0030: a target stays on the device between lists and memory is made
// true when something reads it.

// The scene's second target: the same buffers, drawn at half the side.
constexpr std::uint32_t SmallSide = 16;

List SmallScene() {
    List list = Scene();
    list.Write(0x11E, SmallSide | ((SmallSide - 1) << 12));
    return list;
}

std::vector<std::uint8_t> Fill(std::uint8_t value) {
    return std::vector<std::uint8_t>(ColorBytes, value);
}

// What the reference makes of running the lists one after the other, with the
// memory changed by `between` after each.
template<typename Between>
std::unique_ptr<gpu::GpuMemory> Reference(const std::vector<List>& lists, const Between& between) {
    auto memory = Memory();
    application::ReferenceGpuBackend backend(*memory, gpu::Sampling::Hardware);
    for (const List& list : lists) {
        EXPECT_TRUE(backend.RunCommandList(list.Bytes()).Ok());
        between(*memory);
    }
    return memory;
}

struct Resident {
    std::unique_ptr<VulkanContext> context;
    std::unique_ptr<gpu::GpuMemory> memory;
    std::unique_ptr<VulkanGpuBackend> backend;
};

Resident MakeResident() {
    Resident made;
    made.context = Context();
    if (!made.context) {
        return made;
    }
    made.memory = Memory();
    auto backend = VulkanGpuBackend::Create(*made.context, *made.memory, gpu::Sampling::Hardware);
    EXPECT_TRUE(backend.Ok());
    if (backend) {
        made.backend = std::move(backend.Value());
    }
    return made;
}

TEST(VulkanGpuBackend, KeepsATargetOnTheDeviceUntilSomethingReadsIt) {
    Resident run = MakeResident();
    if (!run.backend) {
        GTEST_SKIP() << "no Vulkan device that can draw";
    }
    const auto reference = Reference({Scene(), Scene()}, [](gpu::GpuMemory&) {});

    ASSERT_TRUE(run.backend->RunCommandList(Scene().Bytes()).Ok());
    ASSERT_TRUE(run.backend->RunCommandList(Scene().Bytes()).Ok());
    EXPECT_EQ(run.backend->Statistics().uploaded_bytes, ColorBytes + DepthBytes)
        << "one upload for two lists";
    EXPECT_EQ(run.backend->Statistics().downloaded_bytes, 0U) << "nothing has read it";

    ASSERT_TRUE(run.backend->BeforeRead(ColorAddress, ColorBytes).Ok());
    EXPECT_EQ(run.backend->Statistics().downloaded_bytes, ColorBytes) << "the colour alone";
    EXPECT_EQ(Differences(*run.memory, *reference, ColorAddress), "");

    ASSERT_TRUE(run.backend->BeforeRead(DepthAddress, DepthBytes).Ok());
    EXPECT_EQ(run.backend->Statistics().downloaded_bytes, ColorBytes + DepthBytes);
    EXPECT_EQ(DepthsApart(*run.memory, *reference), 0U);
    EXPECT_EQ(run.backend->Statistics().conflicts, 0U);
    EXPECT_EQ(run.context->ValidationMessages(), 0U) << run.context->FirstValidationMessage();
}

// ADR 0032: a list is submitted and the game goes on while the device draws
// it. The host stops only when something needs what was drawn, and what it
// finds then is what the reference draws.
TEST(VulkanGpuBackend, ReturnsFromAListWithoutWaitingForTheDevice) {
    Resident run = MakeResident();
    if (!run.backend) {
        GTEST_SKIP() << "no Vulkan device that can draw";
    }
    const auto reference = Reference({Scene(), Scene()}, [](gpu::GpuMemory&) {});

    ASSERT_TRUE(run.backend->RunCommandList(Scene().Bytes()).Ok());
    ASSERT_TRUE(run.backend->RunCommandList(Scene().Bytes()).Ok());
    EXPECT_EQ(run.backend->Statistics().waits, 0U) << "two lists, nothing has needed them";

    ASSERT_TRUE(run.backend->BeforeRead(ColorAddress, ColorBytes).Ok());
    EXPECT_EQ(run.backend->Statistics().waits, 1U) << "the read waits for the copy, once";
    EXPECT_EQ(Differences(*run.memory, *reference, ColorAddress), "");
    EXPECT_EQ(run.context->ValidationMessages(), 0U) << run.context->FirstValidationMessage();
}

// The buffers a group reads are made for the first groups and kept: once every
// frame of the ring has been used, a list the same size as the last needs none
// of its own, and the pictures are as before.
TEST(VulkanGpuBackend, ReusesItsBuffersFromGroupToGroup) {
    Resident run = MakeResident();
    if (!run.backend) {
        GTEST_SKIP() << "no Vulkan device that can draw";
    }
    const std::vector<List> lists(10, Scene());
    const auto reference = Reference(lists, [](gpu::GpuMemory&) {});

    // The backend keeps four groups in flight, each with buffers of its own.
    constexpr std::size_t FramesInFlight = 4;
    for (std::size_t at = 0; at < FramesInFlight; ++at) {
        ASSERT_TRUE(run.backend->RunCommandList(lists.at(at).Bytes()).Ok());
    }
    const std::uint64_t made = run.backend->Statistics().allocations;
    for (std::size_t at = FramesInFlight; at < lists.size(); ++at) {
        ASSERT_TRUE(run.backend->RunCommandList(lists.at(at).Bytes()).Ok());
    }

    EXPECT_EQ(run.backend->Statistics().allocations, made) << "six more groups, no new buffer";
    ASSERT_TRUE(run.backend->BeforeRead(ColorAddress, ColorBytes).Ok());
    EXPECT_EQ(Differences(*run.memory, *reference, ColorAddress), "");
    EXPECT_EQ(run.context->ValidationMessages(), 0U) << run.context->FirstValidationMessage();
}

TEST(VulkanGpuBackend, DropsATargetAFillCoversWithoutWritingItBack) {
    Resident run = MakeResident();
    if (!run.backend) {
        GTEST_SKIP() << "no Vulkan device that can draw";
    }
    const auto fill = [](gpu::GpuMemory& memory) {
        EXPECT_TRUE(memory.Write(ColorAddress, Fill(0x7F)));
    };
    const auto reference = Reference({Scene(), Scene()}, fill);

    ASSERT_TRUE(run.backend->RunCommandList(Scene().Bytes()).Ok());
    ASSERT_TRUE(run.backend->BeforeWrite(ColorAddress, ColorBytes).Ok());
    fill(*run.memory);
    ASSERT_TRUE(run.backend->RunCommandList(Scene().Bytes()).Ok());
    ASSERT_TRUE(run.backend->BeforeRead(ColorAddress, ColorBytes).Ok());
    ASSERT_TRUE(run.backend->BeforeRead(DepthAddress, DepthBytes).Ok());

    EXPECT_EQ(Differences(*run.memory, *reference, ColorAddress), "");
    EXPECT_EQ(DepthsApart(*run.memory, *reference), 0U);
    // The colour was uploaded again after the fill; the write-back that
    // followed the second list is the only one.
    EXPECT_EQ(run.backend->Statistics().uploaded_bytes, 2 * ColorBytes + DepthBytes);
    EXPECT_EQ(run.backend->Statistics().downloaded_bytes, ColorBytes + DepthBytes);
    EXPECT_EQ(run.backend->Statistics().conflicts, 0U);
}

// A buffer a fill left is one pixel repeated, which is taken from memory
// faster; a single pixel that differs, even the last, must still be seen.
TEST(VulkanGpuBackend, SeesTheOnePixelThatDiffersInOtherwiseUniformMemory) {
    Resident run = MakeResident();
    if (!run.backend) {
        GTEST_SKIP() << "no Vulkan device that can draw";
    }
    const auto prepare = [](gpu::GpuMemory& memory) {
        EXPECT_TRUE(memory.Write(ColorAddress, Fill(0x7F)));
        EXPECT_TRUE(
            memory.Write(ColorAddress + ColorBytes - 4, std::vector<std::uint8_t>(4, 0x11)));
        EXPECT_TRUE(memory.Write(DepthAddress, std::vector<std::uint8_t>(DepthBytes, 0xFF)));
        EXPECT_TRUE(
            memory.Write(DepthAddress + DepthBytes - 4, std::vector<std::uint8_t>(4, 0x80)));
    };
    auto reference_memory = Memory();
    prepare(*reference_memory);
    application::ReferenceGpuBackend reference(*reference_memory, gpu::Sampling::Hardware);
    ASSERT_TRUE(reference.RunCommandList(Scene().Bytes()).Ok());
    prepare(*run.memory);

    ASSERT_TRUE(run.backend->RunCommandList(Scene().Bytes()).Ok());
    ASSERT_TRUE(run.backend->BeforeRead(ColorAddress, ColorBytes).Ok());
    ASSERT_TRUE(run.backend->BeforeRead(DepthAddress, DepthBytes).Ok());

    EXPECT_EQ(Differences(*run.memory, *reference_memory, ColorAddress), "");
    EXPECT_EQ(DepthsApart(*run.memory, *reference_memory), 0U);
}

TEST(VulkanGpuBackend, WritesBackWhatAPartialWriteWouldLose) {
    Resident run = MakeResident();
    if (!run.backend) {
        GTEST_SKIP() << "no Vulkan device that can draw";
    }
    constexpr std::uint32_t Row = Side * 4;
    const auto patch = [](gpu::GpuMemory& memory) {
        EXPECT_TRUE(memory.Write(ColorAddress, std::vector<std::uint8_t>(Row, 0x55)));
    };
    const auto reference = Reference({Scene()}, patch);

    ASSERT_TRUE(run.backend->RunCommandList(Scene().Bytes()).Ok());
    ASSERT_TRUE(run.backend->BeforeWrite(ColorAddress, Row).Ok());
    patch(*run.memory);
    ASSERT_TRUE(run.backend->BeforeRead(ColorAddress, ColorBytes).Ok());

    EXPECT_EQ(Differences(*run.memory, *reference, ColorAddress), "")
        << "the rest of the target survived the partial write";
}

TEST(VulkanGpuBackend, TakesMemoryAsTheTruthWhenAWriteWasNotAnnounced) {
    Resident run = MakeResident();
    if (!run.backend) {
        GTEST_SKIP() << "no Vulkan device that can draw";
    }
    const auto fill = [](gpu::GpuMemory& memory) {
        EXPECT_TRUE(memory.Write(ColorAddress, Fill(0x7F)));
    };
    const auto reference = Reference({Scene(), Scene()}, fill);

    ASSERT_TRUE(run.backend->RunCommandList(Scene().Bytes()).Ok());
    fill(*run.memory);
    ASSERT_TRUE(run.backend->RunCommandList(Scene().Bytes()).Ok());
    ASSERT_TRUE(run.backend->BeforeRead(ColorAddress, ColorBytes).Ok());

    EXPECT_EQ(run.backend->Statistics().conflicts, 1U);
    EXPECT_EQ(Differences(*run.memory, *reference, ColorAddress), "");
}

TEST(VulkanGpuBackend, WritesBackATargetAnotherOneOverlaps) {
    Resident run = MakeResident();
    if (!run.backend) {
        GTEST_SKIP() << "no Vulkan device that can draw";
    }
    const auto reference = Reference({Scene(), SmallScene(), Scene()}, [](gpu::GpuMemory&) {});

    ASSERT_TRUE(run.backend->RunCommandList(Scene().Bytes()).Ok());
    ASSERT_TRUE(run.backend->RunCommandList(SmallScene().Bytes()).Ok());
    ASSERT_TRUE(run.backend->RunCommandList(Scene().Bytes()).Ok());
    ASSERT_TRUE(run.backend->BeforeRead(ColorAddress, ColorBytes).Ok());
    ASSERT_TRUE(run.backend->BeforeRead(DepthAddress, DepthBytes).Ok());

    EXPECT_EQ(Differences(*run.memory, *reference, ColorAddress), "");
    EXPECT_EQ(DepthsApart(*run.memory, *reference), 0U);
    EXPECT_EQ(run.backend->Statistics().conflicts, 0U);
    EXPECT_EQ(run.context->ValidationMessages(), 0U) << run.context->FirstValidationMessage();
}

TEST(VulkanGpuBackend, LeavesMemoryTrueWhenItIsDestroyed) {
    Resident run = MakeResident();
    if (!run.backend) {
        GTEST_SKIP() << "no Vulkan device that can draw";
    }
    const auto reference = Reference({Scene()}, [](gpu::GpuMemory&) {});
    ASSERT_TRUE(run.backend->RunCommandList(Scene().Bytes()).Ok());
    run.backend.reset();
    EXPECT_EQ(Differences(*run.memory, *reference, ColorAddress), "");
}

}  // namespace
}  // namespace hexlith::adapters::vulkan
