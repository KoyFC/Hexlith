#ifndef HEXLITH_ADAPTERS_VULKAN_VULKAN_CONTEXT_H
#define HEXLITH_ADAPTERS_VULKAN_VULKAN_CONTEXT_H

#include "core/result.h"

#include <cstdint>
#include <deque>
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

// Names a submission that has been handed to the queue, in the order they
// were: waiting for one is waiting for every one before it. Zero is none.
using Ticket = std::uint64_t;

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

    // Records commands into a one-time command buffer, submits it and waits
    // for it to finish.
    core::Status Submit(const std::function<void(VkCommandBuffer)>& record);

    // The same without the wait. Whatever the commands read has to stay
    // as it is, and what they write is not there to be read, until Wait
    // returns for the ticket or Finished says it is over. The queue runs
    // submissions in the order they were made, but the second only sees what
    // the first wrote if its commands say so with a barrier.
    core::Result<Ticket> SubmitWithoutWaiting(const std::function<void(VkCommandBuffer)>& record);

    // The ticket of the last submission, which is the one to wait for to wait
    // for all of them.
    [[nodiscard]] Ticket LastTicket() const {
        return last_ticket_;
    }

    // Blocks until the submission, and so every one before it, has finished.
    core::Status Wait(Ticket ticket);

    // Whether the submission has finished, without blocking.
    [[nodiscard]] bool Finished(Ticket ticket);

    // A memory type with the properties, among those the mask allows.
    [[nodiscard]] std::optional<std::uint32_t> MemoryType(std::uint32_t allowed,
                                                          VkMemoryPropertyFlags properties) const;

    void NoteValidationMessage(const char* message);

private:
    VulkanContext() = default;

    core::Status CreateInstance(const VulkanOptions& options);
    core::Status ChooseDevice(const VulkanOptions& options);
    core::Status CreateDevice();

    // A submission the device may still be running: its fence, and the
    // command buffer that is freed when it is over.
    struct InFlight {
        Ticket ticket;
        VkFence fence;
        VkCommandBuffer commands;
    };

    // Frees the submissions up to the ticket that have finished, waiting for
    // each when asked to.
    core::Status Retire(Ticket upto, bool wait);

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
    std::deque<InFlight> in_flight_;
    Ticket last_ticket_ = 0;
    Ticket finished_ = 0;
};

// What the host does with a buffer's bytes. One it reads back is put in
// memory the host caches where the device offers it: on an integrated GPU the
// first host-visible type is often uncached, and reading a picture back from
// it a pixel at a time costs far more than drawing the picture did.
enum class HostUse : std::uint8_t { Write, ReadBack };

// A buffer in memory the host can see, mapped for as long as it lives.
class HostBuffer {
public:
    static core::Result<std::unique_ptr<HostBuffer>> Create(VulkanContext& context,
                                                            VkDeviceSize size,
                                                            VkBufferUsageFlags usage,
                                                            HostUse use = HostUse::Write);

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
