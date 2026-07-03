/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "instance.hpp"
#include "lsfg-vk-common/helpers/paths.hpp"
#include "swapchain.hpp"
#include "lsfg-vk-common/configuration/detection.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <stdlib.h>
#include <vulkan/vulkan_core.h>

using namespace lsfgvk;
using namespace lsfgvk::layer;

namespace {
    bool same_device_mode(const ls::GlobalConf& global) {
        const char* backend = std::getenv("LSFGVK_BACKEND");
        if (backend && *backend != '\0')
            return std::string(backend) == "same-device";

        return global.backend && *global.backend == "same-device";
    }

    std::string queue_flags_string(VkQueueFlags flags) {
        std::string text;
        const auto add = [&text, flags](VkQueueFlagBits bit, const char* name) {
            if (!(flags & bit))
                return;
            if (!text.empty())
                text += '|';
            text += name;
        };

        add(VK_QUEUE_GRAPHICS_BIT, "GRAPHICS");
        add(VK_QUEUE_COMPUTE_BIT, "COMPUTE");
        add(VK_QUEUE_TRANSFER_BIT, "TRANSFER");
        add(VK_QUEUE_SPARSE_BINDING_BIT, "SPARSE");
        add(VK_QUEUE_PROTECTED_BIT, "PROTECTED");

        if (text.empty())
            text = "0";
        return text;
    }

    /// helper function to add required extensions
    std::vector<const char*> add_extensions(const char* const* existingExtensions, size_t count,
            const std::vector<const char*>& requiredExtensions) {
        std::vector<const char*> extensions(count);
        std::copy_n(existingExtensions, count, extensions.data());

        for (const auto& requiredExtension : requiredExtensions) {
            auto it = std::ranges::find_if(extensions,
                [requiredExtension](const char* extension) {
                    return std::string(extension) == std::string(requiredExtension);
                });
            if (it == extensions.end())
                extensions.push_back(requiredExtension);
        }

        return extensions;
    }

    std::optional<vk::QueueSelection> reserve_same_device_queue(
            const vk::VulkanInstanceFuncs& funcs, VkPhysicalDevice physdev,
            VkDeviceCreateInfo& createInfo,
            std::vector<VkDeviceQueueCreateInfo>& queueInfos,
            std::vector<std::vector<float>>& queuePriorities) {
        uint32_t queueFamilyCount{};
        funcs.GetPhysicalDeviceQueueFamilyProperties(physdev, &queueFamilyCount, VK_NULL_HANDLE);
        std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
        funcs.GetPhysicalDeviceQueueFamilyProperties(physdev, &queueFamilyCount, families.data());

        queueInfos.reserve(createInfo.queueCreateInfoCount);
        queuePriorities.reserve(createInfo.queueCreateInfoCount);
        std::vector<uint32_t> requestedByFamily(families.size());
        for (uint32_t i = 0; i < createInfo.queueCreateInfoCount; ++i) {
            const auto& requested = createInfo.pQueueCreateInfos[i];
            auto& priorities = queuePriorities.emplace_back();
            priorities.reserve(requested.queueCount + 1);

            for (uint32_t j = 0; j < requested.queueCount; ++j)
                priorities.push_back(requested.pQueuePriorities ?
                    requested.pQueuePriorities[j] : 1.0F);

            auto& copy = queueInfos.emplace_back(requested);
            copy.pQueuePriorities = priorities.data();

            if (requested.queueFamilyIndex < requestedByFamily.size())
                requestedByFamily.at(requested.queueFamilyIndex) += requested.queueCount;
        }

        std::cerr << "lsfg-vk: same-device queue families:\n";
        for (size_t i = 0; i < families.size(); ++i) {
            const auto& family = families.at(i);
            std::cerr << "lsfg-vk:   physical family " << i
                << ": queues=" << family.queueCount
                << ", flags=" << queue_flags_string(family.queueFlags) << '\n';
        }
        std::cerr << "lsfg-vk: same-device app queue requests:\n";
        for (const auto& requested : queueInfos) {
            const uint32_t physicalCount = requested.queueFamilyIndex < families.size() ?
                families.at(requested.queueFamilyIndex).queueCount : 0;
            const uint32_t totalRequested = requested.queueFamilyIndex < requestedByFamily.size() ?
                requestedByFamily.at(requested.queueFamilyIndex) : requested.queueCount;
            std::cerr << "lsfg-vk:   create-info family " << requested.queueFamilyIndex
                << ": queueCount=" << requested.queueCount
                << ", totalRequestedForFamily=" << totalRequested
                << ", physicalQueues=" << physicalCount << '\n';
        }

        constexpr VkQueueFlags requiredFlags =
            VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        for (size_t i = 0; i < queueInfos.size(); ++i) {
            auto& requested = queueInfos.at(i);
            if (requested.queueFamilyIndex >= families.size())
                continue;

            const auto& family = families.at(requested.queueFamilyIndex);
            if ((family.queueFlags & requiredFlags) != requiredFlags)
                continue;
            const uint32_t totalRequested = requestedByFamily.at(requested.queueFamilyIndex);
            if (totalRequested >= family.queueCount)
                continue;

            const vk::QueueSelection selection{
                .familyIndex = requested.queueFamilyIndex,
                .queueIndex = totalRequested
            };
            queuePriorities.at(i).push_back(1.0F);
            requested.queueCount++;
            requested.pQueuePriorities = queuePriorities.at(i).data();
            requestedByFamily.at(requested.queueFamilyIndex)++;

            createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
            createInfo.pQueueCreateInfos = queueInfos.data();
            return selection;
        }

        return std::nullopt;
    }
}

Root::Root() {
    // find active profile
    const auto& profile = findProfile(this->config.get(), ls::identify());
    if (!profile.has_value())
        return;

    this->active_profile = profile->second;

    std::cerr << "lsfg-vk: using profile with name '" << this->active_profile->name << "' ";
    switch (profile->first) {
        case ls::IdentType::OVERRIDE:
            std::cerr << "(identified via override)\n";
            break;
        case ls::IdentType::EXECUTABLE:
            std::cerr << "(identified via executable)\n";
            break;
        case ls::IdentType::WINE_EXECUTABLE:
            std::cerr << "(identified via wine executable)\n";
            break;
        case ls::IdentType::PROCESS_NAME:
            std::cerr << "(identified via process name)\n";
            break;
    }
}

bool Root::update() {
    if (!this->config.update())
        return false;

    const auto& profile = findProfile(this->config.get(), ls::identify());
    if (profile.has_value())
        this->active_profile = profile->second;
    else
        this->active_profile = std::nullopt;

    return true;
}

bool Root::sameDeviceMode() const {
    return same_device_mode(this->config.get().global());
}

void Root::logQueueRequest(VkDevice device, uint32_t familyIndex,
        uint32_t queueIndex, const char* source) const {
    if (!this->active_profile.has_value() || !this->sameDeviceMode())
        return;
    if (this->sameDeviceQueues.contains(device))
        return;

    std::cerr << "lsfg-vk: app fetched queue via " << source
        << ": family " << familyIndex << ", index " << queueIndex << '\n';
}

void Root::modifyInstanceCreateInfo(VkInstanceCreateInfo& createInfo,
        const std::function<void(void)>& finish) const {
    if (!this->active_profile.has_value()) {
        finish();
        return;
    }

    auto extensions = add_extensions(
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount,
        {
            "VK_KHR_get_physical_device_properties2",
            "VK_KHR_external_memory_capabilities",
            "VK_KHR_external_semaphore_capabilities"
        }
    );
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    finish();
}

std::optional<vk::QueueSelection> Root::modifyDeviceCreateInfo(
        const vk::VulkanInstanceFuncs& funcs, VkPhysicalDevice physdev,
        VkDeviceCreateInfo& createInfo,
        const std::function<void(void)>& finish) const {
    if (!this->active_profile.has_value()) {
        finish();
        return std::nullopt;
    }

    const bool sameDevice = same_device_mode(this->config.get().global());

    auto extensions = add_extensions(
        createInfo.ppEnabledExtensionNames,
        createInfo.enabledExtensionCount,
        sameDevice ?
            std::vector<const char*>{
                "VK_KHR_timeline_semaphore"
            } :
            std::vector<const char*>{
                "VK_KHR_external_memory",
                "VK_KHR_external_memory_fd",
                "VK_KHR_external_semaphore",
                "VK_KHR_external_semaphore_fd",
                "VK_KHR_timeline_semaphore"
            }
    );
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    bool isFeatureEnabled = false;
    auto* featureInfo = reinterpret_cast<VkBaseInStructure*>(const_cast<void*>(createInfo.pNext));
    while (featureInfo) {
        if (featureInfo->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* features = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(featureInfo);
            features->timelineSemaphore = VK_TRUE;
            isFeatureEnabled = true;
        } else if (featureInfo->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            auto* features = reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(featureInfo);
            features->timelineSemaphore = VK_TRUE;
            isFeatureEnabled = true;
        }

        featureInfo = const_cast<VkBaseInStructure*>(featureInfo->pNext);
    }

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .pNext = const_cast<void*>(createInfo.pNext),
        .timelineSemaphore = VK_TRUE
    };
    if (!isFeatureEnabled)
        createInfo.pNext = &timelineFeatures;

    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    std::vector<std::vector<float>> queuePriorities;
    std::optional<vk::QueueSelection> queueSelection;
    if (sameDevice) {
        queueSelection = reserve_same_device_queue(
            funcs, physdev, createInfo, queueInfos, queuePriorities);
        if (queueSelection.has_value()) {
            std::cerr << "lsfg-vk: same-device backend reserved queue family "
                << queueSelection->familyIndex << ", queue index "
                << queueSelection->queueIndex << '\n';
        } else {
            std::cerr << "lsfg-vk: same-device backend disabled for this device: "
                "no spare graphics+compute+transfer queue is available\n";
        }
    }

    finish();
    return queueSelection;
}

void Root::registerSameDeviceQueue(VkDevice device, vk::QueueSelection queue) {
    this->sameDeviceQueues.emplace(device, queue);
}

void Root::removeDevice(VkDevice device) {
    this->sameDeviceQueues.erase(device);
}

void Root::modifySwapchainCreateInfo(const vk::Vulkan& vk, VkSwapchainCreateInfoKHR& createInfo,
        const std::function<void(void)>& finish) const {
    if (!this->active_profile.has_value()) {
        finish();
        return;
    }
    if (same_device_mode(this->config.get().global()) && !this->sameDeviceQueues.contains(vk.dev())) {
        finish();
        return;
    }

    VkSurfaceCapabilitiesKHR caps{}; // NOLINT (enum value 0)
    auto res = vk.fi().GetPhysicalDeviceSurfaceCapabilitiesKHR(
        vk.physdev(), createInfo.surface, &caps);
    if (res != VK_SUCCESS)
        throw ls::vulkan_error(res, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR() failed");

    context_ModifySwapchainCreateInfo(*this->active_profile, caps.maxImageCount, createInfo);

    finish();
}

bool Root::createSwapchainContext(const vk::Vulkan& vk,
        VkSwapchainKHR swapchain, const SwapchainInfo& info) {
    if (!this->active_profile.has_value())
        return false;
    const auto& profile = *this->active_profile;
    const bool sameDevice = same_device_mode(this->config.get().global());
    auto sameDeviceQueue = this->sameDeviceQueues.find(vk.dev());
    if (sameDevice && sameDeviceQueue == this->sameDeviceQueues.end()) {
        std::cerr << "lsfg-vk: same-device backend is inactive for this swapchain\n";
        return false;
    }

    if (!this->backend.has_value()) { // emplace backend late, due to loader bug
        const auto& global = this->config.get().global();

        if (!sameDevice)
            setenv("DISABLE_LSFGVK", "1", 1);

        try {
            std::string dll{};
            if (global.dll.has_value())
                dll = *global.dll;
            else
                dll = ls::findShaderDll();

            if (sameDevice) {
                this->backend.emplace(
                    vk::Vulkan(vk.inst(), vk.dev(), vk.physdev(),
                        vk.fi(), vk.df(), false, vk.loaderdatafunc(),
                        std::nullopt, sameDeviceQueue->second),
                    dll, false
                );
            } else {
                this->backend.emplace(
                    [gpu = profile.gpu](
                        const std::string& deviceName,
                        std::pair<const std::string&, const std::string&> ids,
                        const std::optional<std::string>& pci
                    ) {
                        if (!gpu)
                            return true;

                        return (deviceName == *gpu)
                            || (ids.first + ":" + ids.second == *gpu)
                            || (pci && *pci == *gpu);
                    },
                    dll, global.allow_fp16
                );
            }
        } catch (const std::exception& e) {
            if (!sameDevice)
                unsetenv("DISABLE_LSFGVK");
            throw ls::error("failed to create backend instance", e);
        }

        if (!sameDevice)
            unsetenv("DISABLE_LSFGVK");
    }

    this->swapchains.emplace(swapchain,
        Swapchain(vk, this->backend.mut(), profile, info, sameDevice));
    return true;
}

void Root::removeSwapchainContext(VkSwapchainKHR swapchain) {
    this->swapchains.erase(swapchain);
}
