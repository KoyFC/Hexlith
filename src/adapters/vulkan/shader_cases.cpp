#include "adapters/vulkan/shader_cases.h"

#include "adapters/vulkan/glsl_compiler.h"
#include "domain/gpu/shader_glsl.h"

#include <bit>
#include <cstring>
#include <format>

namespace hexlith::adapters::vulkan {
namespace {

namespace gpu = domain::gpu;

// A case as the compute shader reads and writes it, std430: the uniforms, the
// three banks of registers, the integer uniforms, the address registers, and
// the bool uniforms, the condition flags and the outcome.
struct CaseLayout {
    std::array<std::array<float, 4>, gpu::ShaderUnitState::FloatUniforms> uniforms;
    std::array<std::array<float, 4>, gpu::ShaderRegisters::Count> inputs;
    std::array<std::array<float, 4>, gpu::ShaderRegisters::Count> temporaries;
    std::array<std::array<float, 4>, gpu::ShaderRegisters::Count> outputs;
    std::array<std::uint32_t, 4> ints;
    std::array<std::int32_t, 4> address;
    // Bool uniforms, flag x, flag y, outcome: bit 0 ended, bit 1 unsupported.
    std::array<std::uint32_t, 4> state;
};

constexpr std::string_view Header = R"(layout(local_size_x = 64) in;

struct PicaCase {
    vec4 uniforms[96];
    vec4 inputs[16];
    vec4 temporaries[16];
    vec4 outputs[16];
    uvec4 ints;
    ivec4 address;
    uvec4 state;
};

layout(std430, binding = 0) buffer Cases {
    PicaCase cases[];
};

layout(push_constant) uniform Run {
    uint count;
    uint limit;
};

uint pica_case;

vec4 pica_float(uint index) {
    return cases[pica_case].uniforms[index];
}

bool pica_bool(uint index) {
    return ((cases[pica_case].state.x >> index) & 1u) != 0u;
}

uint pica_int(uint index) {
    return cases[pica_case].ints[index];
}

)";

constexpr std::string_view Main = R"(
void main() {
    pica_case = gl_GlobalInvocationID.x;
    if (pica_case >= count) {
        return;
    }
    pica_v = cases[pica_case].inputs;
    pica_r = cases[pica_case].temporaries;
    pica_o = cases[pica_case].outputs;
    pica_a = cases[pica_case].address.xyz;
    pica_cmp = bvec2(cases[pica_case].state.y != 0u, cases[pica_case].state.z != 0u);
    bool ended = pica_run(limit);
    cases[pica_case].temporaries = pica_r;
    cases[pica_case].outputs = pica_o;
    cases[pica_case].address.xyz = pica_a;
    cases[pica_case].state.y = pica_cmp.x ? 1u : 0u;
    cases[pica_case].state.z = pica_cmp.y ? 1u : 0u;
    cases[pica_case].state.w = (ended ? 1u : 0u) | (pica_unsupported ? 2u : 0u);
}
)";

template<std::size_t N>
void Store(std::array<std::array<float, 4>, N>& target, const std::array<gpu::Vec4, N>& source) {
    for (std::size_t at = 0; at < N; ++at) {
        target.at(at) = source.at(at);
    }
}

template<std::size_t N>
void Load(std::array<gpu::Vec4, N>& target, const std::array<std::array<float, 4>, N>& source) {
    for (std::size_t at = 0; at < N; ++at) {
        target.at(at) = source.at(at);
    }
}

CaseLayout Lay(const ShaderCaseState& state) {
    CaseLayout layout{};
    Store(layout.uniforms, state.float_uniforms);
    Store(layout.inputs, state.registers.inputs);
    Store(layout.temporaries, state.registers.temporaries);
    Store(layout.outputs, state.registers.outputs.at(state.registers.output_bank));
    layout.ints = state.int_uniforms;
    layout.address = {
        state.registers.address[0], state.registers.address[1], state.registers.address[2], 0};
    layout.state = {state.bool_uniforms,
                    state.registers.conditions[0] ? 1U : 0U,
                    state.registers.conditions[1] ? 1U : 0U,
                    0};
    return layout;
}

void Unlay(const CaseLayout& layout, ShaderCaseState& state) {
    Load(state.registers.temporaries, layout.temporaries);
    Load(state.registers.outputs.at(state.registers.output_bank), layout.outputs);
    for (std::size_t at = 0; at < 3; ++at) {
        state.registers.address.at(at) = layout.address.at(at);
    }
    state.registers.conditions = {layout.state[1] != 0, layout.state[2] != 0};
    state.ended = (layout.state[3] & 1U) != 0;
    state.unsupported = (layout.state[3] & 2U) != 0;
}

// The objects of one compute pipeline over one storage buffer, destroyed
// together.
class ComputePipeline {
public:
    explicit ComputePipeline(VulkanContext& context) : device_(context.Device()) {}

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;
    ComputePipeline(ComputePipeline&&) = delete;
    ComputePipeline& operator=(ComputePipeline&&) = delete;

    ~ComputePipeline() {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        vkDestroyDescriptorPool(device_, pool_, nullptr);
        vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        vkDestroyShaderModule(device_, module_, nullptr);
    }

    core::Status Build(std::span<const std::uint32_t> spirv, VkBuffer buffer);
    void Record(VkCommandBuffer commands, std::uint32_t count, std::uint32_t limit) const;

private:
    VkDevice device_;
    VkShaderModule module_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

core::Status ComputePipeline::Build(std::span<const std::uint32_t> spirv, VkBuffer buffer) {
    VkShaderModuleCreateInfo module{};
    module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module.codeSize = spirv.size_bytes();
    module.pCode = spirv.data();
    if (core::Status made = Check(vkCreateShaderModule(device_, &module, nullptr, &module_),
                                  "vkCreateShaderModule");
        !made) {
        return made;
    }
    VkDescriptorSetLayoutBinding binding{};
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo set_layout{};
    set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout.bindingCount = 1;
    set_layout.pBindings = &binding;
    if (core::Status made =
            Check(vkCreateDescriptorSetLayout(device_, &set_layout, nullptr, &set_layout_),
                  "vkCreateDescriptorSetLayout");
        !made) {
        return made;
    }
    const VkDescriptorPoolSize size{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                    .descriptorCount = 1};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (core::Status made = Check(vkCreateDescriptorPool(device_, &pool, nullptr, &pool_),
                                  "vkCreateDescriptorPool");
        !made) {
        return made;
    }
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set_layout_;
    if (core::Status made =
            Check(vkAllocateDescriptorSets(device_, &allocate, &set_), "vkAllocateDescriptorSets");
        !made) {
        return made;
    }
    const VkDescriptorBufferInfo buffer_info{.buffer = buffer, .offset = 0, .range = VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    const VkPushConstantRange push{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 2 * sizeof(std::uint32_t)};
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &set_layout_;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (core::Status made = Check(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_),
                                  "vkCreatePipelineLayout");
        !made) {
        return made;
    }
    const VkComputePipelineCreateInfo pipeline{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .pNext = nullptr,
                  .flags = 0,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module_,
                  .pName = "main",
                  .pSpecializationInfo = nullptr},
        .layout = layout_,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0};
    return Check(
        vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_),
        "vkCreateComputePipelines");
}

void ComputePipeline::Record(VkCommandBuffer commands,
                             std::uint32_t count,
                             std::uint32_t limit) const {
    constexpr std::uint32_t GroupSize = 64;
    const std::array<std::uint32_t, 2> run = {count, limit};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(
        commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);
    vkCmdPushConstants(commands, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(run), run.data());
    vkCmdDispatch(commands, (count + GroupSize - 1) / GroupSize, 1, 1);
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commands,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0,
                         1,
                         &barrier,
                         0,
                         nullptr,
                         0,
                         nullptr);
}

}  // namespace

core::Status RunVertexShaderCases(VulkanContext& context,
                                  const gpu::ShaderUnitState& program,
                                  std::uint32_t entry,
                                  std::span<ShaderCaseState> cases,
                                  std::uint32_t limit) {
    if (cases.empty()) {
        return {};
    }
    const gpu::GlslShader translated = gpu::TranslateShader(program, entry);
    const std::string source = "#version 450\n" + FloatControlLines(context.Floats()) +
                               std::string{Header} + translated.source + std::string{Main};
    const core::Result<std::vector<std::uint32_t>> spirv =
        CompileGlsl(source, ShaderStage::Compute);
    if (!spirv) {
        return spirv.Failure();
    }
    const VkDeviceSize size = sizeof(CaseLayout) * cases.size();
    const core::Result<std::unique_ptr<HostBuffer>> buffer =
        HostBuffer::Create(context, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!buffer) {
        return buffer.Failure();
    }
    const std::span<std::uint8_t> bytes = buffer.Value()->Bytes();
    for (std::size_t at = 0; at < cases.size(); ++at) {
        const CaseLayout layout = Lay(cases[at]);
        std::memcpy(bytes.subspan(at * sizeof(CaseLayout)).data(), &layout, sizeof(layout));
    }
    ComputePipeline pipeline(context);
    if (core::Status built = pipeline.Build(spirv.Value(), buffer.Value()->Buffer()); !built) {
        return built;
    }
    const auto count = static_cast<std::uint32_t>(cases.size());
    if (core::Status ran = context.Submit(
            [&](VkCommandBuffer commands) { pipeline.Record(commands, count, limit); });
        !ran) {
        return ran;
    }
    for (std::size_t at = 0; at < cases.size(); ++at) {
        CaseLayout layout{};
        std::memcpy(&layout, bytes.subspan(at * sizeof(CaseLayout)).data(), sizeof(layout));
        Unlay(layout, cases[at]);
    }
    return {};
}

}  // namespace hexlith::adapters::vulkan
