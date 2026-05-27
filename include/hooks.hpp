#pragma once

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <string>

namespace Hooks {

    /// Vulkan device information structure.
    struct DeviceInfo {
        VkInstance instance;
        VkDevice device;
        VkPhysicalDevice physicalDevice;
        std::pair<uint32_t, VkQueue> queue; // graphics family + queue
        std::pair<uint32_t, VkQueue> computeQueue; // compute family + queue
    };

    /// Map of hooked Vulkan functions.
    extern std::unordered_map<std::string, PFN_vkVoidFunction> hooks;

}
