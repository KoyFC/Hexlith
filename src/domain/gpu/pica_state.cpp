#include "domain/gpu/pica_state.h"

#include "domain/gpu/float24.h"

#include <bit>

namespace hexlith::domain::gpu {
namespace {

// The vertex shader's code memory. Source: Azahar refuses a VS code offset
// of 512 or more (HandleSpecialReg).
constexpr std::uint32_t VertexShaderCodeWords = 512;

// GPUREG_xSH_FLOATUNIFORM_INDEX: the index in bits 0 to 6 and float32 mode
// in bit 31. GBATEK gives the index eight bits; Azahar, seven. They agree for
// every index a uniform can have.
constexpr std::uint32_t UniformIndexMask = 0x7F;
constexpr unsigned Float32ModeBit = 31;

// GPUREG_FIXEDATTRIB_INDEX: the attribute, 0Fh for a vertex given in
// immediate mode. GBATEK gives it bits 0 to 3; Azahar reads the whole word
// and drops an attribute whose index is past 0Fh, which only a malformed list
// would show.
constexpr std::uint32_t ImmediateModeIndex = 0xF;

// GPUREG_VSH_NUM_ATTR: the number of inputs less one, bits 0 to 3.
constexpr std::uint32_t AttributeCountMask = 0xF;

// GPUREG_LIGHTING_LUT_INDEX: the entry in bits 0 to 7 and the table in 8 to
// 12. Source: lut_config in Azahar's regs_lighting.h.
constexpr std::uint32_t LutEntryMask = 0xFF;
constexpr unsigned LightingTableShift = 8;
constexpr std::uint32_t LightingTableMask = 0x1F;

// GPUREG_FOG_LUT_INDEX: the entry in bits 0 to 15. Source: fog_lut_offset in
// Azahar's regs_texturing.h.
constexpr std::uint32_t FogIndexMask = 0xFFFF;

// GPUREG_PROCTEX_LUT: the entry in bits 0 to 7, the table in 8 to 11.
// Source: GBATEK, Texturing registers (Procedural Texture).
constexpr unsigned ProcTexTableShift = 8;
constexpr std::uint32_t ProcTexTableMask = 0xF;

// GPUREG_GEOSTAGE_CONFIG bits 0 and 1 say whether a geometry shader is in
// use; GPUREG_VSH_COM_MODE bit 0 keeps the two units' configurations apart.
constexpr std::uint32_t GeometryShaderInUseMask = 0x3;
constexpr std::uint32_t SeparateConfigurationBit = 0x1;

// GPUREG_xSH_OUTMAP_MASK, bits 0 to 15.
constexpr std::uint32_t OutputMaskBits = 0xFFFF;

bool Within(std::uint16_t id, std::uint16_t first, std::uint16_t last) {
    return id >= first && id <= last;
}

}  // namespace

std::optional<Vec4> PackedVec4Queue::Push(std::uint32_t word, bool float32) {
    words_.at(count_) = word;
    ++count_;
    if (count_ < (float32 ? MaximumWords : MaximumWords - 1)) {
        return std::nullopt;
    }
    count_ = 0;
    if (float32) {
        // w first, x last.
        return Vec4{std::bit_cast<float>(words_[3]),
                    std::bit_cast<float>(words_[2]),
                    std::bit_cast<float>(words_[1]),
                    std::bit_cast<float>(words_[0])};
    }
    // 96 bits, x in the lowest 24, sent highest word first.
    constexpr std::uint32_t Low24 = 0xFFFFFF;
    const std::uint32_t x = words_[2] & Low24;
    const std::uint32_t y = ((words_[1] & 0xFFFF) << 8) | (words_[2] >> 24);
    const std::uint32_t z = ((words_[0] & 0xFF) << 16) | (words_[1] >> 16);
    const std::uint32_t w = words_[0] >> 8;
    return Vec4{Float24ToFloat32(x), Float24ToFloat32(y), Float24ToFloat32(z), Float24ToFloat32(w)};
}

WriteEffect PicaState::Write(std::uint16_t id, std::uint32_t value, std::uint32_t byte_mask) {
    if (id >= RegisterCount) {
        return WriteEffect::None;
    }
    const std::uint32_t mask = ExpandByteMask(byte_mask);
    registers_.at(id) = (registers_.at(id) & ~mask) | (value & mask);

    switch (id) {
    case reg::IrqRequestFirst:
        return WriteEffect::Finalize;
    case reg::DrawArrays:
        return WriteEffect::DrawArrays;
    case reg::DrawElements:
        return WriteEffect::DrawElements;
    case reg::CommandBufferJump0:
        return WriteEffect::Jump0;
    case reg::CommandBufferJump1:
        return WriteEffect::Jump1;
    case reg::PrimitiveConfig:
    case reg::RestartPrimitive:
        return WriteEffect::PrimitiveRestart;
    case reg::FixedAttributeIndex:
        fixed_attribute_queue_.Reset();
        immediate_attribute_ = 0;
        return WriteEffect::None;
    default:
        break;
    }

    if (Within(id, reg::FixedAttributeDataFirst, reg::FixedAttributeDataLast)) {
        return TakeFixedAttributeWord(value);
    }
    if (Within(id, reg::LightingLutDataFirst, reg::LightingLutDataLast)) {
        WriteLightingLut(value);
    } else if (Within(id, reg::FogLutDataFirst, reg::FogLutDataLast)) {
        WriteFogLut(value);
    } else if (Within(id, reg::ProcTexLutDataFirst, reg::ProcTexLutDataLast)) {
        WriteProcTexLut(value);
    } else if (Within(
                   id, reg::VertexShaderBase, reg::VertexShaderBase + reg::shader::BlockSize - 1)) {
        return WriteShaderRegister(
            vertex_shader_, reg::VertexShaderBase, id - reg::VertexShaderBase, value);
    } else if (Within(id,
                      reg::GeometryShaderBase,
                      reg::GeometryShaderBase + reg::shader::BlockSize - 1)) {
        return WriteShaderRegister(
            geometry_shader_, reg::GeometryShaderBase, id - reg::GeometryShaderBase, value);
    }
    return WriteEffect::None;
}

bool PicaState::VertexShaderWritesReachGeometryShader() const {
    return (registers_.at(reg::VertexShaderCommonMode) & SeparateConfigurationBit) == 0 &&
           (registers_.at(reg::GeometryStageConfig) & GeometryShaderInUseMask) == 0;
}

WriteEffect PicaState::WriteShaderRegister(ShaderUnitState& unit,
                                           std::uint16_t base,
                                           std::uint16_t offset,
                                           std::uint32_t value) {
    namespace shader = reg::shader;
    const bool mirrored = base == reg::VertexShaderBase && VertexShaderWritesReachGeometryShader();
    const std::uint32_t stored = registers_.at(base + offset);

    if (offset == shader::BoolUniform) {
        unit.bool_uniforms = static_cast<std::uint16_t>(stored);
        if (mirrored) {
            geometry_shader_.bool_uniforms = unit.bool_uniforms;
        }
    } else if (Within(offset, shader::IntUniformFirst, shader::IntUniformLast)) {
        unit.int_uniforms.at(offset - shader::IntUniformFirst) = stored;
        if (mirrored) {
            geometry_shader_.int_uniforms.at(offset - shader::IntUniformFirst) = stored;
        }
    } else if (offset == shader::OutputMask && mirrored) {
        std::uint32_t& target = registers_.at(reg::GeometryShaderBase + shader::OutputMask);
        target = (target & ~OutputMaskBits) | (value & OutputMaskBits);
    } else if (Within(offset, shader::FloatUniformDataFirst, shader::FloatUniformDataLast)) {
        UploadFloatUniform(unit, base, value);
    } else if (Within(offset, shader::CodeTransferDataFirst, shader::CodeTransferDataLast)) {
        UploadCode(unit, base, value);
    } else if (Within(offset, shader::DescriptorDataFirst, shader::DescriptorDataLast)) {
        UploadDescriptor(unit, base, value);
    }
    return WriteEffect::None;
}

// GPUREG_xSH_CODETRANSFER_INDEX steps on with every word written, and a word
// past the end of the unit's code memory is dropped without stepping.
// Source: GBATEK, Shader registers; Azahar for the end.
void PicaState::UploadCode(ShaderUnitState& unit, std::uint16_t base, std::uint32_t value) {
    std::uint32_t& index = registers_.at(base + reg::shader::CodeTransferIndex);
    const std::uint32_t limit =
        base == reg::VertexShaderBase ? VertexShaderCodeWords : ShaderUnitState::CodeWords;
    if (index >= limit) {
        return;
    }
    unit.code.at(index) = value;
    if (base == reg::VertexShaderBase && VertexShaderWritesReachGeometryShader()) {
        geometry_shader_.code.at(index) = value;
    }
    ++index;
}

void PicaState::UploadDescriptor(ShaderUnitState& unit, std::uint16_t base, std::uint32_t value) {
    std::uint32_t& index = registers_.at(base + reg::shader::DescriptorIndex);
    if (index >= ShaderUnitState::DescriptorWords) {
        return;
    }
    unit.descriptors.at(index) = value;
    if (base == reg::VertexShaderBase && VertexShaderWritesReachGeometryShader()) {
        geometry_shader_.descriptors.at(index) = value;
    }
    ++index;
}

// A uniform is taken once its last word arrives, at the index
// GPUREG_xSH_FLOATUNIFORM_INDEX holds, which then steps on. An index past the
// 96 uniforms drops the value without stepping, which Azahar says is what the
// hardware does with the 7Fh some games send.
void PicaState::UploadFloatUniform(ShaderUnitState& unit, std::uint16_t base, std::uint32_t value) {
    std::uint32_t& setup = registers_.at(base + reg::shader::FloatUniformIndex);
    const bool float32 = ((setup >> Float32ModeBit) & 1U) != 0;
    const std::optional<Vec4> uniform = unit.uniform_queue.Push(value, float32);
    const std::uint32_t index = setup & UniformIndexMask;
    if (!uniform.has_value() || index >= ShaderUnitState::FloatUniforms) {
        return;
    }
    unit.float_uniforms.at(index) = *uniform;
    if (base == reg::VertexShaderBase && VertexShaderWritesReachGeometryShader()) {
        geometry_shader_.float_uniforms.at(index) = *uniform;
    }
    setup = (setup & ~UniformIndexMask) | ((index + 1) & UniformIndexMask);
}

// A fixed attribute's three words. GPUREG_FIXEDATTRIB_INDEX names the
// attribute and steps on once it is whole; 0Fh instead fills the inputs of a
// vertex given in immediate mode, one after another, and the vertex is whole
// once GPUREG_VSH_NUM_ATTR's count is reached. Source: GBATEK, Fixed Vertex
// Attributes; Azahar's SubmitImmediate.
WriteEffect PicaState::TakeFixedAttributeWord(std::uint32_t value) {
    const std::optional<Vec4> attribute = fixed_attribute_queue_.Push(value, false);
    if (!attribute.has_value()) {
        return WriteEffect::None;
    }
    std::uint32_t& index = registers_.at(reg::FixedAttributeIndex);
    if (index > ImmediateModeIndex) {
        return WriteEffect::None;
    }
    if (index < ImmediateModeIndex) {
        fixed_attributes_.at(index) = *attribute;
        ++index;
        return WriteEffect::None;
    }
    immediate_vertex_.at(immediate_attribute_) = *attribute;
    if (immediate_attribute_ <
        (registers_.at(reg::VertexShaderAttributeCount) & AttributeCountMask)) {
        ++immediate_attribute_;
        return WriteEffect::None;
    }
    immediate_attribute_ = 0;
    return WriteEffect::ImmediateVertex;
}

void PicaState::WriteLightingLut(std::uint32_t value) {
    std::uint32_t& setup = registers_.at(reg::LightingLutIndex);
    const std::uint32_t entry = setup & LutEntryMask;
    const std::uint32_t table = (setup >> LightingTableShift) & LightingTableMask;
    // Azahar has 24 tables and does not check the five bits; a word for a
    // table past them is dropped here.
    if (table < LightingLuts) {
        lighting_luts_.at(table).at(entry) = value;
    }
    setup = (setup & ~LutEntryMask) | ((entry + 1) & LutEntryMask);
}

void PicaState::WriteFogLut(std::uint32_t value) {
    std::uint32_t& setup = registers_.at(reg::FogLutIndex);
    const std::uint32_t entry = setup & FogIndexMask;
    fog_lut_.at(entry % FogLutEntries) = value;
    setup = (setup & ~FogIndexMask) | ((entry + 1) & FogIndexMask);
}

// The noise, colour map and alpha map tables have 128 entries, the colour
// and colour difference tables 256. Source: GBATEK, Procedural Texture.
void PicaState::WriteProcTexLut(std::uint32_t value) {
    constexpr std::uint32_t Noise = 0;
    constexpr std::uint32_t ColorMap = 2;
    constexpr std::uint32_t AlphaMap = 3;
    constexpr std::uint32_t Color = 4;
    constexpr std::uint32_t ColorDifference = 5;
    std::uint32_t& setup = registers_.at(reg::ProcTexLutIndex);
    const std::uint32_t entry = setup & LutEntryMask;
    const std::uint32_t table = (setup >> ProcTexTableShift) & ProcTexTableMask;
    if (table == Noise || table == ColorMap || table == AlphaMap) {
        proctex_luts_.at(table).at(entry % ProcTexSmallLut) = value;
    } else if (table == Color || table == ColorDifference) {
        proctex_luts_.at(table).at(entry) = value;
    }
    setup = (setup & ~LutEntryMask) | ((entry + 1) & LutEntryMask);
}

}  // namespace hexlith::domain::gpu
