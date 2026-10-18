#ifndef HEXLITH_DOMAIN_GPU_SHADER_INTERPRETER_H
#define HEXLITH_DOMAIN_GPU_SHADER_INTERPRETER_H

#include "domain/gpu/pica_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace hexlith::domain::gpu {

// The registers of one shader unit while it runs a program. GBATEK, 3DS GPU
// Shader Instruction Set - Blurp, Registers: sixteen inputs, sixteen
// temporaries, sixteen outputs; the address registers a0.x, a0.y and aL; the
// two condition flags CMP sets. The outputs are two banks, which a geometry
// shader's EMIT switches between (Azahar's ShaderUnit).
struct ShaderRegisters {
    static constexpr std::size_t Count = 16;
    using Bank = std::array<Vec4, Count>;

    Bank inputs{};
    Bank temporaries{};
    std::array<Bank, 2> outputs{};
    std::size_t output_bank = 0;
    // a0.x, a0.y and aL.
    std::array<std::int32_t, 3> address{};
    std::array<bool, 2> conditions{};
};

// What a geometry shader's EMIT and SETEMIT build: up to three vertices,
// each its outputs packed by the unit's output mask, and the primitives
// emitted from them. The vertex buffer is not cleared between primitives, so
// a primitive can reuse its predecessor's vertices. Source: GBATEK, SETEMIT;
// GeometryEmitter in Azahar's shader_unit.cpp.
class GeometryEmitter {
public:
    static constexpr std::size_t Vertices = 3;

    struct Primitive {
        std::array<ShaderRegisters::Bank, Vertices> vertices{};
        // Whether SETEMIT asked for the winding to be reversed.
        bool reversed = false;
    };

    explicit GeometryEmitter(std::uint32_t output_mask) : output_mask_(output_mask) {}

    void Configure(std::uint32_t vertex, bool primitive, bool reversed) {
        vertex_ = vertex;
        primitive_ = primitive;
        reversed_ = reversed;
    }

    // Stores the outputs as the configured vertex, and emits a primitive if
    // SETEMIT asked for one. False for a vertex number past the buffer.
    bool Emit(const ShaderRegisters::Bank& outputs);

    [[nodiscard]] const std::vector<Primitive>& Primitives() const {
        return primitives_;
    }

    // The primitives emitted so far, leaving none. The vertex buffer and
    // what SETEMIT last configured stay for the next run.
    std::vector<Primitive> TakePrimitives() {
        return std::exchange(primitives_, {});
    }

    // GPUREG_GSH_OUTMAP_MASK, which the next draw may have changed.
    void SetOutputMask(std::uint32_t output_mask) {
        output_mask_ = output_mask;
    }

private:
    std::uint32_t output_mask_;
    std::uint32_t vertex_ = 0;
    bool primitive_ = false;
    bool reversed_ = false;
    std::array<ShaderRegisters::Bank, Vertices> buffer_{};
    std::vector<Primitive> primitives_;
};

// ADR 0018: float32, or float32 rounded to float24 after every operation.
enum class ShaderPrecision : std::uint8_t { Float32, Float24 };

struct ShaderRunResult {
    std::uint64_t instructions = 0;
    // An instruction Hexlith does not implement was reached: DST, DSTI,
    // LITP, an opcode nobody has described, or EMIT and SETEMIT with no
    // geometry emitter, or a vertex number past the emitter's three.
    bool unsupported = false;
    // The run was stopped after its limit of instructions.
    bool stopped = false;
};

struct ShaderRun {
    std::uint32_t entry = 0;
    // Only for a geometry shader.
    GeometryEmitter* emitter = nullptr;
    ShaderPrecision precision = ShaderPrecision::Float32;
    // A malformed program could run for ever; the reference title's longest
    // run is a few hundred instructions.
    static constexpr std::uint64_t DefaultLimit = std::uint64_t{1} << 20U;
    std::uint64_t limit = DefaultLimit;
};

// Runs a unit's program from an entry point to END. ADR 0018 has the rules,
// and docs/pica-shaders.md how they were checked against Azahar.
ShaderRunResult
RunShader(const ShaderUnitState& unit, ShaderRegisters& registers, const ShaderRun& run);

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_SHADER_INTERPRETER_H
