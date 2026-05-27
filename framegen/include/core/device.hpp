#pragma once

#include "core/instance.hpp"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <memory>

namespace LSFG::Core {

    ///
    /// Non-owning C++ wrapper around an application-provided Vulkan device.
    ///
    /// Framegen no longer creates its own VkDevice; the application's device
    /// is adopted here so volk's dispatch can be loaded against it and queue
    /// + family info is accessible to framegen's internals.
    ///
    class Device {
    public:
        ///
        /// Wrap an application-provided device.
        ///
        /// @param instance The application instance.
        /// @param getDeviceProcAddr Caller-provided GDPA for @p device.
        /// @param physicalDevice The application physical device.
        /// @param device The application logical device.
        /// @param computeFamilyIdx Index of a compute-capable queue family on @p device.
        /// @param computeQueue Compute queue handle obtained from @p device.
        ///
        /// @throws LSFG::vulkan_error if any handle is null.
        ///
        Device(const Instance& instance,
            PFN_vkGetDeviceProcAddr getDeviceProcAddr,
            VkPhysicalDevice physicalDevice,
            VkDevice device,
            uint32_t computeFamilyIdx,
            VkQueue computeQueue);

        /// Get the Vulkan handle.
        [[nodiscard]] auto handle() const { return *this->device; }
        /// Get the physical device associated with this logical device.
        [[nodiscard]] VkPhysicalDevice getPhysicalDevice() const { return this->physicalDevice; }
        /// Get the compute queue family index.
        [[nodiscard]] uint32_t getComputeFamilyIdx() const { return this->computeFamilyIdx; }
        /// Get the compute queue.
        [[nodiscard]] VkQueue getComputeQueue() const { return this->computeQueue; }

        // Trivially copyable, moveable and destructible
        Device(const Core::Device&) noexcept = default;
        Device& operator=(const Core::Device&) noexcept = default;
        Device(Device&&) noexcept = default;
        Device& operator=(Device&&) noexcept = default;
        ~Device() = default;
    private:
        std::shared_ptr<VkDevice> device;
        VkPhysicalDevice physicalDevice{};

        uint32_t computeFamilyIdx{0};

        VkQueue computeQueue{};
    };

}
