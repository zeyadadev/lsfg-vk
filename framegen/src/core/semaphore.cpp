#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/semaphore.hpp"
#include "core/device.hpp"
#include "common/exception.hpp"

#include <optional>
#include <cstdint>
#include <memory>
#include <stdexcept>

using namespace LSFG::Core;

Semaphore::Semaphore(const Core::Device& device, std::optional<uint32_t> initial) {
    // create semaphore
    const VkSemaphoreTypeCreateInfo typeInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = initial.value_or(0)
    };
    const VkSemaphoreCreateInfo desc{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = initial.has_value() ? &typeInfo : nullptr,
    };
    VkSemaphore semaphoreHandle{};
    auto res = vkCreateSemaphore(device.handle(), &desc, nullptr, &semaphoreHandle);
    if (res != VK_SUCCESS || semaphoreHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to create semaphore");

    // store semaphore in shared ptr (owning)
    this->isTimeline = initial.has_value();
    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(semaphoreHandle),
        [dev = device.handle()](VkSemaphore* semaphoreHandle) {
            vkDestroySemaphore(dev, *semaphoreHandle, nullptr);
            delete semaphoreHandle;
        }
    );
}

Semaphore::Semaphore(const Core::Device& /*device*/, VkSemaphore adopted) {
    if (adopted == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Null VkSemaphore adopted into Core::Semaphore");

    this->isTimeline = false;
    // non-owning: do not destroy the semaphore; it is owned by the caller
    this->semaphore = std::shared_ptr<VkSemaphore>(
        new VkSemaphore(adopted),
        [](const VkSemaphore* handle) { delete handle; }
    );
}

void Semaphore::signal(const Core::Device& device, uint64_t value) const {
    if (!this->isTimeline)
        throw std::logic_error("Invalid timeline semaphore");

    const VkSemaphoreSignalInfo signalInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = this->handle(),
        .value = value
    };
    auto res = vkSignalSemaphore(device.handle(), &signalInfo);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Unable to signal semaphore");
}

bool Semaphore::wait(const Core::Device& device, uint64_t value, uint64_t timeout) const {
    if (!this->isTimeline)
        throw std::logic_error("Invalid timeline semaphore");

    VkSemaphore semaphore = this->handle();
    const VkSemaphoreWaitInfo waitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &semaphore,
        .pValues = &value
    };
    auto res = vkWaitSemaphores(device.handle(), &waitInfo, timeout);
    if (res != VK_SUCCESS && res != VK_TIMEOUT)
        throw LSFG::vulkan_error(res, "Unable to wait for semaphore");

    return res == VK_SUCCESS;
}
