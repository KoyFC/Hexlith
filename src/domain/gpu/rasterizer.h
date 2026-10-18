#ifndef HEXLITH_DOMAIN_GPU_RASTERIZER_H
#define HEXLITH_DOMAIN_GPU_RASTERIZER_H

#include "domain/gpu/fragment_stage.h"
#include "domain/gpu/gpu_memory.h"
#include "domain/gpu/pica_state.h"
#include "domain/gpu/vertex_stage.h"

#include <cstdint>

namespace hexlith::domain::gpu {

// What the rasterizer has done since it was made.
struct RasterizerStatistics {
    std::uint64_t triangles = 0;
    // Pixels a triangle covered, whether or not a test then dropped them.
    std::uint64_t fragments = 0;
    // A triangle asked for something not implemented: a fragment operation
    // mode other than the default, a colour or depth format the registers do
    // not name, or a texture unit setting SampleTextures refuses.
    bool unsupported = false;
};

// The reference rasterizer: clips a triangle, sets it up in 12.4 fixed
// point, and runs each pixel it covers through texturing, lighting, the
// combiners and the output merger into the colour and depth buffers in GPU
// memory. It follows Azahar's software rasterizer, the only description
// detailed enough to check pixel for pixel; docs/pica-rasterizer.md has each
// rule's source and where GBATEK says more.
class Rasterizer {
public:
    Rasterizer(GpuMemory& memory, Sampling sampling) : memory_(&memory), sampling_(sampling) {}

    // Draws one triangle with the registers as they stand.
    void DrawTriangle(const PicaState& state, const PrimitiveAssembler::Triangle& triangle);

    [[nodiscard]] const RasterizerStatistics& Statistics() const {
        return statistics_;
    }

private:
    GpuMemory* memory_;
    Sampling sampling_;
    RasterizerStatistics statistics_;
};

}  // namespace hexlith::domain::gpu

#endif  // HEXLITH_DOMAIN_GPU_RASTERIZER_H
