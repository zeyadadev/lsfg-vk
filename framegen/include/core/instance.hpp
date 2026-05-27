#pragma once

#include <vulkan/vulkan_core.h>

#include <memory>

namespace LSFG::Core {

    ///
    /// Non-owning C++ wrapper around an application-provided Vulkan instance.
    ///
    /// Framegen no longer creates its own VkInstance; the application's instance
    /// is adopted here so volk's dispatch can be loaded against it.
    ///
    class Instance {
    public:
        ///
        /// Wrap an application-provided Vulkan instance.
        ///
        /// @param getInstanceProcAddr Caller-provided GIPA. volk's dispatch
        ///        will be loaded through this; pass the next-layer's GIPA
        ///        from inside a Vulkan layer, or libvulkan.so's GIPA from a
        ///        regular app.
        /// @param instance Application instance handle.
        ///
        /// @throws LSFG::vulkan_error if instance is VK_NULL_HANDLE.
        ///
        Instance(PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance);

        /// Get the Vulkan handle.
        [[nodiscard]] auto handle() const { return this->instance ? *this->instance : VK_NULL_HANDLE; }

        /// Trivially copyable, moveable and destructible
        Instance(const Instance&) noexcept = default;
        Instance& operator=(const Instance&) noexcept = default;
        Instance(Instance&&) noexcept = default;
        Instance& operator=(Instance&&) noexcept = default;
        ~Instance() = default;
    private:
        std::shared_ptr<VkInstance> instance;
    };

}
