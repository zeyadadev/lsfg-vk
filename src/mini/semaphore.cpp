#include "mini/semaphore.hpp"
#include "common/exception.hpp"
#include "layer.hpp"

#include <vulkan/vulkan_core.h>

#include <iostream>
#include <memory>

using namespace Mini;

namespace {
    std::shared_ptr<VkSemaphore> wrapSemaphore(VkDevice device, VkSemaphore semaphoreHandle) {
        return std::shared_ptr<VkSemaphore>(
            new VkSemaphore(semaphoreHandle),
            [dev = device](VkSemaphore* semaphore) {
                Layer::ovkDestroySemaphore(dev, *semaphore, nullptr);
            }
        );
    }

    bool isValidFd(int fd, VkExternalSemaphoreHandleTypeFlagBits handleType) {
        return fd >= 0
            || (handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT && fd == -1);
    }
}

Semaphore::Semaphore(VkDevice device) {
    // create semaphore
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VkSemaphore semaphoreHandle{};
    auto res = Layer::ovkCreateSemaphore(device, &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE) {
        std::cerr << "lsfg-vk: Mini::Semaphore plain create failed: res=" << res << "\n";
        throw LSFG::vulkan_error(res, "Unable to create semaphore");
    }

    // store semaphore in shared ptr
    this->semaphore = wrapSemaphore(device, semaphoreHandle);
}

Semaphore Semaphore::createExportable(
        VkDevice device,
        VkExternalSemaphoreHandleTypeFlagBits handleType) {
    const VkExportSemaphoreCreateInfo exportInfo{
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = static_cast<VkExternalSemaphoreHandleTypeFlags>(handleType)
    };
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &exportInfo
    };
    VkSemaphore semaphoreHandle{};
    auto res = Layer::ovkCreateSemaphore(device, &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE) {
        std::cerr << "lsfg-vk: Mini::Semaphore export create failed: res=" << res
                  << " handleType=" << handleType << "\n";
        throw LSFG::vulkan_error(res, "Unable to create semaphore");
    }

    Semaphore semaphore;
    semaphore.semaphore = wrapSemaphore(device, semaphoreHandle);
    return semaphore;
}

Semaphore Semaphore::import(
        VkDevice device,
        int fd,
        VkExternalSemaphoreHandleTypeFlagBits handleType) {
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VkSemaphore semaphoreHandle{};
    auto res = Layer::ovkCreateSemaphore(device, &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE) {
        std::cerr << "lsfg-vk: Mini::Semaphore import create failed: res=" << res
                  << " handleType=" << handleType << "\n";
        throw LSFG::vulkan_error(res, "Unable to create semaphore");
    }

    auto vkImportSemaphoreFdKHR = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        Layer::ovkGetDeviceProcAddr(device, "vkImportSemaphoreFdKHR"));
    if (!vkImportSemaphoreFdKHR) {
        Layer::ovkDestroySemaphore(device, semaphoreHandle, nullptr);
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Unable to load vkImportSemaphoreFdKHR");
    }

    const VkImportSemaphoreFdInfoKHR importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = semaphoreHandle,
        .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
        .handleType = handleType,
        .fd = fd,
    };
    res = vkImportSemaphoreFdKHR(device, &importInfo);
    if (res != VK_SUCCESS) {
        std::cerr << "lsfg-vk: Mini::Semaphore import failed: res=" << res
                  << " fd=" << fd << " handleType=" << handleType << "\n";
        Layer::ovkDestroySemaphore(device, semaphoreHandle, nullptr);
        throw LSFG::vulkan_error(res, "Unable to import semaphore from fd");
    }

    Semaphore semaphore;
    semaphore.semaphore = wrapSemaphore(device, semaphoreHandle);
    return semaphore;
}

int Semaphore::exportFd(
        VkDevice device,
        VkExternalSemaphoreHandleTypeFlagBits handleType) const {
    int fd{-1};
    const VkSemaphoreGetFdInfoKHR fdInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = *this->semaphore,
        .handleType = handleType
    };
    const auto res = Layer::ovkGetSemaphoreFdKHR(device, &fdInfo, &fd);
    if (res != VK_SUCCESS || !isValidFd(fd, handleType)) {
        std::cerr << "lsfg-vk: Mini::Semaphore export get-fd failed: res=" << res
                  << " fd=" << fd << " handleType=" << handleType << "\n";
        throw LSFG::vulkan_error(res, "Unable to export semaphore to fd");
    }
    return fd;
}
