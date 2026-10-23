#ifndef HEXLITH_ADAPTERS_VULKAN_VULKAN_CONTEXT_H
#define HEXLITH_ADAPTERS_VULKAN_VULKAN_CONTEXT_H

#include "core/result.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace hexlith::adapters::vulkan {

// What the device promises about float arithmetic in shaders, beyond what
// Vulkan requires. Source: VkPhysicalDeviceFloatControlsProperties, Vulkan
// 1.2.
struct FloatControls {
    // Signed zeros, infinities and NaNs kept, rather than assumed away.
    bool keeps_special_values = false;
    // Denormals kept, rather than flushed to zero.
    bool keeps_denormals = false;
};

struct VulkanOptions {
    // A part of the device's name to choose it by, or empty for the first
    // device that has a graphics queue.
    std::string device;
    // Load VK_LAYER_KHRONOS_validation when it is installed. Every message it
    // gives at warning level or above is counted.
    bool validation = true;
};

// A Vulkan instance and device with one graphics and compute queue, no
// window and no surface: what is drawn is read back into memory.
class VulkanContext {
public:
    static core::Result<std::unique_ptr<VulkanContext>> Create(const VulkanOptions& options);

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&) = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;
    ~VulkanContext();

    [[nodiscard]] VkDevice Device() const {
        return device_;
    }

    [[nodiscard]] VkPhysicalDevice PhysicalDevice() const {
        return physical_;
    }

    [[nodiscard]] const std::string& DeviceName() const {
        return device_name_;
    }

    [[nodiscard]] const FloatControls& Floats() const {
        return floats_;
    }

    // Validation messages at warning level or above so far, and the first.
    [[nodiscard]] std::uint64_t ValidationMessages() const {
        return validation_messages_;
    }

    [[nodiscard]] const std::string& FirstValidationMessage() const {
        return first_validation_message_;
    }

    // Whether the device has what the backend draws with: logic operations
    // and clip distances.
    [[nodiscard]] bool CanDraw() const {
        return can_draw_;
    }

    [[nodiscard]] bool Validating() const {
        return messenger_ != VK_NULL_HANDLE;
    }

    // Records commands into a one-time command buffer, submits it and waits
    // for it to finish.
    core::Status Submit(const std::function<void(VkCommandBuffer)>& record);

    // A memory type with the properties, among those the mask allows.
    [[nodiscard]] std::optional<std::uint32_t> MemoryType(std::uint32_t allowed,
                                                          VkMemoryPropertyFlags properties) const;

    void NoteValidationMessage(const char* message);

private:
    VulkanContext() = default;

    core::Status CreateInstance(const VulkanOptions& options);
    core::Status ChooseDevice(const VulkanOptions& options);
    core::Status CreateDevice();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::string device_name_;
    FloatControls floats_;
    bool can_draw_ = false;
    std::uint64_t validation_messages_ = 0;
    std::string first_validation_message_;
};

// A buffer in memory the host can see, mapped for as long as it lives.
class HostBuffer {
public:
    static core::Result<std::unique_ptr<HostBuffer>>
    Create(VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage);

    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;
    HostBuffer(HostBuffer&&) = delete;
    HostBuffer& operator=(HostBuffer&&) = delete;
    ~HostBuffer();

    [[nodiscard]] VkBuffer Buffer() const {
        return buffer_;
    }

    [[nodiscard]] std::span<std::uint8_t> Bytes() const {
        return {static_cast<std::uint8_t*>(mapped_), static_cast<std::size_t>(size_)};
    }

private:
    explicit HostBuffer(VulkanContext& context) : context_(&context) {}

    VulkanContext* context_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    void* mapped_ = nullptr;
};

// A failed Vulkan call as an error naming it.
core::Status Check(VkResult result, const char* call);

}  // namespace hexlith::adapters::vulkan

#endif  // HEXLITH_ADAPTERS_VULKAN_VULKAN_CONTEXT_H
