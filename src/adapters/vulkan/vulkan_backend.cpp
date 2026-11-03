#include "adapters/vulkan/vulkan_backend.h"

#include "adapters/vulkan/fragment_shader.h"
#include "adapters/vulkan/glsl_compiler.h"
#include "domain/gpu/command_processor.h"
#include "domain/gpu/float24.h"
#include "domain/gpu/pixel_formats.h"
#include "domain/gpu/reference_renderer.h"
#include "domain/gpu/shader_glsl.h"
#include "domain/gpu/texture_cache.h"
#include "domain/gpu/vertex_stage.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <future>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace hexlith::adapters::vulkan {
namespace {

namespace gpu = domain::gpu;

constexpr std::uint32_t Field(std::uint32_t word, unsigned low, unsigned width) {
    return (word >> low) & ((1U << width) - 1);
}

// Registers the backend reads on the CPU. Source: GBATEK, as cited in
// docs/formats/pica-registers.md.
namespace reg {
constexpr std::uint16_t FaceCulling = 0x040;
constexpr std::uint16_t ViewportHalfWidth = 0x041;
constexpr std::uint16_t ViewportHalfHeight = 0x043;
constexpr std::uint16_t ClipEnable = 0x047;
constexpr std::uint16_t ClipCoefficientFirst = 0x048;
constexpr std::uint16_t ViewportCorner = 0x068;
constexpr std::uint16_t TextureConfig = 0x080;
constexpr std::uint16_t ColorOperation = 0x100;
constexpr std::uint16_t BlendFunction = 0x101;
constexpr std::uint16_t LogicOperation = 0x102;
constexpr std::uint16_t BlendColor = 0x103;
constexpr std::uint16_t StencilTest = 0x105;
constexpr std::uint16_t StencilOperation = 0x106;
constexpr std::uint16_t DepthColorMask = 0x107;
constexpr std::uint16_t ColorWrite = 0x113;
constexpr std::uint16_t DepthWrite = 0x115;
constexpr std::uint16_t DepthFormat = 0x116;
constexpr std::uint16_t ColorFormat = 0x117;
constexpr std::uint16_t DepthAddress = 0x11C;
constexpr std::uint16_t ColorAddress = 0x11D;
constexpr std::uint16_t Dimensions = 0x11E;
}  // namespace reg

constexpr std::array<std::uint16_t, 3> UnitDimensions = {0x082, 0x092, 0x09A};
constexpr std::array<std::uint16_t, 3> UnitLevels = {0x084, 0x094, 0x09C};
constexpr std::array<std::uint16_t, 3> UnitAddress = {0x085, 0x095, 0x09D};
constexpr std::array<std::uint16_t, 3> UnitFormat = {0x08E, 0x096, 0x09E};

// The words the fragment shader reads for a draw, as fragment_shader.cpp
// lays them out.
constexpr std::size_t RegisterWords = gpu::RegisterCount;
constexpr std::size_t LightingWords =
    gpu::PicaState::LightingLuts * gpu::PicaState::LightingLutEntries;
constexpr std::size_t FogWords = gpu::PicaState::FogLutEntries;
constexpr std::size_t LevelsPerUnit = 16;
constexpr std::size_t LevelWords = 3 * LevelsPerUnit;
// The vertex shader's uniforms, for a draw whose vertices the GPU shades.
constexpr std::size_t FloatUniformWords = gpu::ShaderUnitState::FloatUniforms * 4;
constexpr std::size_t UniformWords = FloatUniformWords + 1 + gpu::ShaderUnitState::IntUniforms;
constexpr std::size_t DrawWords =
    RegisterWords + LightingWords + FogWords + LevelWords + UniformWords;
constexpr std::uint32_t NoLevel = 0xFFFFFFFF;

// A vertex as the vertex shader takes it: the 24 semantic slots.
constexpr std::size_t VertexFloats = gpu::OutputVertex::Slots;
// A vertex the GPU shades: its sixteen input registers.
constexpr std::size_t InputFloats = gpu::ShaderRegisters::Count * 4;

// The shaders' push constants, std430: the draw, the sampling mode, the
// buffer's height, the clip plane, and the PICA's viewport.
struct PushConstants {
    std::uint32_t draw = 0;
    std::uint32_t sampling = 0;
    std::uint32_t height = 0;
    std::uint32_t clip_enabled = 0;
    std::array<float, 4> clip_plane{};
    std::array<float, 4> viewport{};
};

constexpr VkFormat ColorImageFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat DepthImageFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;

// The fixed-function state a draw's pipeline is built for, as the register
// words it comes from.
struct PipelineKey {
    std::array<std::uint32_t, 11> words{};
    // The vertex program the GPU shades the draw's vertices with, or zero
    // for vertices shaded on the CPU.
    std::uint64_t program = 0;
    // The fragment shader made for the draw's configuration, or zero for the
    // one that reads it from the registers.
    std::uint32_t fragment = 0;
    auto operator<=>(const PipelineKey&) const = default;
};

PipelineKey KeyOf(const gpu::RegisterFile& registers) {
    return {.words = {Field(registers.at(reg::FaceCulling), 0, 2),
                      registers.at(reg::ColorOperation),
                      registers.at(reg::BlendFunction),
                      registers.at(reg::LogicOperation),
                      registers.at(reg::BlendColor),
                      registers.at(reg::StencilTest),
                      registers.at(reg::StencilOperation),
                      registers.at(reg::DepthColorMask),
                      Field(registers.at(reg::ColorWrite), 0, 4),
                      Field(registers.at(reg::DepthWrite), 0, 2),
                      Field(registers.at(reg::DepthFormat), 0, 2)}};
}

// What the fragment stage of a draw is configured to do, as far as a shader
// made for the draw would be built for it: the combiners' sources, operands,
// operations and scales, the textures' formats and modes, the fog, lighting
// and alpha test registers. What the stage only reads as numbers is left out:
// the combiners' constant colours (offset 3 of a stage), the textures' addresses,
// sizes and levels, and the lighting tables' data. Source: GBATEK, as cited in
// docs/formats/pica-registers.md.
constexpr std::size_t FragmentConfigurationSize = 1 + 6 + (6 * 4) + 3 + 25;

constexpr std::array<std::uint16_t, FragmentConfigurationSize> FragmentConfigurationRegisters() {
    constexpr std::array<std::uint16_t, 6> StageBases = {0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8};
    constexpr std::array<std::uint16_t, 4> StageWords = {0, 1, 2, 4};
    constexpr std::array<std::uint16_t, 6> UnitModes = {0x083, 0x093, 0x09B, 0x08E, 0x096, 0x09E};
    constexpr std::array<std::uint16_t, 3> Others = {
        0x0E0, 0x0E1, 0x104};  // fog, fog colour, alpha
    constexpr std::uint16_t LightingFirst = 0x1C0;
    constexpr std::uint16_t LightingLutIndex = 0x1C5;
    constexpr std::uint16_t LightingLast = 0x1D9;
    std::array<std::uint16_t, FragmentConfigurationSize> ids{};
    std::size_t at = 0;
    ids.at(at++) = reg::TextureConfig;
    for (const std::uint16_t mode : UnitModes) {
        ids.at(at++) = mode;
    }
    for (const std::uint16_t base : StageBases) {
        for (const std::uint16_t word : StageWords) {
            ids.at(at++) = static_cast<std::uint16_t>(base + word);
        }
    }
    for (const std::uint16_t id : Others) {
        ids.at(at++) = id;
    }
    for (std::uint16_t id = LightingFirst; id <= LightingLast; ++id) {
        if (id != LightingLutIndex) {
            ids.at(at++) = id;
        }
    }
    return ids;
}

constexpr auto FragmentRegisters = FragmentConfigurationRegisters();

using FragmentConfiguration = std::array<std::uint32_t, FragmentConfigurationSize>;

FragmentConfiguration FragmentConfigurationOf(const gpu::RegisterFile& registers) {
    FragmentConfiguration words{};
    for (std::size_t at = 0; at < FragmentRegisters.size(); ++at) {
        words.at(at) = registers.at(FragmentRegisters.at(at));
    }
    return words;
}

// FNV-1a over the words: a draw's configuration is looked up for every draw.
struct FragmentConfigurationHash {
    std::size_t operator()(const FragmentConfiguration& words) const {
        constexpr std::uint64_t Basis = 14695981039346656037ULL;
        constexpr std::uint64_t Prime = 1099511628211ULL;
        std::uint64_t hash = Basis;
        for (const std::uint32_t word : words) {
            hash = (hash ^ word) * Prime;
        }
        return static_cast<std::size_t>(hash);
    }
};

// Where a draw draws: the colour and depth buffers and their size.
struct Target {
    std::uint32_t color_address = 0;
    std::uint32_t depth_address = 0;
    gpu::PixelFormat color_format = gpu::PixelFormat::Rgba8;
    gpu::DepthFormat depth_format = gpu::DepthFormat::D16;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const Target&) const = default;

    [[nodiscard]] std::uint32_t ColorBytes() const {
        return width * height * static_cast<std::uint32_t>(gpu::BytesPerPixel(color_format));
    }

    [[nodiscard]] std::uint32_t DepthBytes() const {
        return width * height * static_cast<std::uint32_t>(gpu::BytesPerPixel(depth_format));
    }
};

std::optional<Target> TargetOf(const gpu::RegisterFile& registers) {
    constexpr std::uint32_t AddressMask = 0x0FFFFFFF;
    constexpr unsigned AddressShift = 3;
    const std::optional<gpu::PixelFormat> color =
        gpu::ColorBufferFormat(Field(registers.at(reg::ColorFormat), 16, 3));
    const std::optional<gpu::DepthFormat> depth =
        gpu::DepthBufferFormat(Field(registers.at(reg::DepthFormat), 0, 2));
    const std::uint32_t dimensions = registers.at(reg::Dimensions);
    if (!color || !depth) {
        return std::nullopt;
    }
    return Target{.color_address = (registers.at(reg::ColorAddress) & AddressMask) << AddressShift,
                  .depth_address = (registers.at(reg::DepthAddress) & AddressMask) << AddressShift,
                  .color_format = *color,
                  .depth_format = *depth,
                  .width = Field(dimensions, 0, 11),
                  .height = Field(dimensions, 12, 10) + 1};
}

bool Overlaps(std::uint32_t first,
              std::uint32_t first_size,
              std::uint32_t second,
              std::uint32_t second_size) {
    return first < second + second_size && second < first + first_size;
}

// Adds the time between its making and its end to a total.
class Stopwatch {
public:
    explicit Stopwatch(std::chrono::nanoseconds& total)
        : total_(&total), began_(std::chrono::steady_clock::now()) {}

    Stopwatch(const Stopwatch&) = delete;
    Stopwatch& operator=(const Stopwatch&) = delete;
    Stopwatch(Stopwatch&&) = delete;
    Stopwatch& operator=(Stopwatch&&) = delete;

    ~Stopwatch() {
        *total_ += std::chrono::steady_clock::now() - began_;
    }

private:
    std::chrono::nanoseconds* total_;
    std::chrono::steady_clock::time_point began_;
};

// One PICA draw waiting for the list to end.
struct PendingDraw {
    // Whether first_vertex counts in the GPU-shaded vertices' inputs rather
    // than the CPU-shaded vertices.
    bool shaded_on_gpu = false;
    std::uint32_t first_vertex = 0;
    std::uint32_t vertices = 0;
    PipelineKey key;
    Target target;
    PushConstants push;
    VkViewport viewport{};
};

VkCompareOp CompareOp(std::uint32_t function) {
    constexpr std::array<VkCompareOp, 8> Ops = {VK_COMPARE_OP_NEVER,
                                                VK_COMPARE_OP_ALWAYS,
                                                VK_COMPARE_OP_EQUAL,
                                                VK_COMPARE_OP_NOT_EQUAL,
                                                VK_COMPARE_OP_LESS,
                                                VK_COMPARE_OP_LESS_OR_EQUAL,
                                                VK_COMPARE_OP_GREATER,
                                                VK_COMPARE_OP_GREATER_OR_EQUAL};
    return Ops.at(function & 7U);
}

VkStencilOp StencilOp(std::uint32_t action) {
    constexpr std::array<VkStencilOp, 8> Ops = {VK_STENCIL_OP_KEEP,
                                                VK_STENCIL_OP_ZERO,
                                                VK_STENCIL_OP_REPLACE,
                                                VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                                                VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                                VK_STENCIL_OP_INVERT,
                                                VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                                VK_STENCIL_OP_DECREMENT_AND_WRAP};
    return Ops.at(action & 7U);
}

// GPUREG_BLEND_FUNC's factors; the value nobody has described is taken as
// the source colour, as the reference takes it.
VkBlendFactor BlendFactor(std::uint32_t factor) {
    constexpr std::array<VkBlendFactor, 16> Factors = {VK_BLEND_FACTOR_ZERO,
                                                       VK_BLEND_FACTOR_ONE,
                                                       VK_BLEND_FACTOR_SRC_COLOR,
                                                       VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
                                                       VK_BLEND_FACTOR_DST_COLOR,
                                                       VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
                                                       VK_BLEND_FACTOR_SRC_ALPHA,
                                                       VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                       VK_BLEND_FACTOR_DST_ALPHA,
                                                       VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
                                                       VK_BLEND_FACTOR_CONSTANT_COLOR,
                                                       VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
                                                       VK_BLEND_FACTOR_CONSTANT_ALPHA,
                                                       VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
                                                       VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
                                                       VK_BLEND_FACTOR_SRC_COLOR};
    return Factors.at(factor & 0xFU);
}

VkBlendOp BlendOp(std::uint32_t equation) {
    constexpr std::array<VkBlendOp, 8> Ops = {VK_BLEND_OP_ADD,
                                              VK_BLEND_OP_SUBTRACT,
                                              VK_BLEND_OP_REVERSE_SUBTRACT,
                                              VK_BLEND_OP_MIN,
                                              VK_BLEND_OP_MAX,
                                              VK_BLEND_OP_ADD,
                                              VK_BLEND_OP_ADD,
                                              VK_BLEND_OP_ADD};
    return Ops.at(equation & 7U);
}

// GPUREG_LOGIC_OP's sixteen operations in Vulkan's names; the reference's
// table in fragment_stage.cpp is the same order.
VkLogicOp LogicOp(std::uint32_t operation) {
    constexpr std::array<VkLogicOp, 16> Ops = {VK_LOGIC_OP_CLEAR,
                                               VK_LOGIC_OP_AND,
                                               VK_LOGIC_OP_AND_REVERSE,
                                               VK_LOGIC_OP_COPY,
                                               VK_LOGIC_OP_SET,
                                               VK_LOGIC_OP_COPY_INVERTED,
                                               VK_LOGIC_OP_NO_OP,
                                               VK_LOGIC_OP_INVERT,
                                               VK_LOGIC_OP_NAND,
                                               VK_LOGIC_OP_OR,
                                               VK_LOGIC_OP_NOR,
                                               VK_LOGIC_OP_XOR,
                                               VK_LOGIC_OP_EQUIVALENT,
                                               VK_LOGIC_OP_AND_INVERTED,
                                               VK_LOGIC_OP_OR_REVERSE,
                                               VK_LOGIC_OP_OR_INVERTED};
    return Ops.at(operation & 0xFU);
}

// GPUREG_FACECULLING_CONFIG: 1 keeps clockwise triangles, 2 counter-
// clockwise ones. With y flipped, a triangle counter-clockwise for the PICA
// is counter-clockwise for Vulkan too, so that is the front.
VkCullModeFlags CullMode(std::uint32_t cull) {
    switch (cull) {
    case 1:
        return VK_CULL_MODE_FRONT_BIT;
    case 2:
        return VK_CULL_MODE_BACK_BIT;
    default:
        return VK_CULL_MODE_NONE;
    }
}

// An image and its memory on the device.
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// The colour and depth images of one size, and the framebuffer over them.
struct Attachments {
    Image color;
    Image depth;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
};

// Where an image stands with respect to the memory it stands for (ADR 0030).
enum class PartState : std::uint8_t {
    // Memory is the truth and the image is unknown.
    Stale,
    // The image and memory hold the same.
    Clean,
    // The image is newer than memory.
    Dirty
};

struct Part {
    PartState state = PartState::Stale;
    // Memory's bytes as they were when the image last agreed with them.
    std::vector<std::uint8_t> snapshot;
};

// A target kept on the device between lists: its images and the state of its
// colour and its depth, which are written back and dropped apart.
struct Slot {
    Target target;
    Attachments images;
    Part color;
    Part depth;
    std::uint64_t used = 0;
};

// How many targets stay resident; the least recently used is written back to
// make room. The title draws to a handful.
constexpr std::size_t MaxResidentTargets = 16;

}  // namespace

class VulkanGpuBackend::Implementation {
public:
    Implementation(VulkanContext& context,
                   gpu::GpuMemory& memory,
                   gpu::Sampling sampling,
                   VertexShading shading,
                   FragmentShading fragments)
        : context_(&context),
          device_(context.Device()),
          memory_(&memory),
          sampling_(sampling),
          shading_(shading),
          fragments_(fragments),
          vertices_(memory),
          executor_{this},
          processor_(state_, memory, executor_) {}

    Implementation(const Implementation&) = delete;
    Implementation& operator=(const Implementation&) = delete;
    Implementation(Implementation&&) = delete;
    Implementation& operator=(Implementation&&) = delete;
    ~Implementation();

    core::Status Initialise();
    core::Status RunCommandList(std::span<const std::uint8_t> list);

    core::Status BeforeRead(std::uint32_t physical, std::uint32_t size) {
        return MakeTrue({.start = physical, .size = size});
    }

    core::Status BeforeWrite(std::uint32_t physical, std::uint32_t size) {
        return Forget({.start = physical, .size = size}, true, nullptr);
    }

    [[nodiscard]] ports::GpuBackendStatistics Statistics() const {
        return {.lists = lists_,
                .draws = draws_,
                .groups = groups_,
                .uploaded_bytes = uploaded_bytes_,
                .downloaded_bytes = downloaded_bytes_,
                .allocations = allocations_,
                .conflicts = conflicts_,
                .pipelines = pipelines_.size(),
                .fragment_configurations = fragment_numbers_.size(),
                .fallback_draws = fallback_draws_,
                .times = times_,
                .unsupported = unsupported_ || vertices_.Statistics().unsupported_shader};
    }

    [[nodiscard]] std::string DeviceName() const {
        return context_->DeviceName();
    }

    [[nodiscard]] std::uint64_t GpuShadedDraws() const {
        return gpu_shaded_draws_;
    }

    [[nodiscard]] gpu::Sampling SamplingMode() const {
        return sampling_;
    }

    // The command processor's executor, handing on to the implementation.
    struct Executor {
        Implementation* self;

        void Draw(gpu::PicaState& state, bool indexed) const {
            if (self->shading_ == VertexShading::GpuWherePossible &&
                self->vertices_.TakesWholeTriangles(state) && self->QueueShaded(state, indexed)) {
                return;
            }
            self->vertices_.Draw(state, indexed);
            self->Queue(state);
        }

        void ImmediateVertex(gpu::PicaState& state) const {
            self->vertices_.ImmediateVertex(state);
            self->Queue(state);
        }

        void RestartPrimitive(gpu::PicaState& state) const {
            self->vertices_.RestartPrimitive(state);
        }

        void Finalize() {}
    };

private:
    void Queue(const gpu::PicaState& state);
    bool QueueShaded(const gpu::PicaState& state, bool indexed);
    std::optional<PendingDraw> Prepare(const gpu::PicaState& state);
    core::Result<VkShaderModule> ShadedModule(const gpu::PicaState& state, std::uint64_t key);
    // The number of the draw's configuration, new to the backend when it has not
    // been met, with its shader made then if the draws are specialised.
    core::Result<std::uint32_t> FragmentConfigurationNumber(const gpu::RegisterFile& registers);
    void StartFragmentShader(const FragmentConfiguration& configuration, std::uint32_t number);
    void WriteDrawWords(const gpu::PicaState& state, std::uint32_t index);
    std::uint32_t Decode(std::uint32_t address,
                         gpu::TextureFormat format,
                         std::uint32_t width,
                         std::uint32_t height);
    [[nodiscard]] bool ReadsPendingTarget(const gpu::RegisterFile& registers) const;
    core::Status Flush();
    core::Status DrawGroup(std::size_t first, std::size_t end);
    core::Result<VkPipeline>
    BuildPipeline(const PipelineKey& key, VkShaderModule vertex, VkShaderModule fragment) const;
    core::Result<VkPipeline> Pipeline(const PipelineKey& key);
    void StartPipeline(const PipelineKey& key);
    core::Status CreateAttachments(Attachments& made, std::uint32_t width, std::uint32_t height);
    void DestroyAttachments(Attachments& attachments);
    core::Result<Slot*> SlotFor(const Target& target);
    core::Status WriteBack(Slot& slot, bool color, bool depth);
    core::Status MakeTrue(const gpu::MemoryRange& range);
    core::Status Forget(const gpu::MemoryRange& range, bool overwritten, const Slot* except);
    core::Status WriteBackAll();
    void TakeSnapshot(Part& part, const gpu::MemoryRange& range);
    [[nodiscard]] bool MemoryChanged(const Part& part, const gpu::MemoryRange& range) const;
    core::Status CreateImage(Image& image,
                             VkFormat format,
                             VkImageUsageFlags usage,
                             VkImageAspectFlags aspect,
                             std::uint32_t width,
                             std::uint32_t height);
    void DestroyImage(Image& image);
    core::Status CreateLayouts();
    core::Status CreateRenderPass();
    core::Status CreateShaders();
    void UploadColor(const Target& target, std::span<std::uint8_t> color) const;
    void UploadDepth(const Target& target,
                     std::span<std::uint8_t> depth,
                     std::span<std::uint8_t> stencil) const;
    void DownloadColor(const Target& target, std::span<const std::uint8_t> color) const;
    void DownloadDepth(const Target& target,
                       std::span<const std::uint8_t> depth,
                       std::span<const std::uint8_t> stencil) const;

    VulkanContext* context_;
    VkDevice device_;
    gpu::GpuMemory* memory_;
    gpu::Sampling sampling_;
    VertexShading shading_;
    FragmentShading fragments_;
    gpu::PicaState state_;
    gpu::VertexPipeline vertices_;
    Executor executor_;
    gpu::CommandProcessor<Executor> processor_;

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkShaderModule vertex_module_ = VK_NULL_HANDLE;
    VkShaderModule fragment_module_ = VK_NULL_HANDLE;
    std::map<PipelineKey, VkPipeline> pipelines_;
    // The configurations draws have had, by the number the pipelines' keys
    // carry, and the shader made for each when the draws are specialised.
    std::unordered_map<FragmentConfiguration, std::uint32_t, FragmentConfigurationHash>
        fragment_numbers_;
    // The SPIR-V of each configuration's shader, compiled on another thread,
    // by number; and the pipelines being made from it on others.
    std::map<std::uint32_t, std::shared_future<core::Result<std::vector<std::uint32_t>>>>
        compiling_;
    std::map<PipelineKey, std::future<core::Result<VkPipeline>>> building_;
    std::uint64_t fallback_draws_ = 0;
    // Translated vertex programs, by a hash of their code, descriptors and
    // entry point; a null module for one the translation cannot take.
    std::map<std::uint64_t, VkShaderModule> shaded_modules_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::uint64_t clock_ = 0;

    // What the pending draws of the current list need.
    std::vector<PendingDraw> pending_;
    std::vector<float> vertex_data_;
    std::vector<float> input_data_;
    std::vector<std::uint32_t> draw_words_;
    std::vector<std::uint32_t> texels_;
    std::map<std::array<std::uint32_t, 4>, std::uint32_t> decoded_;
    gpu::TextureCache texture_cache_;

    std::uint64_t lists_ = 0;
    std::uint64_t draws_ = 0;
    std::uint64_t gpu_shaded_draws_ = 0;
    std::uint64_t groups_ = 0;
    std::uint64_t uploaded_bytes_ = 0;
    std::uint64_t downloaded_bytes_ = 0;
    std::uint64_t allocations_ = 0;
    std::uint64_t conflicts_ = 0;
    ports::GpuBackendTimes times_;
    bool unsupported_ = false;
};

// ---------------------------------------------------------------------------
// Collecting a list's draws.

// A draw's target, words, push constants and viewport, or nothing for a
// draw the backend does not draw. A texture drawn earlier in the list has to
// be in memory before it is decoded, so the list so far is drawn first.
std::optional<PendingDraw> VulkanGpuBackend::Implementation::Prepare(const gpu::PicaState& state) {
    const gpu::RegisterFile& registers = state.Registers();
    const std::optional<Target> target = TargetOf(registers);
    // Gas and shadow rendering are not implemented.
    if (!target || Field(registers.at(reg::ColorOperation), 0, 2) != 0 || target->width == 0) {
        unsupported_ = true;
        return std::nullopt;
    }
    if (ReadsPendingTarget(registers)) {
        if (const core::Status flushed = Flush(); !flushed) {
            unsupported_ = true;
        }
    }
    PendingDraw draw;
    draw.key = KeyOf(registers);
    const core::Result<std::uint32_t> number = FragmentConfigurationNumber(registers);
    if (!number) {
        unsupported_ = true;
        return std::nullopt;
    }
    if (fragments_ == FragmentShading::Specialised) {
        draw.key.fragment = number.Value();
    }
    draw.target = *target;
    const auto index = static_cast<std::uint32_t>(pending_.size());
    WriteDrawWords(state, index);
    draw.push.draw = index;
    draw.push.sampling = sampling_ == gpu::Sampling::AzaharSoftware ? 1U : 0U;
    draw.push.height = target->height;
    draw.push.clip_enabled = Field(registers.at(reg::ClipEnable), 0, 1);
    for (std::size_t at = 0; at < 4; ++at) {
        draw.push.clip_plane.at(at) = gpu::Float24ToFloat32(
            Field(registers.at(static_cast<std::uint16_t>(reg::ClipCoefficientFirst + at)), 0, 24));
    }
    // The PICA's viewport maps y up from the bottom of the buffer; the
    // vertex shader flips y, so the viewport starts as far from the top as
    // the PICA's ends from the bottom.
    const float half_width =
        gpu::Float24ToFloat32(Field(registers.at(reg::ViewportHalfWidth), 0, 24));
    const float half_height =
        gpu::Float24ToFloat32(Field(registers.at(reg::ViewportHalfHeight), 0, 24));
    const std::uint32_t corner = registers.at(reg::ViewportCorner);
    const auto offset_x =
        static_cast<float>(static_cast<std::int32_t>(Field(corner, 0, 10) << 22U) >> 22);
    const auto offset_y =
        static_cast<float>(static_cast<std::int32_t>(Field(corner, 16, 10) << 22U) >> 22);
    draw.push.viewport = {half_width, half_height, offset_x, offset_y};
    draw.viewport =
        VkViewport{.x = offset_x,
                   .y = static_cast<float>(target->height) - (2.0F * half_height) - offset_y,
                   .width = 2.0F * half_width,
                   .height = 2.0F * half_height,
                   .minDepth = 0.0F,
                   .maxDepth = 1.0F};
    return draw;
}

// A draw whose vertices the CPU shaded and assembled.
void VulkanGpuBackend::Implementation::Queue(const gpu::PicaState& state) {
    std::vector<gpu::PrimitiveAssembler::Triangle>& triangles = vertices_.Triangles();
    ++draws_;
    if (triangles.empty()) {
        return;
    }
    std::optional<PendingDraw> draw = Prepare(state);
    if (!draw) {
        triangles.clear();
        return;
    }
    draw->first_vertex = static_cast<std::uint32_t>(vertex_data_.size() / VertexFloats);
    draw->vertices = static_cast<std::uint32_t>(triangles.size() * 3);
    for (const gpu::PrimitiveAssembler::Triangle& triangle : triangles) {
        for (const gpu::OutputVertex& vertex : triangle) {
            vertex_data_.insert(vertex_data_.end(), vertex.values.begin(), vertex.values.end());
        }
    }
    triangles.clear();
    pending_.push_back(*draw);
}

namespace {

// The hash a vertex program's translation is kept by: FNV-1a over its entry
// point, code and descriptors.
std::uint64_t ProgramKey(const gpu::ShaderUnitState& unit, std::uint32_t entry) {
    constexpr std::uint64_t Basis = 14695981039346656037ULL;
    constexpr std::uint64_t Prime = 1099511628211ULL;
    std::uint64_t key = Basis ^ entry;
    for (const std::uint32_t word : unit.code) {
        key = (key ^ word) * Prime;
    }
    for (const std::uint32_t word : unit.descriptors) {
        key = (key ^ word) * Prime;
    }
    return key;
}

}  // namespace

// A draw whose vertices the GPU shades with the translated program: each
// vertex's inputs loaded and placed as the reference places them, the
// triangles the vertices three at a time. False, and nothing queued, for a
// program the translation cannot take.
bool VulkanGpuBackend::Implementation::QueueShaded(const gpu::PicaState& state, bool indexed) {
    const std::uint32_t entry =
        Field(state.Register(gpu::reg::VertexShaderBase + gpu::reg::shader::EntryPoint), 0, 16);
    const std::uint64_t key = ProgramKey(state.VertexShader(), entry);
    const core::Result<VkShaderModule> module = ShadedModule(state, key);
    if (!module || module.Value() == VK_NULL_HANDLE) {
        return false;
    }
    ++draws_;
    ++gpu_shaded_draws_;
    const std::uint32_t count = state.Register(gpu::reg::VertexCount);
    if (count == 0) {
        return true;
    }
    std::optional<PendingDraw> draw = Prepare(state);
    if (!draw) {
        return true;
    }
    draw->shaded_on_gpu = true;
    draw->key.program = key;
    draw->first_vertex = static_cast<std::uint32_t>(input_data_.size() / InputFloats);
    draw->vertices = count;
    const gpu::VertexLoader loader(state.Registers());
    const std::uint32_t first = state.Register(gpu::reg::FirstIndex);
    for (std::uint32_t at = 0; at < count; ++at) {
        const std::uint32_t vertex =
            indexed ? gpu::ReadIndex(*memory_, state.Registers(), at) : at + first;
        gpu::ShaderRegisters registers;
        gpu::PlaceInputs(state.Registers(),
                         gpu::reg::VertexShaderBase,
                         loader.Load(*memory_, vertex, state.FixedAttributeValues()),
                         registers);
        for (const gpu::Vec4& input : registers.inputs) {
            input_data_.insert(input_data_.end(), input.begin(), input.end());
        }
    }
    pending_.push_back(*draw);
    return true;
}

core::Result<std::uint32_t>
VulkanGpuBackend::Implementation::FragmentConfigurationNumber(const gpu::RegisterFile& registers) {
    const FragmentConfiguration configuration = FragmentConfigurationOf(registers);
    if (const auto found = fragment_numbers_.find(configuration);
        found != fragment_numbers_.end()) {
        return found->second;
    }
    const auto number = static_cast<std::uint32_t>(fragment_numbers_.size() + 1);
    if (fragments_ == FragmentShading::Specialised) {
        StartFragmentShader(configuration, number);
    }
    fragment_numbers_.emplace(configuration, number);
    return number;
}

// The fragment stage with the registers that configure it written in as
// constants, so that the compiler sees which combiner sources, texture
// formats and lighting terms are in use and drops the rest. Compiling is
// started on another thread and waited for by the pipeline that needs it,
// which is made when the group the draw is in is drawn, after the CPU's work
// on the draws that follow.
void VulkanGpuBackend::Implementation::StartFragmentShader(
    const FragmentConfiguration& configuration, std::uint32_t number) {
    // Only the registers that configure the stage are written in. A call with a
    // constant argument folds to its value; one with a loop's counter is
    // resolved when the loop is unrolled.
    std::string definitions = "uint reg(uint id) {\n    switch (id) {\n";
    for (std::size_t at = 0; at < FragmentRegisters.size(); ++at) {
        definitions += std::format(
            "    case {}u: return {}u;\n", FragmentRegisters.at(at), configuration.at(at));
    }
    definitions += "    default: return words[draw * DrawWords + id];\n    }\n}\n";
    std::string source = "#version 450\n" + FloatControlLines(context_->Floats()) +
                         "#define BAKED_REGISTERS\n" + std::string{CommonSource} +
                         std::string{MultiplySource} + definitions +
                         std::string{FragmentShaderSource};
    compiling_.emplace(number, std::async(std::launch::async, [text = std::move(source)] {
                                   return CompileGlsl(text, ShaderStage::Fragment);
                               }).share());
}

core::Result<VkShaderModule>
VulkanGpuBackend::Implementation::ShadedModule(const gpu::PicaState& state, std::uint64_t key) {
    if (const auto found = shaded_modules_.find(key); found != shaded_modules_.end()) {
        return found->second;
    }
    const std::uint32_t entry =
        Field(state.Register(gpu::reg::VertexShaderBase + gpu::reg::shader::EntryPoint), 0, 16);
    const gpu::GlslShader translated = gpu::TranslateShader(state.VertexShader(), entry);
    VkShaderModule module = VK_NULL_HANDLE;
    if (!translated.unsupported) {
        const std::string source =
            "#version 450\n" + FloatControlLines(context_->Floats()) + std::string{CommonSource} +
            std::string{ShadedVertexInputSource} + translated.source +
            std::string{VertexOutputSource} + std::string{ShadedVertexMainSource};
        const core::Result<std::vector<std::uint32_t>> spirv =
            CompileGlsl(source, ShaderStage::Vertex);
        if (!spirv) {
            return spirv.Failure();
        }
        VkShaderModuleCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create.codeSize = spirv.Value().size() * sizeof(std::uint32_t);
        create.pCode = spirv.Value().data();
        if (const core::Status made = Check(
                vkCreateShaderModule(device_, &create, nullptr, &module), "vkCreateShaderModule");
            !made) {
            return made.Failure();
        }
    }
    shaded_modules_.emplace(key, module);
    return module;
}

// The register file, the tables, and each enabled unit's decoded levels.
void VulkanGpuBackend::Implementation::WriteDrawWords(const gpu::PicaState& state,
                                                      std::uint32_t index) {
    const gpu::RegisterFile& registers = state.Registers();
    draw_words_.resize(static_cast<std::size_t>(index + 1) * DrawWords, 0);
    std::uint32_t* words = &draw_words_.at(static_cast<std::size_t>(index) * DrawWords);
    const std::span<std::uint32_t> out{words, DrawWords};
    std::ranges::copy(registers, out.begin());
    std::size_t at = RegisterWords;
    for (const auto& table : state.LightingLutData()) {
        std::ranges::copy(table, out.begin() + static_cast<std::ptrdiff_t>(at));
        at += table.size();
    }
    std::ranges::copy(state.FogLut(), out.begin() + static_cast<std::ptrdiff_t>(at));
    at += FogWords;
    const gpu::ShaderUnitState& vertex_shader = state.VertexShader();
    std::size_t uniform = at + LevelWords;
    for (const gpu::Vec4& value : vertex_shader.float_uniforms) {
        for (const float component : value) {
            out[uniform++] = std::bit_cast<std::uint32_t>(component);
        }
    }
    out[uniform++] = vertex_shader.bool_uniforms;
    std::ranges::copy(vertex_shader.int_uniforms,
                      out.begin() + static_cast<std::ptrdiff_t>(uniform));
    for (std::size_t unit = 0; unit < 3; ++unit) {
        const std::span<std::uint32_t> levels =
            out.subspan(at + (unit * LevelsPerUnit), LevelsPerUnit);
        std::ranges::fill(levels, NoLevel);
        if (Field(registers.at(reg::TextureConfig), static_cast<unsigned>(unit), 1) == 0) {
            continue;
        }
        const std::uint32_t format = Field(registers.at(UnitFormat.at(unit)), 0, 4);
        const std::uint32_t dimensions = registers.at(UnitDimensions.at(unit));
        std::uint32_t width = Field(dimensions, 16, 11);
        std::uint32_t height = Field(dimensions, 0, 11);
        std::uint32_t address = (registers.at(UnitAddress.at(unit)) & 0x0FFFFFFFU) << 3U;
        if (format >= gpu::TextureFormatCount || width == 0 || height == 0 || address == 0) {
            continue;
        }
        const std::uint32_t count = std::min<std::uint32_t>(
            Field(registers.at(UnitLevels.at(unit)), 16, 4) + 1, LevelsPerUnit);
        for (std::uint32_t level = 0; level < count; ++level) {
            const auto texture_format = static_cast<gpu::TextureFormat>(format);
            levels[level] = Decode(address, texture_format, width, height);
            address += static_cast<std::uint32_t>(gpu::TextureBytes(texture_format, width, height));
            width = std::max(width / 2, 8U);
            height = std::max(height / 2, 8U);
        }
    }
}

// A level's texels as the shader reads them, put in the flush's buffer once.
// What was decoded for an earlier list is found again as long as the texture's
// bytes are the same.
std::uint32_t VulkanGpuBackend::Implementation::Decode(std::uint32_t address,
                                                       gpu::TextureFormat format,
                                                       std::uint32_t width,
                                                       std::uint32_t height) {
    const std::array<std::uint32_t, 4> key = {
        address, static_cast<std::uint32_t>(format), width, height};
    if (const auto found = decoded_.find(key); found != decoded_.end()) {
        return found->second;
    }
    const Stopwatch timed{times_.textures};
    const auto size = static_cast<std::uint32_t>(gpu::TextureBytes(format, width, height));
    const std::span<const std::uint8_t> bytes = memory_->Bytes(address, size);
    if (bytes.size() != size) {
        return NoLevel;
    }
    // What a target drew may still be only on the device.
    if (!MakeTrue({.start = address, .size = size})) {
        unsupported_ = true;
    }
    const auto offset = static_cast<std::uint32_t>(texels_.size());
    const std::span<const std::uint32_t> texels =
        texture_cache_.Texels(address, format, width, height, bytes);
    texels_.insert(texels_.end(), texels.begin(), texels.end());
    decoded_.emplace(key, offset);
    return offset;
}

bool VulkanGpuBackend::Implementation::ReadsPendingTarget(
    const gpu::RegisterFile& registers) const {
    for (std::size_t unit = 0; unit < 3; ++unit) {
        if (Field(registers.at(reg::TextureConfig), static_cast<unsigned>(unit), 1) == 0) {
            continue;
        }
        const std::uint32_t address = (registers.at(UnitAddress.at(unit)) & 0x0FFFFFFFU) << 3U;
        const std::uint32_t dimensions = registers.at(UnitDimensions.at(unit));
        // A generous size: the largest format's bytes, every level.
        const std::uint32_t size = Field(dimensions, 16, 11) * Field(dimensions, 0, 11) * 4 * 2;
        for (const PendingDraw& draw : pending_) {
            if (Overlaps(address, size, draw.target.color_address, draw.target.ColorBytes()) ||
                Overlaps(address, size, draw.target.depth_address, draw.target.DepthBytes())) {
                return true;
            }
        }
    }
    return false;
}

core::Status VulkanGpuBackend::Implementation::RunCommandList(std::span<const std::uint8_t> list) {
    const Stopwatch total{times_.total};
    ++lists_;
    const core::Result<gpu::CommandListRun> run = processor_.Run(list);
    core::Status flushed = Flush();
    if (!run) {
        return run.Failure();
    }
    return flushed;
}

// ---------------------------------------------------------------------------
// Drawing what is pending.

// Draws in runs that share a target. A run's images stay on the device
// afterwards (ADR 0030), so nothing is copied back here.
core::Status VulkanGpuBackend::Implementation::Flush() {
    std::size_t first = 0;
    core::Status status;
    while (status && first < pending_.size()) {
        std::size_t end = first + 1;
        while (end < pending_.size() && pending_[end].target == pending_[first].target) {
            ++end;
        }
        status = DrawGroup(first, end);
        first = end;
    }
    pending_.clear();
    vertex_data_.clear();
    input_data_.clear();
    draw_words_.clear();
    texels_.clear();
    decoded_.clear();
    return status;
}

// ---------------------------------------------------------------------------
// Keeping the targets resident (ADR 0030).

namespace {

gpu::MemoryRange ColorRange(const Target& target) {
    return {.start = target.color_address, .size = target.ColorBytes()};
}

gpu::MemoryRange DepthRange(const Target& target) {
    return {.start = target.depth_address, .size = target.DepthBytes()};
}

bool Overlaps(const gpu::MemoryRange& first, const gpu::MemoryRange& second) {
    return Overlaps(first.start, first.size, second.start, second.size);
}

// Whether memory is one pixel repeated, as a memory fill leaves a buffer. The
// pixels' order, tiled or not, makes no difference to such a buffer, so it can
// be decoded once.
bool IsOnePixelRepeated(std::span<const std::uint8_t> bytes, std::size_t pixel_bytes) {
    return bytes.size() > pixel_bytes &&
           std::memcmp(bytes.data(), bytes.data() + pixel_bytes, bytes.size() - pixel_bytes) == 0;
}

bool Covers(const gpu::MemoryRange& outer, const gpu::MemoryRange& inner) {
    return outer.start <= inner.start && inner.start + inner.size <= outer.start + outer.size;
}

}  // namespace

void VulkanGpuBackend::Implementation::TakeSnapshot(Part& part, const gpu::MemoryRange& range) {
    const std::span<const std::uint8_t> bytes =
        std::as_const(*memory_).Bytes(range.start, range.size);
    part.snapshot.assign(bytes.begin(), bytes.end());
}

// Whether memory is not what the part last agreed with it on. Memory the GPU
// cannot see is never said to have changed.
bool VulkanGpuBackend::Implementation::MemoryChanged(const Part& part,
                                                     const gpu::MemoryRange& range) const {
    const std::span<const std::uint8_t> bytes =
        std::as_const(*memory_).Bytes(range.start, range.size);
    return !bytes.empty() && (part.snapshot.size() != bytes.size() ||
                              std::memcmp(part.snapshot.data(), bytes.data(), bytes.size()) != 0);
}

core::Result<Slot*> VulkanGpuBackend::Implementation::SlotFor(const Target& target) {
    for (const std::unique_ptr<Slot>& slot : slots_) {
        if (slot->target == target) {
            slot->used = ++clock_;
            return slot.get();
        }
    }
    if (slots_.size() >= MaxResidentTargets) {
        const auto oldest = std::ranges::min_element(
            slots_, [](const auto& a, const auto& b) { return a->used < b->used; });
        if (const core::Status written = WriteBack(**oldest, true, true); !written) {
            return written.Failure();
        }
        DestroyAttachments((*oldest)->images);
        slots_.erase(oldest);
    }
    auto made = std::make_unique<Slot>();
    made->target = target;
    made->used = ++clock_;
    slots_.push_back(std::move(made));
    Slot& slot = *slots_.back();
    if (const core::Status created = CreateAttachments(slot.images, target.width, target.height);
        !created) {
        slots_.pop_back();
        return created.Failure();
    }
    return &slot;
}

// The dirty parts asked for, converted from their images back into memory.
core::Status VulkanGpuBackend::Implementation::WriteBack(Slot& slot, bool color, bool depth) {
    color = color && slot.color.state == PartState::Dirty;
    depth = depth && slot.depth.state == PartState::Dirty;
    if (!color && !depth) {
        return {};
    }
    const Target& target = slot.target;
    const VkDeviceSize pixels = static_cast<VkDeviceSize>(target.width) * target.height;
    const VkDeviceSize color_size = pixels * 4;
    const VkDeviceSize depth_size = pixels * 4;
    const VkDeviceSize stencil_size = pixels;
    std::optional<Stopwatch> allocating{times_.allocate};
    ++allocations_;
    auto staging =
        HostBuffer::Create(*context_,
                           color_size + depth_size + stencil_size,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           HostUse::ReadBack);
    allocating.reset();
    if (!staging) {
        return staging.Failure();
    }
    const VkExtent3D extent{.width = target.width, .height = target.height, .depth = 1};
    const auto region = [&](VkDeviceSize offset, VkImageAspectFlags aspect) {
        return VkBufferImageCopy{.bufferOffset = offset,
                                 .bufferRowLength = 0,
                                 .bufferImageHeight = 0,
                                 .imageSubresource = {.aspectMask = aspect,
                                                      .mipLevel = 0,
                                                      .baseArrayLayer = 0,
                                                      .layerCount = 1},
                                 .imageOffset = {.x = 0, .y = 0, .z = 0},
                                 .imageExtent = extent};
    };
    const VkBufferImageCopy color_region = region(0, VK_IMAGE_ASPECT_COLOR_BIT);
    const std::array<VkBufferImageCopy, 2> depth_regions = {
        region(color_size, VK_IMAGE_ASPECT_DEPTH_BIT),
        region(color_size + depth_size, VK_IMAGE_ASPECT_STENCIL_BIT)};
    VkBuffer staging_buffer = staging.Value()->Buffer();
    std::optional<Stopwatch> waiting{times_.wait};
    const core::Status copied = context_->Submit([&](VkCommandBuffer commands) {
        if (color) {
            vkCmdCopyImageToBuffer(commands,
                                   slot.images.color.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   staging_buffer,
                                   1,
                                   &color_region);
        }
        if (depth) {
            vkCmdCopyImageToBuffer(commands,
                                   slot.images.depth.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   staging_buffer,
                                   2,
                                   depth_regions.data());
        }
        VkMemoryBarrier host{};
        host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commands,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT,
                             0,
                             1,
                             &host,
                             0,
                             nullptr,
                             0,
                             nullptr);
    });
    waiting.reset();
    if (!copied) {
        return copied;
    }
    const Stopwatch downloading{times_.download};
    const std::span<std::uint8_t> stage = staging.Value()->Bytes();
    if (color) {
        DownloadColor(target, stage.first(color_size));
        TakeSnapshot(slot.color, ColorRange(target));
        slot.color.state = PartState::Clean;
        downloaded_bytes_ += target.ColorBytes();
    }
    if (depth) {
        DownloadDepth(target,
                      stage.subspan(color_size, depth_size),
                      stage.subspan(color_size + depth_size, stencil_size));
        TakeSnapshot(slot.depth, DepthRange(target));
        slot.depth.state = PartState::Clean;
        downloaded_bytes_ += target.DepthBytes();
    }
    return {};
}

// Memory is made true over a range for whoever is about to read it.
core::Status VulkanGpuBackend::Implementation::MakeTrue(const gpu::MemoryRange& range) {
    for (const std::unique_ptr<Slot>& slot : slots_) {
        const bool color =
            slot->color.state == PartState::Dirty && Overlaps(range, ColorRange(slot->target));
        const bool depth =
            slot->depth.state == PartState::Dirty && Overlaps(range, DepthRange(slot->target));
        if (const core::Status written = WriteBack(*slot, color, depth); !written) {
            return written;
        }
    }
    return {};
}

// What the images hold over a range stops being the truth. A dirty part is
// written back first, unless the range is overwritten whole and the bytes it
// holds are about to be replaced.
core::Status VulkanGpuBackend::Implementation::Forget(const gpu::MemoryRange& range,
                                                      bool overwritten,
                                                      const Slot* except) {
    for (const std::unique_ptr<Slot>& slot : slots_) {
        if (slot.get() == except) {
            continue;
        }
        for (const bool color : {true, false}) {
            Part& part = color ? slot->color : slot->depth;
            const gpu::MemoryRange held =
                color ? ColorRange(slot->target) : DepthRange(slot->target);
            if (!Overlaps(range, held)) {
                continue;
            }
            if (part.state == PartState::Dirty && !(overwritten && Covers(range, held))) {
                if (const core::Status written = WriteBack(*slot, color, !color); !written) {
                    return written;
                }
            }
            part.state = PartState::Stale;
        }
    }
    return {};
}

core::Status VulkanGpuBackend::Implementation::WriteBackAll() {
    core::Status status;
    for (const std::unique_ptr<Slot>& slot : slots_) {
        if (const core::Status written = WriteBack(*slot, true, true); !written && status) {
            status = written;
        }
    }
    return status;
}

core::Status VulkanGpuBackend::Implementation::DrawGroup(std::size_t first, std::size_t end) {
    const Target target = pending_[first].target;
    ++groups_;
    const core::Result<Slot*> found = SlotFor(target);
    if (!found) {
        return found.Failure();
    }
    Slot& slot = *found.Value();
    const gpu::MemoryRange color_range = ColorRange(target);
    const gpu::MemoryRange depth_range = DepthRange(target);

    // A part whose memory someone changed without saying so is taken from
    // memory again, as a dirty one is counted as a conflict (ADR 0030).
    for (const bool color : {true, false}) {
        Part& part = color ? slot.color : slot.depth;
        if (part.state != PartState::Stale &&
            MemoryChanged(part, color ? color_range : depth_range)) {
            conflicts_ += part.state == PartState::Dirty ? 1 : 0;
            part.state = PartState::Stale;
        }
    }
    const bool upload_color = slot.color.state == PartState::Stale;
    const bool upload_depth = slot.depth.state == PartState::Stale;
    if (upload_color) {
        if (const core::Status made = MakeTrue(color_range); !made) {
            return made;
        }
    }
    if (upload_depth) {
        if (const core::Status made = MakeTrue(depth_range); !made) {
            return made;
        }
    }
    // The draws change the memory under any other target that lies over this.
    for (const gpu::MemoryRange& range : {color_range, depth_range}) {
        if (const core::Status forgotten = Forget(range, false, &slot); !forgotten) {
            return forgotten;
        }
    }

    const VkDeviceSize pixels = static_cast<VkDeviceSize>(target.width) * target.height;
    const VkDeviceSize color_size = pixels * 4;
    const VkDeviceSize depth_size = pixels * 4;
    const VkDeviceSize stencil_size = pixels;
    constexpr std::uint64_t BuffersPerGroup = 5;
    allocations_ += BuffersPerGroup;
    std::optional<Stopwatch> allocating{times_.allocate};
    auto staging =
        HostBuffer::Create(*context_,
                           color_size + depth_size + stencil_size,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           HostUse::ReadBack);
    auto vertices =
        HostBuffer::Create(*context_,
                           std::max<VkDeviceSize>(vertex_data_.size() * sizeof(float), 16),
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    auto words = HostBuffer::Create(*context_,
                                    std::max<VkDeviceSize>(draw_words_.size() * 4, 16),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto texels = HostBuffer::Create(*context_,
                                     std::max<VkDeviceSize>(texels_.size() * 4, 16),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto inputs = HostBuffer::Create(*context_,
                                     std::max<VkDeviceSize>(input_data_.size() * sizeof(float), 16),
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    allocating.reset();
    for (const auto* buffer : {&staging, &vertices, &inputs, &words, &texels}) {
        if (!*buffer) {
            return buffer->Failure();
        }
    }
    const std::span<std::uint8_t> stage = staging.Value()->Bytes();
    {
        const Stopwatch uploading{times_.upload};
        if (upload_color) {
            UploadColor(target, stage.first(color_size));
            TakeSnapshot(slot.color, color_range);
            uploaded_bytes_ += target.ColorBytes();
        }
        if (upload_depth) {
            UploadDepth(target,
                        stage.subspan(color_size, depth_size),
                        stage.subspan(color_size + depth_size, stencil_size));
            TakeSnapshot(slot.depth, depth_range);
            uploaded_bytes_ += target.DepthBytes();
        }
    }
    std::memcpy(
        vertices.Value()->Bytes().data(), vertex_data_.data(), vertex_data_.size() * sizeof(float));
    std::memcpy(
        inputs.Value()->Bytes().data(), input_data_.data(), input_data_.size() * sizeof(float));
    std::memcpy(words.Value()->Bytes().data(), draw_words_.data(), draw_words_.size() * 4);
    std::memcpy(texels.Value()->Bytes().data(), texels_.data(), texels_.size() * 4);

    const std::array<VkDescriptorBufferInfo, 2> infos = {
        VkDescriptorBufferInfo{
            .buffer = words.Value()->Buffer(), .offset = 0, .range = VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{
            .buffer = texels.Value()->Buffer(), .offset = 0, .range = VK_WHOLE_SIZE}};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::uint32_t binding = 0; binding < 2; ++binding) {
        writes.at(binding).sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes.at(binding).dstSet = descriptor_set_;
        writes.at(binding).dstBinding = binding;
        writes.at(binding).descriptorCount = 1;
        writes.at(binding).descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes.at(binding).pBufferInfo = &infos.at(binding);
    }
    vkUpdateDescriptorSets(device_, 2, writes.data(), 0, nullptr);

    std::vector<VkPipeline> pipelines;
    for (std::size_t at = first; at < end; ++at) {
        const core::Result<VkPipeline> pipeline = Pipeline(pending_[at].key);
        if (!pipeline) {
            return pipeline.Failure();
        }
        pipelines.push_back(pipeline.Value());
    }

    Attachments& images = slot.images;
    VkBuffer staging_buffer = staging.Value()->Buffer();
    VkBuffer vertex_buffer = vertices.Value()->Buffer();
    VkBuffer input_buffer = inputs.Value()->Buffer();
    std::optional<Stopwatch> waiting{times_.wait};
    core::Status drawn = context_->Submit([&](VkCommandBuffer commands) {
        const auto barrier = [&](VkImage image,
                                 VkImageAspectFlags aspect,
                                 VkImageLayout from,
                                 VkImageLayout to,
                                 VkAccessFlags source,
                                 VkAccessFlags destination) {
            VkImageMemoryBarrier change{};
            change.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            change.srcAccessMask = source;
            change.dstAccessMask = destination;
            change.oldLayout = from;
            change.newLayout = to;
            change.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            change.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            change.image = image;
            change.subresourceRange = {.aspectMask = aspect,
                                       .baseMipLevel = 0,
                                       .levelCount = 1,
                                       .baseArrayLayer = 0,
                                       .layerCount = 1};
            vkCmdPipelineBarrier(commands,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0,
                                 0,
                                 nullptr,
                                 0,
                                 nullptr,
                                 1,
                                 &change);
        };
        constexpr VkImageAspectFlags DepthStencil =
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        constexpr VkAccessFlags ColorUse =
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        constexpr VkAccessFlags DepthUse = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        const VkExtent3D extent{.width = target.width, .height = target.height, .depth = 1};
        const auto region = [&](VkDeviceSize offset, VkImageAspectFlags aspect) {
            return VkBufferImageCopy{.bufferOffset = offset,
                                     .bufferRowLength = 0,
                                     .bufferImageHeight = 0,
                                     .imageSubresource = {.aspectMask = aspect,
                                                          .mipLevel = 0,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = 1},
                                     .imageOffset = {.x = 0, .y = 0, .z = 0},
                                     .imageExtent = extent};
        };
        // A part taken from memory is copied in; one that was kept comes from
        // the layout the last pass left it in. Either way it ends as an
        // attachment.
        if (upload_color) {
            const VkBufferImageCopy color_region = region(0, VK_IMAGE_ASPECT_COLOR_BIT);
            barrier(images.color.image,
                    VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdCopyBufferToImage(commands,
                                   staging_buffer,
                                   images.color.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1,
                                   &color_region);
            barrier(images.color.image,
                    VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    ColorUse);
        } else {
            barrier(images.color.image,
                    VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    0,
                    ColorUse);
        }
        if (upload_depth) {
            const std::array<VkBufferImageCopy, 2> depth_regions = {
                region(color_size, VK_IMAGE_ASPECT_DEPTH_BIT),
                region(color_size + depth_size, VK_IMAGE_ASPECT_STENCIL_BIT)};
            barrier(images.depth.image,
                    DepthStencil,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdCopyBufferToImage(commands,
                                   staging_buffer,
                                   images.depth.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   2,
                                   depth_regions.data());
            barrier(images.depth.image,
                    DepthStencil,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    DepthUse);
        } else {
            barrier(images.depth.image,
                    DepthStencil,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    0,
                    DepthUse);
        }

        VkRenderPassBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = render_pass_;
        begin.framebuffer = images.framebuffer;
        begin.renderArea = {.offset = {.x = 0, .y = 0},
                            .extent = {.width = target.width, .height = target.height}};
        vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindDescriptorSets(commands,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_,
                                0,
                                1,
                                &descriptor_set_,
                                0,
                                nullptr);
        const VkDeviceSize no_offset = 0;
        const VkRect2D scissor{.offset = {.x = 0, .y = 0},
                               .extent = {.width = target.width, .height = target.height}};
        for (std::size_t at = first; at < end; ++at) {
            const PendingDraw& draw = pending_[at];
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.at(at - first));
            vkCmdBindVertexBuffers(
                commands, 0, 1, draw.shaded_on_gpu ? &input_buffer : &vertex_buffer, &no_offset);
            vkCmdSetViewport(commands, 0, 1, &draw.viewport);
            vkCmdSetScissor(commands, 0, 1, &scissor);
            vkCmdPushConstants(commands,
                               pipeline_layout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0,
                               sizeof(PushConstants),
                               &draw.push);
            vkCmdDraw(commands, draw.vertices, 1, draw.first_vertex, 0);
        }
        vkCmdEndRenderPass(commands);
    });
    waiting.reset();
    if (!drawn) {
        return drawn;
    }
    slot.color.state = PartState::Dirty;
    slot.depth.state = PartState::Dirty;
    return {};
}

// The buffers' pixels from memory into rows top first, as Vulkan's images
// hold them: colour as RGBA8, depth as a float of the stored integer over the
// largest, stencil as bytes. A buffer outside memory reads as zero.
void VulkanGpuBackend::Implementation::UploadColor(const Target& target,
                                                   std::span<std::uint8_t> color) const {
    const std::size_t color_bytes = gpu::BytesPerPixel(target.color_format);
    const std::span<const std::uint8_t> colors =
        memory_->Bytes(target.color_address, target.ColorBytes());
    if (IsOnePixelRepeated(colors, color_bytes)) {
        const gpu::Color value = gpu::DecodePixel(target.color_format, colors.first(color_bytes));
        for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(target.width) * target.height;
             ++pixel) {
            color[pixel * 4] = value.red;
            color[(pixel * 4) + 1] = value.green;
            color[(pixel * 4) + 2] = value.blue;
            color[(pixel * 4) + 3] = value.alpha;
        }
        return;
    }
    // Memory the GPU cannot see, or a size that is not whole tiles, is black.
    if (colors.empty() ||
        !gpu::DecodeTiledImage(target.color_format, target.width, target.height, colors, color)) {
        std::ranges::fill(color, 0);
    }
}

void VulkanGpuBackend::Implementation::UploadDepth(const Target& target,
                                                   std::span<std::uint8_t> depth,
                                                   std::span<std::uint8_t> stencil) const {
    const std::size_t depth_bytes = gpu::BytesPerPixel(target.depth_format);
    const std::span<const std::uint8_t> depths =
        memory_->Bytes(target.depth_address, target.DepthBytes());
    const auto maximum = static_cast<float>(gpu::MaximumDepth(target.depth_format));
    if (IsOnePixelRepeated(depths, depth_bytes)) {
        const std::span<const std::uint8_t> stored = depths.first(depth_bytes);
        const float stored_depth =
            static_cast<float>(gpu::DecodeDepth(target.depth_format, stored)) / maximum;
        const std::uint8_t stored_stencil =
            target.depth_format == gpu::DepthFormat::D24S8 ? gpu::DecodeStencil(stored) : 0;
        for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(target.width) * target.height;
             ++pixel) {
            std::memcpy(&depth[pixel * 4], &stored_depth, sizeof(float));
            stencil[pixel] = stored_stencil;
        }
        return;
    }
    for (std::uint32_t y = 0; y < target.height; ++y) {
        for (std::uint32_t x = 0; x < target.width; ++x) {
            const std::size_t pixel = (static_cast<std::size_t>(y) * target.width) + x;
            float stored_depth = 0.0F;
            std::uint8_t stored_stencil = 0;
            if (!depths.empty()) {
                const std::span<const std::uint8_t> stored =
                    depths.subspan(gpu::TiledOffset(x, y, target.width, depth_bytes), depth_bytes);
                stored_depth =
                    static_cast<float>(gpu::DecodeDepth(target.depth_format, stored)) / maximum;
                if (target.depth_format == gpu::DepthFormat::D24S8) {
                    stored_stencil = gpu::DecodeStencil(stored);
                }
            }
            std::memcpy(&depth[pixel * 4], &stored_depth, sizeof(float));
            stencil[pixel] = stored_stencil;
        }
    }
}

// And back: colour encoded in the buffer's format, depth rounded to the
// nearest integer the format holds.
void VulkanGpuBackend::Implementation::DownloadColor(const Target& target,
                                                     std::span<const std::uint8_t> color) const {
    const std::span<std::uint8_t> colors =
        memory_->Bytes(target.color_address, target.ColorBytes());
    if (colors.empty()) {
        return;
    }
    static_cast<void>(
        gpu::EncodeTiledImage(target.color_format, target.width, target.height, color, colors));
}

void VulkanGpuBackend::Implementation::DownloadDepth(const Target& target,
                                                     std::span<const std::uint8_t> depth,
                                                     std::span<const std::uint8_t> stencil) const {
    const std::size_t depth_bytes = gpu::BytesPerPixel(target.depth_format);
    const std::span<std::uint8_t> depths =
        memory_->Bytes(target.depth_address, target.DepthBytes());
    if (depths.empty()) {
        return;
    }
    const auto maximum = static_cast<double>(gpu::MaximumDepth(target.depth_format));
    for (std::uint32_t y = 0; y < target.height; ++y) {
        for (std::uint32_t x = 0; x < target.width; ++x) {
            const std::size_t pixel = (static_cast<std::size_t>(y) * target.width) + x;
            float stored = 0.0F;
            std::memcpy(&stored, &depth[pixel * 4], sizeof(float));
            const std::span<std::uint8_t> target_bytes =
                depths.subspan(gpu::TiledOffset(x, y, target.width, depth_bytes), depth_bytes);
            gpu::EncodeDepth(
                target.depth_format,
                static_cast<std::uint32_t>(std::lround(static_cast<double>(stored) * maximum)),
                target_bytes);
            if (target.depth_format == gpu::DepthFormat::D24S8) {
                gpu::EncodeStencil(stencil[pixel], target_bytes);
            }
        }
    }
}

core::Status VulkanGpuBackend::Implementation::Initialise() {
    if (!context_->CanDraw()) {
        return core::Error{
            std::format("{} lacks logic operations or clip distances", context_->DeviceName())};
    }
    for (const auto& step : {&Implementation::CreateLayouts,
                             &Implementation::CreateRenderPass,
                             &Implementation::CreateShaders}) {
        if (const core::Status status = (this->*step)(); !status) {
            return status;
        }
    }
    return {};
}

core::Status VulkanGpuBackend::Implementation::CreateLayouts() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (std::uint32_t binding = 0; binding < 2; ++binding) {
        bindings.at(binding).binding = binding;
        bindings.at(binding).descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings.at(binding).descriptorCount = 1;
        bindings.at(binding).stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set{};
    set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set.bindingCount = 2;
    set.pBindings = bindings.data();
    if (const core::Status made =
            Check(vkCreateDescriptorSetLayout(device_, &set, nullptr, &set_layout_),
                  "vkCreateDescriptorSetLayout");
        !made) {
        return made;
    }
    const VkPushConstantRange push{.stageFlags =
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   .offset = 0,
                                   .size = sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &set_layout_;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (const core::Status made =
            Check(vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_),
                  "vkCreatePipelineLayout");
        !made) {
        return made;
    }
    const VkDescriptorPoolSize size{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                    .descriptorCount = 2};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (const core::Status made =
            Check(vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_),
                  "vkCreateDescriptorPool");
        !made) {
        return made;
    }
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = descriptor_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set_layout_;
    return Check(vkAllocateDescriptorSets(device_, &allocate, &descriptor_set_),
                 "vkAllocateDescriptorSets");
}

// One render pass for every draw: the buffers loaded as memory had them and
// left for copying back.
core::Status VulkanGpuBackend::Implementation::CreateRenderPass() {
    const VkAttachmentDescription color_attachment{
        .flags = 0,
        .format = ColorImageFormat,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    const VkAttachmentDescription depth_attachment{
        .flags = 0,
        .format = DepthImageFormat,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    const std::array<VkAttachmentDescription, 2> attachments = {color_attachment, depth_attachment};
    const VkAttachmentReference color{.attachment = 0,
                                      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth{.attachment = 1,
                                      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pDepthStencilAttachment = &depth;
    VkRenderPassCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create.attachmentCount = 2;
    create.pAttachments = attachments.data();
    create.subpassCount = 1;
    create.pSubpasses = &subpass;
    return Check(vkCreateRenderPass(device_, &create, nullptr, &render_pass_),
                 "vkCreateRenderPass");
}

core::Status VulkanGpuBackend::Implementation::CreateShaders() {
    const std::string preamble = "#version 450\n" + FloatControlLines(context_->Floats()) +
                                 std::string{CommonSource} + std::string{MultiplySource};
    const core::Result<std::vector<std::uint32_t>> vertex =
        CompileGlsl(preamble + std::string{VertexOutputSource} + std::string{VertexShaderSource},
                    ShaderStage::Vertex);
    if (!vertex) {
        return vertex.Failure();
    }
    const core::Result<std::vector<std::uint32_t>> fragment =
        CompileGlsl(preamble + std::string{FragmentShaderSource}, ShaderStage::Fragment);
    if (!fragment) {
        return fragment.Failure();
    }
    for (const auto& [code, module] : {std::pair{&vertex.Value(), &vertex_module_},
                                       std::pair{&fragment.Value(), &fragment_module_}}) {
        VkShaderModuleCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create.codeSize = code->size() * sizeof(std::uint32_t);
        create.pCode = code->data();
        if (const core::Status made = Check(vkCreateShaderModule(device_, &create, nullptr, module),
                                            "vkCreateShaderModule");
            !made) {
            return made;
        }
    }
    return {};
}

// A pipeline for one combination of the output merger's settings.
core::Result<VkPipeline> VulkanGpuBackend::Implementation::BuildPipeline(
    const PipelineKey& key, VkShaderModule vertex, VkShaderModule fragment) const {
    const auto& w = key.words;
    const std::uint32_t cull = w[0];
    const std::uint32_t operation = w[1];
    const std::uint32_t blend = w[2];
    const std::uint32_t stencil_test = w[5];
    const std::uint32_t stencil_operation = w[6];
    const std::uint32_t mask = w[7];
    const bool color_writes = w[8] != 0;
    const bool depth_writes = w[9] != 0;
    const bool has_stencil = w[10] == static_cast<std::uint32_t>(gpu::DepthFormat::D24S8);

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages = {
        VkPipelineShaderStageCreateInfo{.sType =
                                            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        .pNext = nullptr,
                                        .flags = 0,
                                        .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                        .module = vertex,
                                        .pName = "main",
                                        .pSpecializationInfo = nullptr},
        VkPipelineShaderStageCreateInfo{.sType =
                                            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        .pNext = nullptr,
                                        .flags = 0,
                                        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                        .module = fragment,
                                        .pName = "main",
                                        .pSpecializationInfo = nullptr}};
    // Vertices shaded on the CPU come as their 24 slots, those the GPU
    // shades as their 16 input registers; four floats to a location.
    const std::size_t floats = key.program == 0 ? VertexFloats : InputFloats;
    const VkVertexInputBindingDescription binding{
        .binding = 0,
        .stride = static_cast<std::uint32_t>(floats * sizeof(float)),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attributes(floats / 4);
    for (std::uint32_t location = 0; location < attributes.size(); ++location) {
        attributes.at(location) = {.location = location,
                                   .binding = 0,
                                   .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                   .offset =
                                       location * 4 * static_cast<std::uint32_t>(sizeof(float))};
    }
    VkPipelineVertexInputStateCreateInfo input{};
    input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    input.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = CullMode(cull);
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0F;
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 0.0F,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE};

    // Depth is written even with its test off, so the test stays on and
    // always passes.
    const bool depth_test = Field(mask, 0, 1) != 0;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthCompareOp = depth_test ? CompareOp(Field(mask, 4, 3)) : VK_COMPARE_OP_ALWAYS;
    depth.depthWriteEnable = depth_writes && Field(mask, 12, 1) != 0 ? VK_TRUE : VK_FALSE;
    depth.stencilTestEnable = has_stencil && Field(stencil_test, 0, 1) != 0 ? VK_TRUE : VK_FALSE;
    depth.front = {.failOp = StencilOp(Field(stencil_operation, 0, 3)),
                   .passOp = StencilOp(Field(stencil_operation, 8, 3)),
                   .depthFailOp = StencilOp(Field(stencil_operation, 4, 3)),
                   .compareOp = CompareOp(Field(stencil_test, 4, 3)),
                   .compareMask = Field(stencil_test, 24, 8),
                   .writeMask = depth_writes ? Field(stencil_test, 8, 8) : 0U,
                   .reference = Field(stencil_test, 16, 8)};
    depth.back = depth.front;

    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = Field(operation, 8, 1) != 0 ? VK_TRUE : VK_FALSE;
    attachment.colorBlendOp = BlendOp(Field(blend, 0, 3));
    attachment.alphaBlendOp = BlendOp(Field(blend, 8, 3));
    attachment.srcColorBlendFactor = BlendFactor(Field(blend, 16, 4));
    attachment.dstColorBlendFactor = BlendFactor(Field(blend, 20, 4));
    attachment.srcAlphaBlendFactor = BlendFactor(Field(blend, 24, 4));
    attachment.dstAlphaBlendFactor = BlendFactor(Field(blend, 28, 4));
    attachment.colorWriteMask = color_writes ? Field(mask, 8, 4) : 0U;
    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.logicOpEnable = attachment.blendEnable == VK_TRUE ? VK_FALSE : VK_TRUE;
    blending.logicOp = LogicOp(Field(w[3], 0, 4));
    blending.attachmentCount = 1;
    blending.pAttachments = &attachment;
    const std::span<float, 4> constants{blending.blendConstants};
    for (std::size_t channel = 0; channel < constants.size(); ++channel) {
        constants[channel] =
            static_cast<float>(Field(w[4], 8 * static_cast<unsigned>(channel), 8)) / 255.0F;
    }
    const std::array<VkDynamicState, 2> dynamic = {VK_DYNAMIC_STATE_VIEWPORT,
                                                   VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamics{};
    dynamics.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamics.dynamicStateCount = 2;
    dynamics.pDynamicStates = dynamic.data();

    VkGraphicsPipelineCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    create.stageCount = 2;
    create.pStages = stages.data();
    create.pVertexInputState = &input;
    create.pInputAssemblyState = &assembly;
    create.pViewportState = &viewport;
    create.pRasterizationState = &raster;
    create.pMultisampleState = &multisample;
    create.pDepthStencilState = &depth;
    create.pColorBlendState = &blending;
    create.pDynamicState = &dynamics;
    create.layout = pipeline_layout_;
    create.renderPass = render_pass_;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (const core::Status made = Check(
            vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline),
            "vkCreateGraphicsPipelines");
        !made) {
        return made.Failure();
    }
    return pipeline;
}

// The pipeline a draw is drawn with. One made for the draw's configuration is
// built on another thread, because the driver takes about a second to compile
// the shader and the game would stand still for it; until it is ready the draw
// takes the pipeline of the generic shader, which draws the same pixels
// (ADR 0031).
core::Result<VkPipeline> VulkanGpuBackend::Implementation::Pipeline(const PipelineKey& key) {
    if (const auto found = pipelines_.find(key); found != pipelines_.end()) {
        return found->second;
    }
    if (key.fragment != 0) {
        if (const auto building = building_.find(key); building != building_.end()) {
            if (building->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                const core::Result<VkPipeline> built = building->second.get();
                building_.erase(building);
                if (!built) {
                    return built.Failure();
                }
                pipelines_.emplace(key, built.Value());
                return built.Value();
            }
        } else {
            StartPipeline(key);
        }
        ++fallback_draws_;
        PipelineKey generic = key;
        generic.fragment = 0;
        return Pipeline(generic);
    }
    const VkShaderModule vertex =
        key.program == 0 ? vertex_module_ : shaded_modules_.at(key.program);
    const Stopwatch making{times_.pipelines};
    const core::Result<VkPipeline> pipeline = BuildPipeline(key, vertex, fragment_module_);
    if (pipeline) {
        pipelines_.emplace(key, pipeline.Value());
    }
    return pipeline;
}

void VulkanGpuBackend::Implementation::StartPipeline(const PipelineKey& key) {
    const VkShaderModule vertex =
        key.program == 0 ? vertex_module_ : shaded_modules_.at(key.program);
    building_.emplace(
        key,
        std::async(std::launch::async, [this, key, vertex, spirv = compiling_.at(key.fragment)] {
            const core::Result<std::vector<std::uint32_t>>& code = spirv.get();
            if (!code) {
                return core::Result<VkPipeline>{code.Failure()};
            }
            VkShaderModuleCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            create.codeSize = code.Value().size() * sizeof(std::uint32_t);
            create.pCode = code.Value().data();
            VkShaderModule fragment = VK_NULL_HANDLE;
            if (const core::Status made =
                    Check(vkCreateShaderModule(device_, &create, nullptr, &fragment),
                          "vkCreateShaderModule");
                !made) {
                return core::Result<VkPipeline>{made.Failure()};
            }
            const core::Result<VkPipeline> pipeline = BuildPipeline(key, vertex, fragment);
            // A shader module may be destroyed once the pipeline is made.
            vkDestroyShaderModule(device_, fragment, nullptr);
            return pipeline;
        }));
}

core::Status VulkanGpuBackend::Implementation::CreateImage(Image& image,
                                                           VkFormat format,
                                                           VkImageUsageFlags usage,
                                                           VkImageAspectFlags aspect,
                                                           std::uint32_t width,
                                                           std::uint32_t height) {
    const VkImageCreateInfo create{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                   .pNext = nullptr,
                                   .flags = 0,
                                   .imageType = VK_IMAGE_TYPE_2D,
                                   .format = format,
                                   .extent = {.width = width, .height = height, .depth = 1},
                                   .mipLevels = 1,
                                   .arrayLayers = 1,
                                   .samples = VK_SAMPLE_COUNT_1_BIT,
                                   .tiling = VK_IMAGE_TILING_OPTIMAL,
                                   .usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                   .queueFamilyIndexCount = 0,
                                   .pQueueFamilyIndices = nullptr,
                                   .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (const core::Status made =
            Check(vkCreateImage(device_, &create, nullptr, &image.image), "vkCreateImage");
        !made) {
        return made;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image.image, &requirements);
    const std::optional<std::uint32_t> type =
        context_->MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!type) {
        return core::Error{"no device memory for an image"};
    }
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = *type;
    if (const core::Status made =
            Check(vkAllocateMemory(device_, &allocate, nullptr, &image.memory), "vkAllocateMemory");
        !made) {
        return made;
    }
    vkBindImageMemory(device_, image.image, image.memory, 0);
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {.aspectMask = aspect,
                             .baseMipLevel = 0,
                             .levelCount = 1,
                             .baseArrayLayer = 0,
                             .layerCount = 1};
    return Check(vkCreateImageView(device_, &view, nullptr, &image.view), "vkCreateImageView");
}

void VulkanGpuBackend::Implementation::DestroyImage(Image& image) {
    vkDestroyImageView(device_, image.view, nullptr);
    vkDestroyImage(device_, image.image, nullptr);
    vkFreeMemory(device_, image.memory, nullptr);
}

core::Status VulkanGpuBackend::Implementation::CreateAttachments(Attachments& made,
                                                                 std::uint32_t width,
                                                                 std::uint32_t height) {
    if (const core::Status color = CreateImage(made.color,
                                               ColorImageFormat,
                                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                               VK_IMAGE_ASPECT_COLOR_BIT,
                                               width,
                                               height);
        !color) {
        return color;
    }
    if (const core::Status depth =
            CreateImage(made.depth,
                        DepthImageFormat,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                        width,
                        height);
        !depth) {
        return depth;
    }
    const std::array<VkImageView, 2> views = {made.color.view, made.depth.view};
    VkFramebufferCreateInfo framebuffer{};
    framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass = render_pass_;
    framebuffer.attachmentCount = 2;
    framebuffer.pAttachments = views.data();
    framebuffer.width = width;
    framebuffer.height = height;
    framebuffer.layers = 1;
    return Check(vkCreateFramebuffer(device_, &framebuffer, nullptr, &made.framebuffer),
                 "vkCreateFramebuffer");
}

void VulkanGpuBackend::Implementation::DestroyAttachments(Attachments& attachments) {
    vkDeviceWaitIdle(device_);
    vkDestroyFramebuffer(device_, attachments.framebuffer, nullptr);
    DestroyImage(attachments.color);
    DestroyImage(attachments.depth);
}

VulkanGpuBackend::Implementation::~Implementation() {
    // Memory is left true; there is nobody to tell if this fails.
    static_cast<void>(WriteBackAll());
    vkDeviceWaitIdle(device_);
    for (const std::unique_ptr<Slot>& slot : slots_) {
        DestroyAttachments(slot->images);
    }
    for (auto& [key, building] : building_) {
        if (const core::Result<VkPipeline> built = building.get(); built) {
            vkDestroyPipeline(device_, built.Value(), nullptr);
        }
    }
    for (auto& [key, pipeline] : pipelines_) {
        vkDestroyPipeline(device_, pipeline, nullptr);
    }
    for (auto& [key, module] : shaded_modules_) {
        vkDestroyShaderModule(device_, module, nullptr);
    }
    vkDestroyShaderModule(device_, vertex_module_, nullptr);
    vkDestroyShaderModule(device_, fragment_module_, nullptr);
    vkDestroyRenderPass(device_, render_pass_, nullptr);
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
}

// ---------------------------------------------------------------------------

core::Result<std::unique_ptr<VulkanGpuBackend>>
VulkanGpuBackend::Create(VulkanContext& context,
                         gpu::GpuMemory& memory,
                         gpu::Sampling sampling,
                         VertexShading shading,
                         FragmentShading fragments) {
    auto implementation =
        std::make_unique<Implementation>(context, memory, sampling, shading, fragments);
    if (const core::Status ready = implementation->Initialise(); !ready) {
        return ready.Failure();
    }
    return std::unique_ptr<VulkanGpuBackend>{new VulkanGpuBackend(std::move(implementation))};
}

VulkanGpuBackend::VulkanGpuBackend(std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}

VulkanGpuBackend::~VulkanGpuBackend() = default;

core::Status VulkanGpuBackend::RunCommandList(std::span<const std::uint8_t> list) {
    return implementation_->RunCommandList(list);
}

core::Status VulkanGpuBackend::BeforeRead(std::uint32_t physical, std::uint32_t size) {
    return implementation_->BeforeRead(physical, size);
}

core::Status VulkanGpuBackend::BeforeWrite(std::uint32_t physical, std::uint32_t size) {
    return implementation_->BeforeWrite(physical, size);
}

ports::GpuBackendStatistics VulkanGpuBackend::Statistics() const {
    return implementation_->Statistics();
}

std::string VulkanGpuBackend::Describe() const {
    return std::format("Vulkan on {}, {} draws' vertices shaded on the GPU, sampling {}",
                       implementation_->DeviceName(),
                       implementation_->GpuShadedDraws(),
                       implementation_->SamplingMode() == gpu::Sampling::Hardware
                           ? "as the registers say"
                           : "as Azahar does");
}

}  // namespace hexlith::adapters::vulkan
