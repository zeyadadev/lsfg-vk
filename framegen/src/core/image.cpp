#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/image.hpp"
#include "core/device.hpp"
#include "common/exception.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>

using namespace LSFG::Core;

Image::Image(const Core::Device& device, VkExtent2D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags)
        : extent(extent), format(format), aspectFlags(aspectFlags) {
    // create image
    const VkImageCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {
            .width = extent.width,
            .height = extent.height,
            .depth = 1
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkImage imageHandle{};
    auto res = vkCreateImage(device.handle(), &desc, nullptr, &imageHandle);
    if (res != VK_SUCCESS || imageHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create Vulkan image");

    // find memory type
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(), &memProps);

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device.handle(), imageHandle, &memReqs);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    std::optional<uint32_t> memType{};
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1 << i)) && // NOLINTBEGIN
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memType.emplace(i);
            break;
        } // NOLINTEND
    }
    if (!memType.has_value())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "Unable to find memory type for image");
#pragma clang diagnostic pop

    // allocate and bind memory
    const VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memType.value()
    };
    VkDeviceMemory memoryHandle{};
    res = vkAllocateMemory(device.handle(), &allocInfo, nullptr, &memoryHandle);
    if (res != VK_SUCCESS || memoryHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to allocate memory for Vulkan image");

    res = vkBindImageMemory(device.handle(), imageHandle, memoryHandle, 0);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to bind memory to Vulkan image");

    // create image view
    const VkImageViewCreateInfo viewDesc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = imageHandle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY
        },
        .subresourceRange = {
            .aspectMask = aspectFlags,
            .levelCount = 1,
            .layerCount = 1
        }
    };

    VkImageView viewHandle{};
    res = vkCreateImageView(device.handle(), &viewDesc, nullptr, &viewHandle);
    if (res != VK_SUCCESS || viewHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create image view");

    // store objects in shared ptr
    this->layout = std::make_shared<VkImageLayout>(VK_IMAGE_LAYOUT_UNDEFINED);
    this->image = std::shared_ptr<VkImage>(
        new VkImage(imageHandle),
        [dev = device.handle()](VkImage* img) {
            vkDestroyImage(dev, *img, nullptr);
        }
    );
    this->memory = std::shared_ptr<VkDeviceMemory>(
        new VkDeviceMemory(memoryHandle),
        [dev = device.handle()](VkDeviceMemory* mem) {
            vkFreeMemory(dev, *mem, nullptr);
        }
    );
    this->view = std::shared_ptr<VkImageView>(
        new VkImageView(viewHandle),
        [dev = device.handle()](VkImageView* imgView) {
            vkDestroyImageView(dev, *imgView, nullptr);
        }
    );
}

// shared memory constructor

Image::Image(const Core::Device& device, VkExtent2D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags, int fd
#ifdef LSFGVK_USE_DMA_HEAP
        , uint64_t drmModifier, VkSubresourceLayout planeLayout
#endif
        )
        : extent(extent), format(format), aspectFlags(aspectFlags) {
    // create image
#ifdef LSFGVK_USE_DMA_HEAP
    VkSubresourceLayout sanitizedLayout = planeLayout;
    sanitizedLayout.size = 0;
    sanitizedLayout.arrayPitch = 0;
    sanitizedLayout.depthPitch = 0;
    const VkSubresourceLayout planeLayouts[1] = { sanitizedLayout };
    const VkImageDrmFormatModifierExplicitCreateInfoEXT modInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = drmModifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = planeLayouts,
    };
    const VkExternalMemoryImageCreateInfo externalInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modInfo,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
#else
    const VkExternalMemoryImageCreateInfo externalInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR
    };
#endif
#ifdef LSFGVK_USE_DMA_HEAP
    const VkImageUsageFlags effectiveUsage = fd == -1 ? usage : (usage
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
        | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
#else
    const VkImageUsageFlags effectiveUsage = usage;
#endif
    const VkImageCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = fd == -1 ? nullptr : &externalInfo,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {
            .width = extent.width,
            .height = extent.height,
            .depth = 1
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
#ifdef LSFGVK_USE_DMA_HEAP
        .tiling = fd == -1 ? VK_IMAGE_TILING_OPTIMAL : VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
#endif
        .usage = effectiveUsage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkImage imageHandle{};
    auto res = vkCreateImage(device.handle(), &desc, nullptr, &imageHandle);
    if (res != VK_SUCCESS || imageHandle == VK_NULL_HANDLE) {
#ifdef LSFGVK_USE_DMA_HEAP
        std::fprintf(stderr,
            "lsfg-vk: framegen Core::Image vkCreateImage (import) failed: res=%d "
            "fd=%d mod=0x%llx usage(in)=0x%x usage(eff)=0x%x fmt=%d %ux%u layout(offset=%llu size=%llu rowPitch=%llu)\n",
            res, fd, (unsigned long long)drmModifier, usage, effectiveUsage, format,
            extent.width, extent.height,
            (unsigned long long)planeLayout.offset, (unsigned long long)planeLayout.size,
            (unsigned long long)planeLayout.rowPitch);
        if (fd != -1) {
            const uint64_t probeModifier = drmModifier;
            const VkImageDrmFormatModifierListCreateInfoEXT probeModList{
                .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
                .drmFormatModifierCount = 1,
                .pDrmFormatModifiers = &probeModifier,
            };
            const VkImageCreateInfo probeDesc{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = &probeModList,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = format,
                .extent = { extent.width, extent.height, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                .usage = effectiveUsage,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE
            };
            VkImage probeImg{};
            const VkResult probeRes = vkCreateImage(device.handle(), &probeDesc, nullptr, &probeImg);
            if (probeRes == VK_SUCCESS) {
                VkSubresourceLayout probeLayout{};
                const VkImageSubresource probeSub{
                    .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
                    .mipLevel = 0,
                    .arrayLayer = 0
                };
                vkGetImageSubresourceLayout(device.handle(), probeImg, &probeSub, &probeLayout);
                std::fprintf(stderr,
                    "lsfg-vk: probe (LIST) on importer device: usage=0x%x mod=0x%llx layout(offset=%llu size=%llu rowPitch=%llu)\n",
                    effectiveUsage, (unsigned long long)probeModifier,
                    (unsigned long long)probeLayout.offset,
                    (unsigned long long)probeLayout.size,
                    (unsigned long long)probeLayout.rowPitch);
                vkDestroyImage(device.handle(), probeImg, nullptr);
            } else {
                std::fprintf(stderr,
                    "lsfg-vk: probe (LIST) on importer device FAILED: res=%d (combination unsupported)\n",
                    probeRes);
            }
        }
#endif
        throw LSFG::vulkan_error(res, "Failed to create Vulkan image");
    }

    // find memory type
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(), &memProps);

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device.handle(), imageHandle, &memReqs);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    std::optional<uint32_t> memType{};
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
#ifdef LSFGVK_USE_DMA_HEAP
        if (memReqs.memoryTypeBits & (1U << i)) { // NOLINT
            memType.emplace(i);
            break;
        }
#else
        if ((memReqs.memoryTypeBits & (1 << i)) && // NOLINTBEGIN
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memType.emplace(i);
            break;
        } // NOLINTEND
#endif
    }
    if (!memType.has_value())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "Unable to find memory type for image");
#pragma clang diagnostic pop

    // ~~allocate~~ and bind memory
    const VkMemoryDedicatedAllocateInfoKHR dedicatedInfo2{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
        .image = imageHandle,
    };
    const VkImportMemoryFdInfoKHR importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicatedInfo2,
#ifdef LSFGVK_USE_DMA_HEAP
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
#else
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
#endif
        .fd = fd // closes the fd
    };
    const VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = fd == -1 ? nullptr : &importInfo,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memType.value()
    };
    VkDeviceMemory memoryHandle{};
    res = vkAllocateMemory(device.handle(), &allocInfo, nullptr, &memoryHandle);
    if (res != VK_SUCCESS || memoryHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to allocate memory for Vulkan image");

    res = vkBindImageMemory(device.handle(), imageHandle, memoryHandle, 0);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to bind memory to Vulkan image");

    // create image view
    const VkImageViewCreateInfo viewDesc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = imageHandle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY
        },
        .subresourceRange = {
            .aspectMask = aspectFlags,
            .levelCount = 1,
            .layerCount = 1
        }
    };

    VkImageView viewHandle{};
    res = vkCreateImageView(device.handle(), &viewDesc, nullptr, &viewHandle);
    if (res != VK_SUCCESS || viewHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create image view");

    // store objects in shared ptr
    this->layout = std::make_shared<VkImageLayout>(VK_IMAGE_LAYOUT_UNDEFINED);
    this->image = std::shared_ptr<VkImage>(
        new VkImage(imageHandle),
        [dev = device.handle()](VkImage* img) {
            vkDestroyImage(dev, *img, nullptr);
        }
    );
    this->memory = std::shared_ptr<VkDeviceMemory>(
        new VkDeviceMemory(memoryHandle),
        [dev = device.handle()](VkDeviceMemory* mem) {
            vkFreeMemory(dev, *mem, nullptr);
        }
    );
    this->view = std::shared_ptr<VkImageView>(
        new VkImageView(viewHandle),
        [dev = device.handle()](VkImageView* imgView) {
            vkDestroyImageView(dev, *imgView, nullptr);
        }
    );
}
