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
#include <vector>

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

// The vertex half of the pipeline in software: vertex loading, the vertex
// shader, the geometry shader and primitive assembly, which leave each draw's
// triangles, as the rasterizer takes them, in Triangles(). The reference
// renderer rasterizes them; the Vulkan backend draws them when a geometry
// shader or immediate mode keeps the vertices on the CPU (ADR 0022).
class VertexPipeline {
public:
    explicit VertexPipeline(GpuMemory& memory, ShaderPrecision precision = ShaderPrecision::Float32)
        : memory_(&memory), precision_(precision), emitter_(0) {}

    void Draw(PicaState& state, bool indexed);
    void ImmediateVertex(PicaState& state);
    void RestartPrimitive(PicaState& state);

    // The triangles assembled so far, for the caller to take and clear.
    [[nodiscard]] std::vector<PrimitiveAssembler::Triangle>& Triangles() {
        return triangles_;
    }

    [[nodiscard]] const ReferenceRendererStatistics& Statistics() const {
        return statistics_;
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
    PrimitiveAssembler assembler_;
    ReferenceRendererStatistics statistics_;
    std::vector<PrimitiveAssembler::Triangle> triangles_;

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

// The whole pipeline in software, as a command processor's executor: the
// vertex pipeline, then the reference rasterizer, drawing into GPU memory.
// It is what the Vulkan backend is checked against, and is itself checked
// against Azahar's software renderer (docs/pica-rasterizer.md).
class ReferenceRenderer {
public:
    explicit ReferenceRenderer(GpuMemory& memory,
                               Sampling sampling = Sampling::Hardware,
                               ShaderPrecision precision = ShaderPrecision::Float32)
        : vertices_(memory, precision), rasterizer_(memory, sampling) {}

    void Draw(PicaState& state, bool indexed);
    void ImmediateVertex(PicaState& state);

    void RestartPrimitive(PicaState& state) {
        vertices_.RestartPrimitive(state);
    }

    void Finalize() {}

    [[nodiscard]] const ReferenceRendererStatistics& Statistics() const {
        return vertices_.Statistics();
    }

    [[nodiscard]] const RasterizerStatistics& Rasterization() const {
        return rasterizer_.Statistics();
    }

private:
    void Rasterize(const PicaState& state);

    VertexPipeline vertices_;
    Rasterizer rasterizer_;
};

static_assert(PicaExecutor<ReferenceRenderer>);

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_REFERENCE_RENDERER_H
