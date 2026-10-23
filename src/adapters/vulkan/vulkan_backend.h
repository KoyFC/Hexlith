#ifndef HEXLITH_ADAPTERS_VULKAN_VULKAN_BACKEND_H
#define HEXLITH_ADAPTERS_VULKAN_VULKAN_BACKEND_H

#include "adapters/vulkan/vulkan_context.h"
#include "core/result.h"
#include "domain/gpu/fragment_stage.h"
#include "domain/gpu/gpu_memory.h"
#include "ports/gpu_backend.h"

#include <memory>

namespace hexlith::adapters::vulkan {

// Draws command lists with Vulkan. GPU memory stays what is true: a list's
// colour and depth buffers are copied from it into Vulkan images before the
// list draws and back once it has, and textures are decoded from it on the
// CPU. Vertices are shaded on the CPU by the reference's vertex pipeline;
// rasterization, the fragment stage and the output merger run on the GPU.
// ADR 0022.
class VulkanGpuBackend final : public ports::GpuBackend {
public:
    static core::Result<std::unique_ptr<VulkanGpuBackend>>
    Create(VulkanContext& context, domain::gpu::GpuMemory& memory, domain::gpu::Sampling sampling);

    VulkanGpuBackend(const VulkanGpuBackend&) = delete;
    VulkanGpuBackend& operator=(const VulkanGpuBackend&) = delete;
    VulkanGpuBackend(VulkanGpuBackend&&) = delete;
    VulkanGpuBackend& operator=(VulkanGpuBackend&&) = delete;
    ~VulkanGpuBackend() override;

    [[nodiscard]] core::Status RunCommandList(std::span<const std::uint8_t> list) override;
    [[nodiscard]] ports::GpuBackendStatistics Statistics() const override;
    [[nodiscard]] std::string Describe() const override;

    class Implementation;

private:
    explicit VulkanGpuBackend(std::unique_ptr<Implementation> implementation);

    std::unique_ptr<Implementation> implementation_;
};

}  // namespace hexlith::adapters::vulkan

#endif  // HEXLITH_ADAPTERS_VULKAN_VULKAN_BACKEND_H
