#include "domain/gpu/vertex_stage.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace hexlith::domain::gpu {
namespace {

constexpr std::uint32_t Field(std::uint32_t word, unsigned low, unsigned width) {
    return (word >> low) & ((1U << width) - 1);
}

// GPUREG_ATTR_BUF_FORMAT_LOW and HIGH as one 64-bit field: a nibble of type
// and size for each of twelve attributes, the fixed attribute mask in bits 48
// to 59, and the attribute count less one in 60 to 63.
std::uint64_t AttributeFormats(const RegisterFile& registers) {
    return registers.at(reg::AttributeFormatLow) |
           (static_cast<std::uint64_t>(registers.at(reg::AttributeFormatHigh)) << 32U);
}

constexpr std::size_t ArrayAttributes = 12;
constexpr std::uint32_t FirstPadding = 12;
constexpr std::uint32_t TypeFloat = 3;
constexpr std::uint32_t TypeShort = 2;
constexpr unsigned FixedMaskShift = 48;
constexpr unsigned CountShift = 60;

std::uint32_t ElementBytes(std::uint32_t type) {
    switch (type) {
    case TypeFloat:
        return 4;
    case TypeShort:
        return 2;
    default:
        return 1;
    }
}

std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// One component of an attribute, as a float. Bytes and halfwords are taken
// as the integers they hold, not normalised. Source: Azahar's
// VertexLoader::LoadAttribute.
float ReadComponent(const GpuMemory& memory, std::uint32_t address, std::uint32_t type) {
    const std::span<const std::uint8_t> bytes = memory.Bytes(address, ElementBytes(type));
    if (bytes.empty()) {
        return 0.0F;
    }
    switch (type) {
    case 0:
        return static_cast<float>(static_cast<std::int8_t>(bytes[0]));
    case 1:
        return static_cast<float>(bytes[0]);
    case 2:
        return static_cast<float>(static_cast<std::int16_t>(bytes[0] | (bytes[1] << 8U)));
    default:
        return std::bit_cast<float>(static_cast<std::uint32_t>(bytes[0]) |
                                    (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                                    (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                                    (static_cast<std::uint32_t>(bytes[3]) << 24U));
    }
}

}  // namespace

VertexLoader::VertexLoader(const RegisterFile& registers) {
    const std::uint64_t formats = AttributeFormats(registers);
    count_ = static_cast<std::size_t>(formats >> CountShift) + 1;
    for (std::size_t attribute = 0; attribute < Attributes; ++attribute) {
        fixed_.at(attribute) =
            attribute >= ArrayAttributes || ((formats >> (FixedMaskShift + attribute)) & 1U) != 0;
    }

    // Each of twelve arrays: its offset from the base, then its components
    // in two words, the bytes of an entry and the count of components.
    const std::uint32_t base = (registers.at(reg::AttributeBufferBase) & 0x0FFFFFFFU) << 3U;
    for (std::uint32_t array = 0; array < ArrayAttributes; ++array) {
        const auto first = static_cast<std::uint16_t>(reg::AttributeBufferFirst + (3 * array));
        const std::uint32_t offset = registers.at(first) & 0x0FFFFFFFU;
        const std::uint64_t layout = registers.at(first + 1U) |
                                     (static_cast<std::uint64_t>(registers.at(first + 2U)) << 32U);
        const auto stride = static_cast<std::uint32_t>((layout >> 48U) & 0xFFU);
        const auto components = static_cast<std::uint32_t>(layout >> 60U);
        std::uint32_t position = 0;
        for (std::uint32_t component = 0; component < components && component < ArrayAttributes;
             ++component) {
            const auto attribute = static_cast<std::uint32_t>((layout >> (4 * component)) & 0xFU);
            if (attribute >= FirstPadding) {
                // Four, eight, twelve or sixteen bytes of padding.
                position = AlignUp(position, 4) + ((attribute - FirstPadding + 1) * 4);
                continue;
            }
            const auto nibble = static_cast<std::uint32_t>((formats >> (4 * attribute)) & 0xFU);
            const std::uint32_t type = nibble & 3U;
            const std::uint32_t size = (nibble >> 2U) + 1;
            position = AlignUp(position, ElementBytes(type));
            sources_.at(attribute) = Source{.address = base + offset + position,
                                            .stride = stride,
                                            .type = type,
                                            .components = size};
            position += size * ElementBytes(type);
        }
    }
}

VertexLoader::Inputs
VertexLoader::Load(const GpuMemory& memory,
                   std::uint32_t vertex,
                   const std::array<Vec4, PicaState::FixedAttributes>& fixed) const {
    Inputs inputs{};
    for (std::size_t attribute = 0; attribute < count_ && attribute < Attributes; ++attribute) {
        if (fixed_.at(attribute)) {
            inputs.at(attribute) = fixed.at(attribute);
            continue;
        }
        const std::optional<Source>& source = sources_.at(attribute);
        if (!source.has_value()) {
            continue;
        }
        // Components the array leaves out are 0, and w 1.
        Vec4 value{0.0F, 0.0F, 0.0F, 1.0F};
        for (std::uint32_t component = 0; component < source->components; ++component) {
            value.at(component) = ReadComponent(memory,
                                                source->address + (source->stride * vertex) +
                                                    (component * ElementBytes(source->type)),
                                                source->type);
        }
        inputs.at(attribute) = value;
    }
    return inputs;
}

void PlaceInputs(const RegisterFile& registers,
                 std::uint16_t unit_base,
                 const VertexLoader::Inputs& attributes,
                 ShaderRegisters& shader) {
    const std::uint32_t last =
        Field(registers.at(unit_base + reg::shader::InputBufferConfig), 0, 4);
    const std::uint64_t permutation =
        registers.at(unit_base + reg::shader::AttributePermutationLow) |
        (static_cast<std::uint64_t>(registers.at(unit_base + reg::shader::AttributePermutationHigh))
         << 32U);
    for (std::uint32_t attribute = 0; attribute <= last; ++attribute) {
        const auto target = static_cast<std::size_t>((permutation >> (4 * attribute)) & 0xFU);
        shader.inputs.at(target) = attributes.at(attribute);
    }
}

ShaderRegisters::Bank PackOutputs(std::uint32_t output_mask, const ShaderRegisters::Bank& outputs) {
    ShaderRegisters::Bank packed{};
    std::size_t next = 0;
    for (std::size_t output = 0; output < ShaderRegisters::Count; ++output) {
        if (((output_mask >> output) & 1U) != 0) {
            packed.at(next++) = outputs.at(output);
        }
    }
    return packed;
}

OutputVertex MakeOutputVertex(const RegisterFile& registers, const ShaderRegisters::Bank& packed) {
    // GPUREG_SH_OUTMAP_TOTAL's count, then for each output a byte for each
    // component naming its semantic; 1Fh and the other unnamed numbers land
    // past the slots the rasterizer reads.
    constexpr std::uint16_t OutputMapTotal = 0x04F;
    constexpr std::uint16_t FirstOutputMap = 0x050;
    constexpr std::size_t Semantics = 0x20;
    std::array<float, Semantics> slots{};
    slots.fill(1.0F);
    const std::uint32_t count = Field(registers.at(OutputMapTotal), 0, 3);
    for (std::uint32_t output = 0; output < count; ++output) {
        const std::uint32_t map = registers.at(static_cast<std::uint16_t>(FirstOutputMap + output));
        for (std::uint32_t component = 0; component < 4; ++component) {
            slots.at(Field(map, 8 * component, 5)) = packed.at(output).at(component);
        }
    }
    OutputVertex vertex;
    std::copy_n(slots.begin(), OutputVertex::Slots, vertex.values.begin());
    for (std::size_t channel = 0; channel < 4; ++channel) {
        float& color = vertex.values.at(OutputVertex::Color + channel);
        const float magnitude = std::fabs(color);
        color = magnitude < 1.0F ? magnitude : 1.0F;
    }
    return vertex;
}

void PrimitiveAssembler::Restart(Topology topology) {
    topology_ = topology;
    index_ = 0;
    ready_ = false;
    reversed_ = false;
}

std::optional<PrimitiveAssembler::Triangle> PrimitiveAssembler::Submit(const OutputVertex& vertex) {
    switch (topology_) {
    case Topology::List:
    case Topology::Shader:
        if (index_ < 2) {
            buffer_.at(index_++) = vertex;
            return std::nullopt;
        }
        index_ = 0;
        if (topology_ == Topology::Shader && reversed_) {
            reversed_ = false;
            return Triangle{buffer_[1], buffer_[0], vertex};
        }
        return Triangle{buffer_[0], buffer_[1], vertex};
    case Topology::Strip:
    case Topology::Fan: {
        std::optional<Triangle> triangle;
        if (ready_) {
            triangle = Triangle{buffer_[0], buffer_[1], vertex};
        }
        buffer_.at(index_) = vertex;
        ready_ = ready_ || index_ == 1;
        // A strip alternates which vertex it replaces; a fan keeps the first.
        index_ = topology_ == Topology::Strip ? 1 - index_ : 1;
        return triangle;
    }
    }
    return std::nullopt;
}

}  // namespace hexlith::domain::gpu
