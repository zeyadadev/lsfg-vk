#pragma once

#include <vulkan/vulkan_core.h>

#include <memory>

namespace Mini {

    ///
    /// C++ wrapper class for a Vulkan semaphore.
    ///
    /// This class manages the lifetime of a Vulkan semaphore.
    ///
    class Semaphore {
    public:
        Semaphore() noexcept = default;

        ///
        /// Create the semaphore.
        ///
        /// @param device Vulkan device
        ///
        /// @throws LSFG::vulkan_error if object creation fails.
        ///
        Semaphore(VkDevice device);

        ///
        /// Create a semaphore that can later be exported to a file descriptor.
        ///
        /// @param device Vulkan device
        /// @param handleType External semaphore handle type.
        ///
        /// @throws LSFG::vulkan_error if object creation fails.
        ///
        [[nodiscard]] static Semaphore createExportable(
            VkDevice device,
            VkExternalSemaphoreHandleTypeFlagBits handleType);

        ///
        /// Import a semaphore from a file descriptor.
        ///
        /// @param device Vulkan device
        /// @param fd File descriptor to import the semaphore from.
        /// @param handleType External semaphore handle type.
        ///
        /// @throws LSFG::vulkan_error if object creation fails.
        ///
        [[nodiscard]] static Semaphore import(
            VkDevice device,
            int fd,
            VkExternalSemaphoreHandleTypeFlagBits handleType);

        ///
        /// Export the semaphore to a file descriptor.
        ///
        /// @param device Vulkan device
        /// @param handleType External semaphore handle type.
        /// @return Exported file descriptor. For `SYNC_FD`, `-1` is a valid
        ///         already-signaled sentinel.
        ///
        /// @throws LSFG::vulkan_error if object export fails.
        ///
        [[nodiscard]] int exportFd(
            VkDevice device,
            VkExternalSemaphoreHandleTypeFlagBits handleType) const;

        /// Get the Vulkan handle.
        [[nodiscard]] auto handle() const { return *this->semaphore; }

        // Trivially copyable, moveable and destructible
        Semaphore(const Semaphore&) noexcept = default;
        Semaphore& operator=(const Semaphore&) noexcept = default;
        Semaphore(Semaphore&&) noexcept = default;
        Semaphore& operator=(Semaphore&&) noexcept = default;
        ~Semaphore() = default;
    private:
        std::shared_ptr<VkSemaphore> semaphore;
    };

}
