#include "hooks.hpp"
#include "common/exception.hpp"
#include "config/config.hpp"
#include "utils/utils.hpp"
#include "context.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>
#include <lsfg_3_1.hpp>
#include <lsfg_3_1p.hpp>

#include <unordered_map>
#include <filesystem>
#include <stdexcept>
#include <algorithm>
#include <exception>
#include <iostream>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Hooks;

namespace {

    /// The most-recently-created VkInstance. Most apps create exactly one; if
    /// they create more, the last one wins. We snapshot this into DeviceInfo
    /// when the device is created so framegen can be initialised against it.
    VkInstance lastCreatedInstance = VK_NULL_HANDLE;

    ///
    /// Add extensions to the instance create info.
    ///
    VkResult myvkCreateInstance(
            const VkInstanceCreateInfo* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkInstance* pInstance) {
        auto extensions = Utils::addExtensions(
            pCreateInfo->ppEnabledExtensionNames,
            pCreateInfo->enabledExtensionCount,
            {
                "VK_KHR_get_physical_device_properties2",
                "VK_KHR_external_memory_capabilities",
                "VK_KHR_external_semaphore_capabilities"
            }
        );
        VkInstanceCreateInfo createInfo = *pCreateInfo;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        auto res = Layer::ovkCreateInstance(&createInfo, pAllocator, pInstance);
        if (res == VK_ERROR_EXTENSION_NOT_PRESENT)
            throw std::runtime_error(
                "Required Vulkan instance extensions are not present."
                "Your GPU driver is not supported.");
        if (res == VK_SUCCESS)
            lastCreatedInstance = *pInstance;
        return res;
    }

    /// Map of devices to related information.
    std::unordered_map<VkDevice, DeviceInfo> deviceToInfo;

    std::unordered_map<VkSwapchainKHR, LsContext> swapchains;
    std::unordered_map<VkSwapchainKHR, VkDevice> swapchainToDeviceTable;
    std::unordered_map<VkSwapchainKHR, VkPresentModeKHR> swapchainToPresent;

    ///
    /// Walk pCreateInfo->pNext, OR-in framegen's required feature bits where the
    /// app already provides the relevant feature struct. Returns a triple of
    /// flags indicating which struct types we still need to allocate ourselves.
    ///
    struct FoundFeatureStructs {
        bool features12{false};
        bool sync2{false};
        bool robustness2{false};
    };
    FoundFeatureStructs orInFeatures(const VkDeviceCreateInfo* pCreateInfo) {
        FoundFeatureStructs found{};
        auto* base = const_cast<VkBaseOutStructure*>(
            reinterpret_cast<const VkBaseOutStructure*>(pCreateInfo->pNext));
        while (base != nullptr) {
            switch (base->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
                    auto* f = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(base);
                    f->vulkanMemoryModel = VK_TRUE;
                    f->timelineSemaphore = VK_TRUE;
                    found.features12 = true;
                    break;
                }
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
                    auto* f = reinterpret_cast<VkPhysicalDeviceVulkan13Features*>(base);
                    f->synchronization2 = VK_TRUE;
                    found.sync2 = true;
                    break;
                }
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES: {
                    auto* f = reinterpret_cast<VkPhysicalDeviceSynchronization2Features*>(base);
                    f->synchronization2 = VK_TRUE;
                    found.sync2 = true;
                    break;
                }
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
                    auto* f = reinterpret_cast<VkPhysicalDeviceRobustness2FeaturesEXT*>(base);
                    f->nullDescriptor = VK_TRUE;
                    found.robustness2 = true;
                    break;
                }
                default:
                    break;
            }
            base = base->pNext;
        }
        return found;
    }

    ///
    /// Add extensions, features, and a compute queue to the device create info.
    /// (function pointers are not initialized yet)
    ///
    VkResult myvkCreateDevicePre(
            VkPhysicalDevice physicalDevice,
            const VkDeviceCreateInfo* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkDevice* pDevice) {
        // 1. extensions
        auto extensions = Utils::addExtensions(
            pCreateInfo->ppEnabledExtensionNames,
            pCreateInfo->enabledExtensionCount,
            {
                "VK_KHR_external_memory",
                "VK_KHR_external_memory_fd",
                "VK_KHR_external_semaphore",
                "VK_KHR_external_semaphore_fd",
                "VK_KHR_synchronization2",
                "VK_EXT_robustness2",
            }
        );

        // 2. features — OR-in to existing structs, allocate the rest
        const FoundFeatureStructs found = orInFeatures(pCreateInfo);

        // thread_local so the structs outlive this call into the driver
        thread_local VkPhysicalDeviceRobustness2FeaturesEXT robustness2Storage{};
        thread_local VkPhysicalDeviceSynchronization2Features sync2Storage{};
        thread_local VkPhysicalDeviceVulkan12Features features12Storage{};

        const void* newPNext = pCreateInfo->pNext;
        if (!found.robustness2) {
            robustness2Storage = VkPhysicalDeviceRobustness2FeaturesEXT{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
                .pNext = const_cast<void*>(newPNext),
                .nullDescriptor = VK_TRUE,
            };
            newPNext = &robustness2Storage;
        }
        if (!found.sync2) {
            sync2Storage = VkPhysicalDeviceSynchronization2Features{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
                .pNext = const_cast<void*>(newPNext),
                .synchronization2 = VK_TRUE,
            };
            newPNext = &sync2Storage;
        }
        if (!found.features12) {
            features12Storage = VkPhysicalDeviceVulkan12Features{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                .pNext = const_cast<void*>(newPNext),
                .timelineSemaphore = VK_TRUE,
                .vulkanMemoryModel = VK_TRUE,
            };
            newPNext = &features12Storage;
        }

        // 3. assemble create info and forward (queue handling deferred to post-hook,
        //    which uses findQueue with VK_QUEUE_COMPUTE_BIT against the app's queues)
        VkDeviceCreateInfo createInfo = *pCreateInfo;
        createInfo.pNext = newPNext;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        auto res = Layer::ovkCreateDevice(physicalDevice, &createInfo, pAllocator, pDevice);
        if (res == VK_ERROR_EXTENSION_NOT_PRESENT)
            throw std::runtime_error(
                "Required Vulkan device extensions (VK_EXT_robustness2/VK_KHR_synchronization2) "
                "are not present. "
                "Your GPU driver is not supported.");
        if (res == VK_ERROR_FEATURE_NOT_PRESENT)
            throw std::runtime_error(
                "Required Vulkan device features "
                "(vulkanMemoryModel/timelineSemaphore/synchronization2/nullDescriptor) "
                "are not supported by your GPU driver.");
        return res;
    }

    ///
    /// Add related device information after the device is created.
    ///
    VkResult myvkCreateDevicePost(
            VkPhysicalDevice physicalDevice,
            VkDeviceCreateInfo* pCreateInfo,
            const VkAllocationCallbacks*,
            VkDevice* pDevice) {
        // graphics queue: from the app's createInfo
        auto graphicsQ = Utils::findQueue(*pDevice, physicalDevice, pCreateInfo,
            VK_QUEUE_GRAPHICS_BIT);

        // compute queue: most Mali queue families are graphics+compute, so the app's
        // existing queues usually satisfy this — findQueue picks the first match.
        auto computeQ = Utils::findQueue(*pDevice, physicalDevice, pCreateInfo,
            VK_QUEUE_COMPUTE_BIT);

        deviceToInfo.emplace(*pDevice, DeviceInfo {
            .instance = lastCreatedInstance,
            .device = *pDevice,
            .physicalDevice = physicalDevice,
            .queue = graphicsQ,
            .computeQueue = computeQ,
        });
        return VK_SUCCESS;
    }

    /// Erase the device information when the device is destroyed.
    void myvkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) noexcept {
        for (auto it = swapchainToDeviceTable.begin(); it != swapchainToDeviceTable.end();) {
            if (it->second == device) {
                const VkSwapchainKHR swapchain = it->first;
                swapchains.erase(swapchain);
                swapchainToPresent.erase(swapchain);
                it = swapchainToDeviceTable.erase(it);
            } else {
                ++it;
            }
        }
        LSFG_3_1P::finalize();
        LSFG_3_1::finalize();
        deviceToInfo.erase(device);
        Layer::ovkDestroyDevice(device, pAllocator);
    }

    ///
    /// Adjust swapchain creation parameters and create a swapchain context.
    ///
    VkResult myvkCreateSwapchainKHR(
            VkDevice device,
            const VkSwapchainCreateInfoKHR* pCreateInfo,
            const VkAllocationCallbacks* pAllocator,
            VkSwapchainKHR* pSwapchain) noexcept {
        // find device
        auto it = deviceToInfo.find(device);
        if (it == deviceToInfo.end()) {
            Utils::logLimitN("swapMap", 5, "Device not found in map");
            return Layer::ovkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        }
        Utils::resetLimitN("swapMap");
        auto& deviceInfo = it->second;

        // increase amount of images in swapchain
        VkSwapchainCreateInfoKHR createInfo = *pCreateInfo;
        const auto maxImages = Utils::getMaxImageCount(
            deviceInfo.physicalDevice, pCreateInfo->surface);
        createInfo.minImageCount = createInfo.minImageCount + 1
            + static_cast<uint32_t>(deviceInfo.queue.first);
        if (createInfo.minImageCount > maxImages) {
            createInfo.minImageCount = maxImages;
            Utils::logLimitN("swapCount", 10,
                "Requested image count (" +
                    std::to_string(pCreateInfo->minImageCount) + ") "
                "exceeds maximum allowed (" +
                    std::to_string(maxImages) + "). "
                "Continuing with maximum allowed image count. "
                "This might lead to performance degradation.");
        } else {
            Utils::resetLimitN("swapCount");
        }

        // allow copy operations on swapchain images
        createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        // enforce present mode
        createInfo.presentMode = Config::activeConf.e_present;

        // retire potential old swapchain
        if (pCreateInfo->oldSwapchain) {
            swapchains.erase(pCreateInfo->oldSwapchain);
            swapchainToDeviceTable.erase(pCreateInfo->oldSwapchain);
        }

        // create swapchain
        auto res = Layer::ovkCreateSwapchainKHR(device, &createInfo, pAllocator, pSwapchain);
        if (res != VK_SUCCESS)
            return res; // can't be caused by lsfg-vk (yet)

        try {
            swapchainToPresent.emplace(*pSwapchain, createInfo.presentMode);

            // get all swapchain images
            uint32_t imageCount{};
            res = Layer::ovkGetSwapchainImagesKHR(device, *pSwapchain, &imageCount, nullptr);
            if (res != VK_SUCCESS || imageCount == 0)
                throw LSFG::vulkan_error(res, "Failed to get swapchain image count");

            std::vector<VkImage> swapchainImages(imageCount);
            res = Layer::ovkGetSwapchainImagesKHR(device, *pSwapchain,
                &imageCount, swapchainImages.data());
            if (res != VK_SUCCESS)
                throw LSFG::vulkan_error(res, "Failed to get swapchain images");

            // create swapchain context
            swapchainToDeviceTable.emplace(*pSwapchain, device);
            swapchains.emplace(*pSwapchain, LsContext(
                deviceInfo, *pSwapchain, pCreateInfo->imageExtent,
                swapchainImages
            ));

            std::cerr << "lsfg-vk: Swapchain context " <<
                    (createInfo.oldSwapchain ? "recreated" : "created")
                << " (using " << imageCount << " images).\n";

            Utils::resetLimitN("swapCtxCreate");
        } catch (const std::exception& e) {
            Utils::logLimitN("swapCtxCreate", 5,
                "An error occurred while creating the swapchain wrapper:\n"
                "- " + std::string(e.what()));
            return VK_SUCCESS; // swapchain is still valid
        }
        return VK_SUCCESS;
    }

    ///
    /// Update presentation parameters and present the next frame(s).
    ///
    VkResult myvkQueuePresentKHR(
            VkQueue queue,
            const VkPresentInfoKHR* pPresentInfo) noexcept {
        // find swapchain device
        auto it = swapchainToDeviceTable.find(*pPresentInfo->pSwapchains);
        if (it == swapchainToDeviceTable.end()) {
            Utils::logLimitN("swapMap", 5,
                "Swapchain not found in map");
            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);
        }

        // find device info
        auto it2 = deviceToInfo.find(it->second);
        if (it2 == deviceToInfo.end()) {
            Utils::logLimitN("swapMap", 5,
                "Device not found in map");
            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);
        }
        auto& deviceInfo = it2->second;

        // find swapchain context
        auto it3 = swapchains.find(*pPresentInfo->pSwapchains);
        if (it3 == swapchains.end()) {
            Utils::logLimitN("swapMap", 5,
                "Swapchain context not found in map");
            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);
        }
        auto& swapchain = it3->second;

        // find present mode
        auto it4 = swapchainToPresent.find(*pPresentInfo->pSwapchains);
        if (it4 == swapchainToPresent.end()) {
            Utils::logLimitN("swapMap", 5,
                "Swapchain present mode not found in map");
            return Layer::ovkQueuePresentKHR(queue, pPresentInfo);
        }
        auto& present = it4->second;

        // enforce present mode | NOLINTBEGIN
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
        const VkSwapchainPresentModeInfoEXT* presentModeInfo =
            reinterpret_cast<const VkSwapchainPresentModeInfoEXT*>(pPresentInfo->pNext);
        while (presentModeInfo) {
            if (presentModeInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT) {
                for (size_t i = 0; i < presentModeInfo->swapchainCount; i++)
                    const_cast<VkPresentModeKHR*>(presentModeInfo->pPresentModes)[i] =
                        present;
            }
            presentModeInfo =
                reinterpret_cast<const VkSwapchainPresentModeInfoEXT*>(presentModeInfo->pNext);
        }
        #pragma clang diagnostic pop

        // NOLINTEND | present the next frame
        VkResult res{}; // might return VK_SUBOPTIMAL_KHR
        try {
            // ensure config is valid
            auto& conf = Config::activeConf;
            if (!conf.config_file.empty()
                    && (
                            !std::filesystem::exists(conf.config_file)
                          || conf.timestamp != std::filesystem::last_write_time(conf.config_file)
                    )) {
                Layer::ovkQueuePresentKHR(queue, pPresentInfo);
                return VK_ERROR_OUT_OF_DATE_KHR;
            }

            // ensure present mode is still valid
            if (present != conf.e_present) {
                Layer::ovkQueuePresentKHR(queue, pPresentInfo);
                return VK_ERROR_OUT_OF_DATE_KHR;
            }

            // skip if disabled
            if (conf.multiplier <= 1)
                return Layer::ovkQueuePresentKHR(queue, pPresentInfo);

            // present the swapchain
            std::vector<VkSemaphore> semaphores(pPresentInfo->waitSemaphoreCount);
            std::copy_n(pPresentInfo->pWaitSemaphores, semaphores.size(), semaphores.data());

            res = swapchain.present(deviceInfo, pPresentInfo->pNext,
                queue, semaphores, *pPresentInfo->pImageIndices);

            Utils::resetLimitN("swapPresent");
        } catch (const std::exception& e) {
            Utils::logLimitN("swapPresent", 5,
                "An error occurred while presenting the swapchain:\n"
                "- " + std::string(e.what()));
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return res;
    }

    /// Erase the swapchain context and mapping when the swapchain is destroyed.
    void myvkDestroySwapchainKHR(
            VkDevice device,
            VkSwapchainKHR swapchain,
            const VkAllocationCallbacks* pAllocator) noexcept {
        swapchains.erase(swapchain);
        swapchainToDeviceTable.erase(swapchain);
        swapchainToPresent.erase(swapchain);
        Layer::ovkDestroySwapchainKHR(device, swapchain, pAllocator);
    }
}

std::unordered_map<std::string, PFN_vkVoidFunction> Hooks::hooks = {
    // instance hooks
    {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(myvkCreateInstance)},

    // device hooks
    {"vkCreateDevicePre", reinterpret_cast<PFN_vkVoidFunction>(myvkCreateDevicePre)},
    {"vkCreateDevicePost", reinterpret_cast<PFN_vkVoidFunction>(myvkCreateDevicePost)},
    {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(myvkDestroyDevice)},

    // swapchain hooks
    {"vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(myvkCreateSwapchainKHR)},
    {"vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(myvkQueuePresentKHR)},
    {"vkDestroySwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(myvkDestroySwapchainKHR)}
};
