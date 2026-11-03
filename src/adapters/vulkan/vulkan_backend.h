#ifndef HEXLITH_ADAPTERS_VULKAN_VULKAN_BACKEND_H
#define HEXLITH_ADAPTERS_VULKAN_VULKAN_BACKEND_H

#include "adapters/vulkan/vulkan_context.h"
#include "core/result.h"
#include "domain/gpu/fragment_stage.h"
#include "domain/gpu/gpu_memory.h"
#include "ports/gpu_backend.h"

#include <cstdint>
#include <memory>

namespace hexlith::adapters::vulkan {

// Where a draw's vertices are shaded. On the GPU, by the translation of the
// vertex program, for every draw that has no geometry shader and is a list of
// whole triangles; the others on the CPU by the reference's vertex pipeline,
// as all of them are with Cpu.
enum class VertexShading : std::uint8_t { Cpu, GpuWherePossible };

// Draws command lists with Vulkan. A target's colour and depth buffers are
// copied from GPU memory into Vulkan images the first time they are drawn to
// and stay there; they are written back when BeforeRead, a texture or another
// target over the same bytes needs memory true, and dropped when BeforeWrite
// says the bytes are being replaced (ADR 0030). Textures are decoded from
// memory on the CPU. Rasterization, the fragment stage and the output merger run on the
// GPU, and so do the vertex shaders of the draws VertexShading allows.
// ADR 0022 and ADR 0030.
class VulkanGpuBackend final : public ports::GpuBackend {
public:
    static core::Result<std::unique_ptr<VulkanGpuBackend>>
    Create(VulkanContext& context,
           domain::gpu::GpuMemory& memory,
           domain::gpu::Sampling sampling,
           VertexShading shading = VertexShading::GpuWherePossible);

    VulkanGpuBackend(const VulkanGpuBackend&) = delete;
    VulkanGpuBackend& operator=(const VulkanGpuBackend&) = delete;
    VulkanGpuBackend(VulkanGpuBackend&&) = delete;
    VulkanGpuBackend& operator=(VulkanGpuBackend&&) = delete;
    ~VulkanGpuBackend() override;

    [[nodiscard]] core::Status RunCommandList(std::span<const std::uint8_t> list) override;
    [[nodiscard]] core::Status BeforeRead(std::uint32_t physical, std::uint32_t size) override;
    [[nodiscard]] core::Status BeforeWrite(std::uint32_t physical, std::uint32_t size) override;
    [[nodiscard]] ports::GpuBackendStatistics Statistics() const override;
    [[nodiscard]] std::string Describe() const override;

    class Implementation;

private:
    explicit VulkanGpuBackend(std::unique_ptr<Implementation> implementation);

    std::unique_ptr<Implementation> implementation_;
};

}  // namespace hexlith::adapters::vulkan

#endif  // HEXLITH_ADAPTERS_VULKAN_VULKAN_BACKEND_H
