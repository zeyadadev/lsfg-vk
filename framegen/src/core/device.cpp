#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/device.hpp"
#include "core/instance.hpp"
#include "common/exception.hpp"

#include <cstdint>
#include <memory>

using namespace LSFG::Core;

Device::Device(const Instance& instance,
        PFN_vkGetDeviceProcAddr getDeviceProcAddr,
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        uint32_t computeFamilyIdx,
        VkQueue computeQueue) {
    if (getDeviceProcAddr == nullptr
            || instance.handle() == VK_NULL_HANDLE
            || physicalDevice == VK_NULL_HANDLE
            || device == VK_NULL_HANDLE
            || computeQueue == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Null handle or GDPA provided to Core::Device");

    vkGetDeviceProcAddr = getDeviceProcAddr;
    volkLoadDevice(device);
#if defined(VK_KHR_synchronization2)
    if (vkCmdPipelineBarrier2 == nullptr && vkCmdPipelineBarrier2KHR != nullptr)
        vkCmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
            vkCmdPipelineBarrier2KHR);
#endif

    this->computeQueue = computeQueue;
    this->computeFamilyIdx = computeFamilyIdx;
    this->physicalDevice = physicalDevice;

    // non-owning: deleter does not call vkDestroyDevice
    this->device = std::shared_ptr<VkDevice>(
        new VkDevice(device),
        [](const VkDevice* handle) { delete handle; }
    );
}
