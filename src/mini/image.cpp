#include "mini/image.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#ifdef LSFGVK_USE_DMA_HEAP
#include "utils/dmaheap.hpp"
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <iostream>
#endif

#include <vulkan/vulkan_core.h>

#include <memory>
#include <cstdint>
#include <optional>

using namespace Mini;

#ifdef LSFGVK_USE_DMA_HEAP

Image::Image(VkDevice device, VkPhysicalDevice physicalDevice,
        VkExtent2D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags, int* fd,
        uint64_t* drmModifier, VkSubresourceLayout* planeLayout)
        : extent(extent), format(format), aspectFlags(aspectFlags) {
    // create LINEAR DRM-format-modifier image, externally backed by a dma_buf
    const uint64_t modifiers[1] = { 0 }; // DRM_FORMAT_MOD_LINEAR
    const VkImageDrmFormatModifierListCreateInfoEXT modInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = 1,
        .pDrmFormatModifiers = modifiers,
    };
    const VkExternalMemoryImageCreateInfo externalInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modInfo,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    // union with importer-side usages so Mali picks a layout valid for both producer
    // (TRANSFER_SRC/DST in the layer) and consumer (STORAGE|SAMPLED in framegen)
    const VkImageUsageFlags unionUsage = usage
        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
        | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &externalInfo,
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
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = unionUsage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkImage imageHandle{};
    auto res = Layer::ovkCreateImage(device, &desc, nullptr, &imageHandle);
    if (res != VK_SUCCESS || imageHandle == VK_NULL_HANDLE) {
        std::cerr << "lsfg-vk: Mini::Image vkCreateImage failed: res=" << res
                  << " extent=" << extent.width << "x" << extent.height
                  << " format=" << format << " usage=0x" << std::hex << usage << std::dec
                  << "\n";
        throw LSFG::vulkan_error(res, "Failed to create Vulkan image (DRM modifier)");
    }

    VkPhysicalDeviceMemoryProperties memProps;
    Layer::ovkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    VkMemoryRequirements memReqs;
    Layer::ovkGetImageMemoryRequirements(device, imageHandle, &memReqs);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    std::optional<uint32_t> memType{};
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if (memReqs.memoryTypeBits & (1U << i)) {
            memType.emplace(i);
            break;
        }
    }
    if (!memType.has_value())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "Unable to find memory type for image");
#pragma clang diagnostic pop

    // allocate dma_buf from /dev/dma_heap/system
    const int rawFd = Utils::dmaHeapAllocate(memReqs.size);

    // Vulkan takes ownership of the fd we pass; we need to keep one for the caller
    const int fdForVulkan = ::dup(rawFd);
    if (fdForVulkan < 0) {
        const int err = errno;
        ::close(rawFd);
        Layer::ovkDestroyImage(device, imageHandle, nullptr);
        throw std::runtime_error(std::string("dup() failed: ") + std::strerror(err));
    }

    const VkMemoryDedicatedAllocateInfoKHR dedicatedInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
        .image = imageHandle,
    };
    const VkImportMemoryFdInfoKHR importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicatedInfo,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = fdForVulkan // closes the fd
    };
    const VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &importInfo,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memType.value()
    };
    VkDeviceMemory memoryHandle{};
    res = Layer::ovkAllocateMemory(device, &allocInfo, nullptr, &memoryHandle);
    if (res != VK_SUCCESS || memoryHandle == VK_NULL_HANDLE) {
        // Vulkan failed before taking ownership; we still own fdForVulkan and rawFd
        ::close(fdForVulkan);
        ::close(rawFd);
        Layer::ovkDestroyImage(device, imageHandle, nullptr);
        throw LSFG::vulkan_error(res, "Failed to allocate/import dma_buf memory");
    }

    res = Layer::ovkBindImageMemory(device, imageHandle, memoryHandle, 0);
    if (res != VK_SUCCESS) {
        Layer::ovkFreeMemory(device, memoryHandle, nullptr);
        Layer::ovkDestroyImage(device, imageHandle, nullptr);
        ::close(rawFd);
        throw LSFG::vulkan_error(res, "Failed to bind memory to Vulkan image");
    }

    // query plane 0 layout for the importer
    VkSubresourceLayout layout{};
    if (planeLayout != nullptr) {
        const VkImageSubresource sub{
            .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
            .mipLevel = 0,
            .arrayLayer = 0
        };
        Layer::ovkGetImageSubresourceLayout(device, imageHandle, &sub, &layout);
        *planeLayout = layout;
    }
    if (drmModifier != nullptr)
        *drmModifier = 0; // LINEAR
    std::cerr << "lsfg-vk: Mini::Image exported "
              << extent.width << "x" << extent.height << " fmt=" << format
              << " usage(in)=0x" << std::hex << usage
              << " usage(eff)=0x" << unionUsage << std::dec
              << " memSize=" << memReqs.size
              << " layout(off=" << layout.offset << " size=" << layout.size
              << " rowPitch=" << layout.rowPitch << ")\n";

    *fd = rawFd;

    this->image = std::shared_ptr<VkImage>(
        new VkImage(imageHandle),
        [dev = device](VkImage* img) {
            Layer::ovkDestroyImage(dev, *img, nullptr);
        }
    );
    this->memory = std::shared_ptr<VkDeviceMemory>(
        new VkDeviceMemory(memoryHandle),
        [dev = device](VkDeviceMemory* mem) {
            Layer::ovkFreeMemory(dev, *mem, nullptr);
        }
    );
}

#else // !LSFGVK_USE_DMA_HEAP

Image::Image(VkDevice device, VkPhysicalDevice physicalDevice,
        VkExtent2D extent, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags, int* fd,
        uint64_t* /*drmModifier*/, VkSubresourceLayout* /*planeLayout*/)
        : extent(extent), format(format), aspectFlags(aspectFlags) {
    // create image
    const VkExternalMemoryImageCreateInfo externalInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR
    };
    const VkImageCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &externalInfo,
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
    auto res = Layer::ovkCreateImage(device, &desc, nullptr, &imageHandle);
    if (res != VK_SUCCESS || imageHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create Vulkan image");

    // find memory type
    VkPhysicalDeviceMemoryProperties memProps;
    Layer::ovkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    VkMemoryRequirements memReqs;
    Layer::ovkGetImageMemoryRequirements(device, imageHandle, &memReqs);

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
    const VkMemoryDedicatedAllocateInfoKHR dedicatedInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
        .image = imageHandle,
    };
    const VkExportMemoryAllocateInfo exportInfo{
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicatedInfo,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR
    };
    const VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &exportInfo,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memType.value()
    };
    VkDeviceMemory memoryHandle{};
    res = Layer::ovkAllocateMemory(device, &allocInfo, nullptr, &memoryHandle);
    if (res != VK_SUCCESS || memoryHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to allocate memory for Vulkan image");

    res = Layer::ovkBindImageMemory(device, imageHandle, memoryHandle, 0);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to bind memory to Vulkan image");

    // obtain the sharing fd
    const VkMemoryGetFdInfoKHR fdInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = memoryHandle,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
    };
    res = Layer::ovkGetMemoryFdKHR(device, &fdInfo, fd);
    if (res != VK_SUCCESS || *fd < 0)
        throw LSFG::vulkan_error(res, "Failed to obtain sharing fd for Vulkan image");

    // store objects in shared ptr
    this->image = std::shared_ptr<VkImage>(
        new VkImage(imageHandle),
        [dev = device](VkImage* img) {
            Layer::ovkDestroyImage(dev, *img, nullptr);
        }
    );
    this->memory = std::shared_ptr<VkDeviceMemory>(
        new VkDeviceMemory(memoryHandle),
        [dev = device](VkDeviceMemory* mem) {
            Layer::ovkFreeMemory(dev, *mem, nullptr);
        }
    );
}

#endif // LSFGVK_USE_DMA_HEAP
