#include "adapters/vulkan/vulkan_context.h"

#include <array>
#include <format>
#include <optional>
#include <string_view>

namespace hexlith::adapters::vulkan {
namespace {

constexpr const char* ValidationLayer = "VK_LAYER_KHRONOS_validation";

VKAPI_ATTR VkBool32 VKAPI_CALL
OnValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT /*severity*/,
                    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                    const VkDebugUtilsMessengerCallbackDataEXT* data,
                    void* context) {
    static_cast<VulkanContext*>(context)->NoteValidationMessage(data->pMessage);
    return VK_FALSE;
}

bool LayerAvailable(const char* name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& layer : layers) {
        if (std::string_view{static_cast<const char*>(layer.layerName)} == name) {
            return true;
        }
    }
    return false;
}

}  // namespace

core::Status Check(VkResult result, const char* call) {
    if (result != VK_SUCCESS) {
        return core::Error{
            std::format("{} failed with VkResult {}", call, static_cast<int>(result))};
    }
    return {};
}

core::Result<std::unique_ptr<VulkanContext>> VulkanContext::Create(const VulkanOptions& options) {
    std::unique_ptr<VulkanContext> context{new VulkanContext()};
    if (const core::Status instance = context->CreateInstance(options); !instance) {
        return instance.Failure();
    }
    if (const core::Status chosen = context->ChooseDevice(options); !chosen) {
        return chosen.Failure();
    }
    if (const core::Status created = context->CreateDevice(); !created) {
        return created.Failure();
    }
    return context;
}

core::Status VulkanContext::CreateInstance(const VulkanOptions& options) {
    const bool validation = options.validation && LayerAvailable(ValidationLayer);
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Hexlith";
    application.apiVersion = VK_API_VERSION_1_2;

    VkDebugUtilsMessengerCreateInfoEXT messenger{};
    messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    // The loader's own messages, such as a driver it skips, are not about
    // what Hexlith asks of Vulkan.
    messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    messenger.pfnUserCallback = OnValidationMessage;
    messenger.pUserData = this;

    const std::array<const char*, 1> layers = {ValidationLayer};
    const std::array<const char*, 1> extensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkInstanceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &application;
    if (validation) {
        create.pNext = &messenger;
        create.enabledLayerCount = 1;
        create.ppEnabledLayerNames = layers.data();
        create.enabledExtensionCount = 1;
        create.ppEnabledExtensionNames = extensions.data();
    }
    if (const core::Status created =
            Check(vkCreateInstance(&create, nullptr, &instance_), "vkCreateInstance");
        !created) {
        return created;
    }
    if (validation) {
        const auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create_messenger != nullptr) {
            return Check(create_messenger(instance_, &messenger, nullptr, &messenger_),
                         "vkCreateDebugUtilsMessengerEXT");
        }
    }
    return {};
}

core::Status VulkanContext::ChooseDevice(const VulkanOptions& options) {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        const std::string name{static_cast<const char*>(properties.deviceName)};
        if ((!options.device.empty() && name.find(options.device) == std::string::npos) ||
            properties.apiVersion < VK_API_VERSION_1_2) {
            continue;
        }
        std::uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &families, nullptr);
        std::vector<VkQueueFamilyProperties> family_properties(families);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &families, family_properties.data());
        for (std::uint32_t family = 0; family < families; ++family) {
            const VkQueueFlags wanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((family_properties[family].queueFlags & wanted) == wanted) {
                physical_ = device;
                queue_family_ = family;
                device_name_ = name;
                VkPhysicalDeviceFloatControlsProperties floats{};
                floats.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES;
                VkPhysicalDeviceProperties2 all{};
                all.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                all.pNext = &floats;
                vkGetPhysicalDeviceProperties2(device, &all);
                floats_.keeps_special_values =
                    floats.shaderSignedZeroInfNanPreserveFloat32 == VK_TRUE;
                floats_.keeps_denormals = floats.shaderDenormPreserveFloat32 == VK_TRUE;
                return {};
            }
        }
    }
    return core::Error{options.device.empty()
                           ? std::string{"no Vulkan 1.2 device has a graphics queue"}
                           : std::format("no Vulkan device named like '{}'", options.device)};
}

core::Status VulkanContext::CreateDevice() {
    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = queue_family_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    // The output merger's logic operations and the custom clip plane need
    // these; a device without them still checks shaders.
    VkPhysicalDeviceFeatures available{};
    vkGetPhysicalDeviceFeatures(physical_, &available);
    VkPhysicalDeviceFeatures enabled{};
    enabled.logicOp = available.logicOp;
    enabled.shaderClipDistance = available.shaderClipDistance;
    can_draw_ = available.logicOp == VK_TRUE && available.shaderClipDistance == VK_TRUE;
    VkDeviceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queue;
    create.pEnabledFeatures = &enabled;
    if (const core::Status created =
            Check(vkCreateDevice(physical_, &create, nullptr, &device_), "vkCreateDevice");
        !created) {
        return created;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queue_family_;
    return Check(vkCreateCommandPool(device_, &pool, nullptr, &pool_), "vkCreateCommandPool");
}

VulkanContext::~VulkanContext() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (messenger_ != VK_NULL_HANDLE) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy != nullptr) {
            destroy(instance_, messenger_, nullptr);
        }
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

void VulkanContext::NoteValidationMessage(const char* message) {
    if (validation_messages_++ == 0) {
        first_validation_message_ = message;
    }
}

core::Status VulkanContext::Submit(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = pool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    if (const core::Status allocated = Check(
            vkAllocateCommandBuffers(device_, &allocate, &commands), "vkAllocateCommandBuffers");
        !allocated) {
        return allocated;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands, &begin);
    record(commands);
    vkEndCommandBuffer(commands);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    core::Status status = Check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    if (status) {
        status = Check(vkQueueWaitIdle(queue_), "vkQueueWaitIdle");
    }
    vkFreeCommandBuffers(device_, pool_, 1, &commands);
    return status;
}

std::optional<std::uint32_t> VulkanContext::MemoryType(std::uint32_t allowed,
                                                       VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
    for (std::uint32_t type = 0; type < memory.memoryTypeCount; ++type) {
        if ((allowed & (1U << type)) != 0 &&
            (memory.memoryTypes[type].propertyFlags & properties) == properties) {
            return type;
        }
    }
    return std::nullopt;
}

core::Result<std::unique_ptr<HostBuffer>>
HostBuffer::Create(VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage) {
    std::unique_ptr<HostBuffer> buffer{new HostBuffer(context)};
    buffer->size_ = size;
    VkBufferCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    create.size = size;
    create.usage = usage;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkDevice device = context.Device();
    if (const core::Status created =
            Check(vkCreateBuffer(device, &create, nullptr, &buffer->buffer_), "vkCreateBuffer");
        !created) {
        return created.Failure();
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer->buffer_, &requirements);
    const std::optional<std::uint32_t> type = context.MemoryType(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!type) {
        return core::Error{"no memory the host can see for a buffer"};
    }
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = *type;
    if (const core::Status allocated = Check(
            vkAllocateMemory(device, &allocate, nullptr, &buffer->memory_), "vkAllocateMemory");
        !allocated) {
        return allocated.Failure();
    }
    vkBindBufferMemory(device, buffer->buffer_, buffer->memory_, 0);
    if (const core::Status mapped = Check(
            vkMapMemory(device, buffer->memory_, 0, size, 0, &buffer->mapped_), "vkMapMemory");
        !mapped) {
        return mapped.Failure();
    }
    return buffer;
}

HostBuffer::~HostBuffer() {
    VkDevice device = context_->Device();
    if (mapped_ != nullptr) {
        vkUnmapMemory(device, memory_);
    }
    vkDestroyBuffer(device, buffer_, nullptr);
    vkFreeMemory(device, memory_, nullptr);
}

}  // namespace hexlith::adapters::vulkan
