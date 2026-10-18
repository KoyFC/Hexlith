#ifndef HEXLITH_DOMAIN_GPU_REFERENCE_RENDERER_H
#define HEXLITH_DOMAIN_GPU_REFERENCE_RENDERER_H

#include "domain/gpu/command_processor.h"
#include "domain/gpu/fragment_stage.h"
#include "domain/gpu/gpu_memory.h"
#include "domain/gpu/pica_state.h"
#include "domain/gpu/rasterizer.h"
#include "domain/gpu/shader_interpreter.h"
#include "domain/gpu/vertex_stage.h"

#include <cstddef>
#include <cstdint>

namespace hexlith::domain::gpu {

// What the reference renderer has done, and whether it met anything it does
// not implement.
struct ReferenceRendererStatistics {
    std::uint64_t draws = 0;
    std::uint64_t vertex_shader_runs = 0;
    std::uint64_t geometry_shader_runs = 0;
    // A shader reached something the interpreter does not implement, or a
    // geometry shader mode other than the one that takes vertices whole.
    bool unsupported_shader = false;
};

// The whole pipeline in software, as a command processor's executor: vertex
// loading, the vertex shader, the geometry shader, primitive assembly and
// the reference rasterizer, drawing into GPU memory. It is what the Vulkan
// backend is checked against, and is itself checked against Azahar's
// software renderer (docs/pica-rasterizer.md).
class ReferenceRenderer {
public:
    explicit ReferenceRenderer(GpuMemory& memory,
                               Sampling sampling = Sampling::Hardware,
                               ShaderPrecision precision = ShaderPrecision::Float32)
        : memory_(&memory), precision_(precision), rasterizer_(memory, sampling), emitter_(0) {}

    void Draw(PicaState& state, bool indexed);
    void ImmediateVertex(PicaState& state);
    void RestartPrimitive(PicaState& state);

    void Finalize() {}

    [[nodiscard]] const ReferenceRendererStatistics& Statistics() const {
        return statistics_;
    }

    [[nodiscard]] const RasterizerStatistics& Rasterization() const {
        return rasterizer_.Statistics();
    }

private:
    // Sets up the geometry stage for a draw: whether a geometry shader runs,
    // and its input buffer empty again.
    void ConfigureGeometryStage(const PicaState& state);
    // Runs the vertex shader on one vertex's attributes.
    ShaderRegisters::Bank ShadeVertex(const PicaState& state,
                                      const VertexLoader::Inputs& attributes,
                                      ShaderRegisters& registers);
    // Hands a vertex shader's packed outputs on, to the geometry shader or
    // straight to primitive assembly.
    void SubmitVertex(PicaState& state, const ShaderRegisters::Bank& packed);
    void RunGeometryShader(PicaState& state);
    void Assemble(const PicaState& state, const ShaderRegisters::Bank& packed);

    GpuMemory* memory_;
    ShaderPrecision precision_;
    Rasterizer rasterizer_;
    PrimitiveAssembler assembler_;
    ReferenceRendererStatistics statistics_;

    // The geometry shader keeps its registers and its emitter's state from one
    // run to the next, and from one draw to the next (Azahar's
    // GeometryShaderUnit, which the PicaCore owns for good).
    bool geometry_shader_ = false;
    ShaderRegisters geometry_registers_;
    GeometryEmitter emitter_;
    VertexLoader::Inputs geometry_inputs_{};
    std::size_t geometry_filled_ = 0;
    std::size_t geometry_needed_ = 0;
    std::size_t vertex_outputs_ = 0;
};

static_assert(PicaExecutor<ReferenceRenderer>);

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_REFERENCE_RENDERER_H
