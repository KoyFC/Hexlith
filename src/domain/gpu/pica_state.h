#ifndef HEXLITH_DOMAIN_GPU_PICA_STATE_H
#define HEXLITH_DOMAIN_GPU_PICA_STATE_H

#include "domain/gpu/pica_registers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace hexlith::domain::gpu {

// Four components, x, y, z and w, as host floats converted without loss from
// what a list sent. ADR 0017.
using Vec4 = std::array<float, 4>;

// Three words that carry four float24 values, or four that carry four
// float32 values, gathered one at a time from a data port. Source: GBATEK,
// GPUREG_FIXEDATTRIB_DATA, for the float24 packing, which the float uniforms
// share; GPUREG_xSH_FLOATUNIFORM_DATA for the float32 order, w first.
class PackedVec4Queue {
public:
    // The value once its last word has arrived.
    std::optional<Vec4> Push(std::uint32_t word, bool float32);

    void Reset() {
        count_ = 0;
    }

private:
    static constexpr std::size_t MaximumWords = 4;
    std::array<std::uint32_t, MaximumWords> words_{};
    std::size_t count_ = 0;
};

// What one shader unit holds that is not a register: its code and operand
// descriptor memories, and its uniforms. GBATEK, Shader registers: code
// offsets are twelve bits, so up to 4096 words; the vertex shader's code
// memory takes 512 (Azahar refuses an offset past it). Azahar keeps 4096
// descriptor slots for both units, of which an instruction can name 128.
struct ShaderUnitState {
    static constexpr std::size_t CodeWords = 4096;
    static constexpr std::size_t DescriptorWords = 4096;
    static constexpr std::size_t FloatUniforms = 96;
    static constexpr std::size_t IntUniforms = 4;

    std::array<std::uint32_t, CodeWords> code{};
    std::array<std::uint32_t, DescriptorWords> descriptors{};
    std::array<Vec4, FloatUniforms> float_uniforms{};
    std::uint16_t bool_uniforms = 0;
    // Each as its register holds it: x in bits 0 to 7, y in 8 to 15, z in 16
    // to 23.
    std::array<std::uint32_t, IntUniforms> int_uniforms{};
    PackedVec4Queue uniform_queue;
};

// What a write asks for beyond storing its value.
enum class WriteEffect : std::uint8_t {
    None,
    // GPUREG_ATTR_BUF_DRAWARRAYS and DRAWELEMENTS.
    DrawArrays,
    DrawElements,
    // A vertex has been given whole in immediate mode, in ImmediateVertex.
    ImmediateVertex,
    // GPUREG_PRIMITIVE_CONFIG was written, or GPUREG_RESTART_PRIMITIVE: the
    // primitive assembler starts again.
    PrimitiveRestart,
    // GPUREG_CMDBUF_JUMP0 and JUMP1.
    Jump0,
    Jump1,
    // GPUREG_IRQ_REQ(0), the FINALIZE that ends a list.
    Finalize,
};

// The PICA200's state as command lists build it. ADR 0017 has why it is the
// raw register file with the memories beside it, and docs/pica-commands.md
// what each side effect follows.
class PicaState {
public:
    static constexpr std::size_t FixedAttributes = 16;
    static constexpr std::size_t VertexInputs = 16;
    static constexpr std::size_t LightingLuts = 24;
    static constexpr std::size_t LightingLutEntries = 256;
    static constexpr std::size_t FogLutEntries = 128;
    static constexpr std::size_t ProcTexSmallLut = 128;
    static constexpr std::size_t ProcTexLargeLut = 256;
    // Tables 0 to 5, of which 1 is none.
    static constexpr std::size_t ProcTexTables = 6;

    // Stores a value under a byte mask, and carries out what the register
    // does when written. The value a shader, LUT or attribute port takes is
    // the value as written, before the mask, as Azahar takes it: what the
    // hardware does with a partial write to a port is not known.
    WriteEffect Write(std::uint16_t id, std::uint32_t value, std::uint32_t byte_mask);

    [[nodiscard]] const RegisterFile& Registers() const {
        return registers_;
    }

    [[nodiscard]] std::uint32_t Register(std::uint16_t id) const {
        return registers_.at(id);
    }

    [[nodiscard]] const ShaderUnitState& VertexShader() const {
        return vertex_shader_;
    }

    [[nodiscard]] const ShaderUnitState& GeometryShader() const {
        return geometry_shader_;
    }

    // The geometry shader sets its bool uniform b15 every time it has run,
    // which a list's next write of the bool uniforms replaces. Source:
    // GBATEK, GPUREG_xSH_BOOLUNIFORM, "Bit15 seems to be automatically set
    // after executing shader code"; Azahar's GeometryPipeline::SubmitVertex.
    void MarkGeometryShaderRun() {
        constexpr std::uint16_t RunFlag = 1U << 15U;
        geometry_shader_.bool_uniforms |= RunFlag;
    }

    // The values the shader takes for inputs no attribute array supplies.
    [[nodiscard]] const std::array<Vec4, FixedAttributes>& FixedAttributeValues() const {
        return fixed_attributes_;
    }

    // The inputs of the last vertex given in immediate mode.
    [[nodiscard]] const std::array<Vec4, VertexInputs>& ImmediateVertex() const {
        return immediate_vertex_;
    }

    [[nodiscard]] const std::array<std::array<std::uint32_t, LightingLutEntries>, LightingLuts>&
    LightingLutData() const {
        return lighting_luts_;
    }

    [[nodiscard]] const std::array<std::uint32_t, FogLutEntries>& FogLut() const {
        return fog_lut_;
    }

    // The procedural texture's tables by GBATEK's numbers: noise 0, colour
    // map 2, alpha map 3, colour 4, colour difference 5. 1 is not a table.
    [[nodiscard]] const std::array<std::array<std::uint32_t, ProcTexLargeLut>, ProcTexTables>&
    ProcTexLuts() const {
        return proctex_luts_;
    }

private:
    WriteEffect WriteShaderRegister(ShaderUnitState& unit,
                                    std::uint16_t base,
                                    std::uint16_t offset,
                                    std::uint32_t value);
    void UploadCode(ShaderUnitState& unit, std::uint16_t base, std::uint32_t value);
    void UploadDescriptor(ShaderUnitState& unit, std::uint16_t base, std::uint32_t value);
    void UploadFloatUniform(ShaderUnitState& unit, std::uint16_t base, std::uint32_t value);
    WriteEffect TakeFixedAttributeWord(std::uint32_t value);
    void WriteLightingLut(std::uint32_t value);
    void WriteFogLut(std::uint32_t value);
    void WriteProcTexLut(std::uint32_t value);
    // Whether a write to the vertex shader's block also reaches the geometry
    // shader's: when GPUREG_VSH_COM_MODE does not keep them apart and no
    // geometry shader is in use. Source: Azahar's HandleSpecialReg.
    [[nodiscard]] bool VertexShaderWritesReachGeometryShader() const;

    RegisterFile registers_{};
    ShaderUnitState vertex_shader_;
    ShaderUnitState geometry_shader_;
    std::array<Vec4, FixedAttributes> fixed_attributes_{};
    std::array<Vec4, VertexInputs> immediate_vertex_{};
    std::size_t immediate_attribute_ = 0;
    PackedVec4Queue fixed_attribute_queue_;
    std::array<std::array<std::uint32_t, LightingLutEntries>, LightingLuts> lighting_luts_{};
    std::array<std::uint32_t, FogLutEntries> fog_lut_{};
    std::array<std::array<std::uint32_t, ProcTexLargeLut>, ProcTexTables> proctex_luts_{};
};

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_PICA_STATE_H
