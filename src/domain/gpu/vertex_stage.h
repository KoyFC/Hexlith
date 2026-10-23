#ifndef HEXLITH_DOMAIN_GPU_VERTEX_STAGE_H
#define HEXLITH_DOMAIN_GPU_VERTEX_STAGE_H

#include "domain/gpu/gpu_memory.h"
#include "domain/gpu/pica_state.h"
#include "domain/gpu/shader_interpreter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace hexlith::domain::gpu {

// The attribute arrays a draw reads its vertices from. Source: GBATEK,
// Geometry Pipeline registers, Vertex Attribute Arrays; Azahar's VertexLoader
// for the alignment of an attribute inside an array and the components an
// attribute of fewer than four leaves.
class VertexLoader {
public:
    static constexpr std::size_t Attributes = 16;
    using Inputs = std::array<Vec4, Attributes>;

    explicit VertexLoader(const RegisterFile& registers);

    // Every attribute of a vertex: from its array, at the array's offset plus
    // its stride times the vertex's index, or the fixed value when the
    // attribute is fixed. Bytes outside GPU memory read as zero.
    [[nodiscard]] Inputs Load(const GpuMemory& memory,
                              std::uint32_t vertex,
                              const std::array<Vec4, PicaState::FixedAttributes>& fixed) const;

private:
    struct Source {
        std::uint32_t address = 0;
        std::uint32_t stride = 0;
        // 0 signed byte, 1 unsigned byte, 2 signed halfword, 3 float32.
        std::uint32_t type = 0;
        std::uint32_t components = 0;
    };

    std::array<std::optional<Source>, Attributes> sources_{};
    std::array<bool, Attributes> fixed_{};
    std::size_t count_ = 0;
};

// The vertex an indexed draw names at a position of its index buffer:
// GPUREG_INDEXBUFFER_CONFIG's offset from the attribute base in bits 0 to 27,
// halfwords rather than bytes when bit 31 is set. An index outside GPU memory
// reads as zero. Source: GBATEK, Geometry Pipeline registers.
std::uint32_t ReadIndex(const GpuMemory& memory, const RegisterFile& registers, std::uint32_t at);

// The shader's inputs as GPUREG_xSH_ATTR_PERMUTATION places a vertex's
// attributes in its input registers, the first `count` of them. Registers
// no attribute names keep what they held. Source: GBATEK, Shader registers.
void PlaceInputs(const RegisterFile& registers,
                 std::uint16_t unit_base,
                 const VertexLoader::Inputs& attributes,
                 ShaderRegisters& shader);

// A unit's enabled outputs, packed in register order by its output mask.
// Source: GBATEK, GPUREG_xSH_OUTMAP_MASK; Azahar's ShaderUnit::WriteOutput.
ShaderRegisters::Bank PackOutputs(std::uint32_t output_mask, const ShaderRegisters::Bank& outputs);

// A vertex as the rasterizer takes it: what GPUREG_SH_OUTMAP_Oi gives each
// semantic. Source: GBATEK, Shader Output.
struct OutputVertex {
    // Semantic numbers, the slots of `values`.
    static constexpr std::size_t Position = 0x00;
    static constexpr std::size_t Quaternion = 0x04;
    static constexpr std::size_t Color = 0x08;
    static constexpr std::size_t Texcoord0 = 0x0C;
    static constexpr std::size_t Texcoord1 = 0x0E;
    static constexpr std::size_t Texcoord0W = 0x10;
    static constexpr std::size_t View = 0x12;
    static constexpr std::size_t Texcoord2 = 0x16;
    static constexpr std::size_t Slots = 0x18;

    std::array<float, Slots> values{};
};

// Builds a vertex from the packed outputs of the stage before. A semantic
// no output gives is 1.0, and the colour is made positive and kept to 1.0
// before it is interpolated. Source: OutputVertex in Azahar's
// output_vertex.cpp, which says both were measured on hardware.
OutputVertex MakeOutputVertex(const RegisterFile& registers, const ShaderRegisters::Bank& packed);

// Turns vertices into triangles by GPUREG_PRIMITIVE_CONFIG's topology:
// a list, a strip, a fan, or the geometry shader's primitives, whose winding
// SETEMIT can reverse. Source: GBATEK, GPUREG_PRIMITIVE_CONFIG; Azahar's
// PrimitiveAssembler for how a strip alternates and a reversed winding
// swaps the first two vertices.
class PrimitiveAssembler {
public:
    enum class Topology : std::uint8_t { List = 0, Strip = 1, Fan = 2, Shader = 3 };
    using Triangle = std::array<OutputVertex, 3>;

    // Starts again, with a topology.
    void Restart(Topology topology);

    // The next triangle reverses its winding, if it comes from a geometry
    // shader.
    void ReverseNextWinding() {
        reversed_ = true;
    }

    std::optional<Triangle> Submit(const OutputVertex& vertex);

    // Whether no vertex waits for a triangle, so that what comes next starts
    // a fresh one.
    [[nodiscard]] bool Idle() const {
        return index_ == 0 && !ready_;
    }

    [[nodiscard]] Topology CurrentTopology() const {
        return topology_;
    }

private:
    Topology topology_ = Topology::List;
    std::array<OutputVertex, 2> buffer_{};
    std::size_t index_ = 0;
    bool ready_ = false;
    bool reversed_ = false;
};

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_VERTEX_STAGE_H
