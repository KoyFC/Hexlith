#include "domain/gpu/reference_renderer.h"

#include <algorithm>
#include <optional>

namespace hexlith::domain::gpu {
namespace {

constexpr std::uint32_t Field(std::uint32_t word, unsigned low, unsigned width) {
    return (word >> low) & ((1U << width) - 1);
}

// Geometry pipeline and shader registers. Source: GBATEK, Geometry Pipeline
// registers and Shader registers.
constexpr std::uint16_t VertexOutputTotal = 0x24A;
constexpr std::uint16_t GeometryShaderConfig = 0x252;
constexpr std::uint32_t GeometryShaderInUse = 2;
constexpr std::uint32_t PointMode = 0;
constexpr std::uint32_t AddressMask = 0x0FFFFFFF;
constexpr unsigned AddressShift = 3;

std::uint32_t UnitRegister(const PicaState& state, std::uint16_t base, std::uint16_t offset) {
    return state.Register(static_cast<std::uint16_t>(base + offset));
}

// GPUREG_INDEXBUFFER_CONFIG: the offset from the attribute base in bits 0 to
// 27, and in bit 31 whether the indices are halfwords rather than bytes.
std::uint32_t ReadIndex(const GpuMemory& memory, const PicaState& state, std::uint32_t at) {
    constexpr unsigned HalfwordBit = 31;
    const std::uint32_t config = state.Register(reg::IndexList);
    const std::uint32_t base = (state.Register(reg::AttributeBufferBase) & AddressMask)
                               << AddressShift;
    const bool halfwords = Field(config, HalfwordBit, 1) != 0;
    const std::uint32_t size = halfwords ? 2 : 1;
    const std::span<const std::uint8_t> bytes =
        memory.Bytes(base + (config & AddressMask) + (at * size), size);
    if (bytes.empty()) {
        return 0;
    }
    return halfwords ? static_cast<std::uint32_t>(bytes[0] | (bytes[1] << 8U)) : bytes[0];
}

// A small cache of shaded vertices for indexed draws, the most recent 64
// replacing the oldest. Whether the hardware has one, and of what size, is
// not documented; Azahar keeps this one, and a vertex shader with side
// effects across vertices would tell the difference.
class VertexCache {
public:
    static constexpr std::size_t Size = 64;

    [[nodiscard]] std::optional<ShaderRegisters::Bank> Find(std::uint32_t vertex) const {
        for (std::size_t slot = 0; slot < Size; ++slot) {
            if (valid_.at(slot) && ids_.at(slot) == vertex) {
                return outputs_.at(slot);
            }
        }
        return std::nullopt;
    }

    void Add(std::uint32_t vertex, const ShaderRegisters::Bank& outputs) {
        ids_.at(next_) = vertex;
        outputs_.at(next_) = outputs;
        valid_.at(next_) = true;
        next_ = (next_ + 1) % Size;
    }

private:
    std::array<std::uint32_t, Size> ids_{};
    std::array<ShaderRegisters::Bank, Size> outputs_{};
    std::array<bool, Size> valid_{};
    std::size_t next_ = 0;
};

}  // namespace

void ReferenceRenderer::Draw(PicaState& state, bool indexed) {
    ++statistics_.draws;
    ConfigureGeometryStage(state);
    const VertexLoader loader(state.Registers());
    const std::uint32_t count = state.Register(reg::VertexCount);
    const std::uint32_t first = state.Register(reg::FirstIndex);
    // One unit's registers for the whole draw: what a run leaves in the
    // temporaries is there for the next vertex (Azahar's LoadVertices).
    ShaderRegisters registers;
    VertexCache cache;
    for (std::uint32_t at = 0; at < count; ++at) {
        // An indexed draw does not add the first index.
        const std::uint32_t vertex = indexed ? ReadIndex(*memory_, state, at) : at + first;
        std::optional<ShaderRegisters::Bank> outputs = indexed ? cache.Find(vertex) : std::nullopt;
        if (!outputs.has_value()) {
            outputs = ShadeVertex(
                state, loader.Load(*memory_, vertex, state.FixedAttributeValues()), registers);
            if (indexed) {
                cache.Add(vertex, *outputs);
            }
        }
        SubmitVertex(state, *outputs);
    }
}

// A vertex given in immediate mode runs on registers of its own (Azahar's
// DrawImmediate), and goes on as a drawn one does.
void ReferenceRenderer::ImmediateVertex(PicaState& state) {
    const bool geometry_shader =
        Field(state.Register(reg::GeometryStageConfig), 0, 2) == GeometryShaderInUse;
    if (geometry_shader != geometry_shader_) {
        ConfigureGeometryStage(state);
    }
    ShaderRegisters registers;
    SubmitVertex(state, ShadeVertex(state, state.ImmediateVertex(), registers));
}

// GPUREG_PRIMITIVE_CONFIG bits 8 and 9 give the topology. Source: GBATEK.
void ReferenceRenderer::RestartPrimitive(PicaState& state) {
    constexpr unsigned TopologyShift = 8;
    assembler_.Restart(static_cast<PrimitiveAssembler::Topology>(
        Field(state.Register(reg::PrimitiveConfig), TopologyShift, 2)));
}

// GPUREG_GEOSTAGE_CONFIG bits 0 and 1 are 2 when a geometry shader runs, and
// GPUREG_GSH_MISC0's low byte is its mode. Only the mode that takes a fixed
// number of the vertex shader's outputs at a time is implemented: GBATEK
// calls it point mode, and it is the one the reference title uses.
void ReferenceRenderer::ConfigureGeometryStage(const PicaState& state) {
    geometry_shader_ = Field(state.Register(reg::GeometryStageConfig), 0, 2) == GeometryShaderInUse;
    geometry_filled_ = 0;
    if (!geometry_shader_) {
        return;
    }
    if (Field(state.Register(GeometryShaderConfig), 0, 8) != PointMode) {
        statistics_.unsupported_shader = true;
    }
    vertex_outputs_ = Field(state.Register(VertexOutputTotal), 0, 4) + 1;
    geometry_needed_ =
        Field(UnitRegister(state, reg::GeometryShaderBase, reg::shader::InputBufferConfig), 0, 4) +
        1;
    emitter_.SetOutputMask(UnitRegister(state, reg::GeometryShaderBase, reg::shader::OutputMask));
}

ShaderRegisters::Bank ReferenceRenderer::ShadeVertex(const PicaState& state,
                                                     const VertexLoader::Inputs& attributes,
                                                     ShaderRegisters& registers) {
    ++statistics_.vertex_shader_runs;
    PlaceInputs(state.Registers(), reg::VertexShaderBase, attributes, registers);
    const ShaderRun run{
        .entry = Field(UnitRegister(state, reg::VertexShaderBase, reg::shader::EntryPoint), 0, 16),
        .precision = precision_};
    if (RunShader(state.VertexShader(), registers, run).unsupported) {
        statistics_.unsupported_shader = true;
    }
    return PackOutputs(UnitRegister(state, reg::VertexShaderBase, reg::shader::OutputMask),
                       registers.outputs.at(registers.output_bank));
}

void ReferenceRenderer::SubmitVertex(PicaState& state, const ShaderRegisters::Bank& packed) {
    if (!geometry_shader_) {
        Assemble(state, packed);
        return;
    }
    // The vertex shader's outputs are gathered until the geometry shader has
    // as many inputs as it takes.
    for (std::size_t output = 0;
         output < vertex_outputs_ && geometry_filled_ < geometry_inputs_.size();
         ++output) {
        geometry_inputs_.at(geometry_filled_++) = packed.at(output);
    }
    if (geometry_filled_ >= geometry_needed_) {
        geometry_filled_ = 0;
        RunGeometryShader(state);
    }
}

void ReferenceRenderer::RunGeometryShader(PicaState& state) {
    ++statistics_.geometry_shader_runs;
    PlaceInputs(state.Registers(), reg::GeometryShaderBase, geometry_inputs_, geometry_registers_);
    const ShaderRun run{
        .entry =
            Field(UnitRegister(state, reg::GeometryShaderBase, reg::shader::EntryPoint), 0, 16),
        .emitter = &emitter_,
        .precision = precision_};
    if (RunShader(state.GeometryShader(), geometry_registers_, run).unsupported) {
        statistics_.unsupported_shader = true;
    }
    state.MarkGeometryShaderRun();
    for (const GeometryEmitter::Primitive& primitive : emitter_.TakePrimitives()) {
        if (primitive.reversed) {
            assembler_.ReverseNextWinding();
        }
        for (const ShaderRegisters::Bank& vertex : primitive.vertices) {
            Assemble(state, vertex);
        }
    }
}

void ReferenceRenderer::Assemble(const PicaState& state, const ShaderRegisters::Bank& packed) {
    const std::optional<PrimitiveAssembler::Triangle> triangle =
        assembler_.Submit(MakeOutputVertex(state.Registers(), packed));
    if (triangle.has_value()) {
        rasterizer_.DrawTriangle(state, *triangle);
    }
}

}  // namespace hexlith::domain::gpu
