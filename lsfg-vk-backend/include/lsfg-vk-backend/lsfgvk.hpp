/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace vk {
    class Vulkan;
}

namespace lsfgvk::backend {

    class [[gnu::visibility("default")]] ContextImpl;
    class [[gnu::visibility("default")]] InstanceImpl;

    using Context = ContextImpl;

    ///
    /// Primitive exception class that deliveres a detailed error message
    ///
    class [[gnu::visibility("default")]] error : public std::runtime_error {
    public:
        ///
        /// Construct an error
        ///
        /// @param msg Error message.
        /// @param inner Inner exception.
        ///
        explicit error(const std::string &msg, const std::exception &inner);

        ///
        /// Construct an error
        ///
        /// @param msg Error message.
        ///
        explicit error(const std::string &msg);

        error(const error &) = default;
        error &operator=(const error &) = default;
        error(error &&) = default;
        error &operator=(error &&) = default;
        ~error() override;
    };

    /// Function type for picking a device based on its name and IDs
    using DevicePicker = std::function<bool(
        const std::string& deviceName,
        std::pair<const std::string&, const std::string&> ids, // (vendor ID, device ID) 0xXXXX format
        const std::optional<std::string>& pci // (bus:slot.func) if available, no padded zeros
    )>;

    ///
    /// Main entry point of the library
    ///
    class [[gnu::visibility("default")]] Instance {
    public:
        ///
        /// Create a lsfg-vk instance that owns its own Vulkan device.
        ///
        /// @param devicePicker Function that picks a physical device based on some identifiers.
        /// @param shaderDllPath Path to the Lossless.dll file to load shaders from.
        /// @param allowLowPrecision Whether to load low-precision (FP16) shaders if supported.
        ///
        /// @throws backend::error on failure
        ///
        Instance(
            const DevicePicker& devicePicker,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision
        );

        ///
        /// Create a lsfg-vk instance that adopts the caller's Vulkan device.
        ///
        /// The backend won't create its own VkInstance/VkDevice — bridge images
        /// and the sync semaphore are shared as Vulkan handles instead of file
        /// descriptors. This is the only viable path on drivers that don't
        /// support `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT` (e.g. Mali).
        ///
        /// @param sharedVulkan Reference to the caller's vk::Vulkan; must
        ///        outlive this Instance.
        /// @param shaderDllPath Path to the Lossless.dll file to load shaders from.
        /// @param allowLowPrecision Whether to load low-precision (FP16) shaders if supported.
        ///
        /// @throws backend::error on failure
        ///
        Instance(
            const vk::Vulkan& sharedVulkan,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision
        );

        ///
        /// Open a frame generation context (fd-based, dual-device).
        ///
        /// @param sourceFds Pair of file descriptors for the source images alternated between.
        /// @param destFds Vector with file descriptors to import output images from.
        /// @param syncFd File descriptor for the timeline semaphore used for synchronization.
        /// @param width Width of the images.
        /// @param height Height of the images.
        /// @param hdr Whether the images are HDR.
        /// @param flow Motion flow factor.
        /// @param perf Whether to enable performance mode.
        ///
        /// @throws backend::error on failure
        ///
        Context& openContext(
            std::pair<int, int> sourceFds,
            const std::vector<int>& destFds,
            int syncFd,
            uint32_t width, uint32_t height,
            bool hdr, float flow, bool perf
        );

        ///
        /// Open a frame generation context (handle-based, single-device).
        ///
        /// All VkImage / VkSemaphore handles are adopted — the caller retains
        /// ownership and must keep them alive while the context is open.
        ///
        /// The same timeline-semaphore protocol applies as the fd-based path:
        /// - Application signals N -> backend wakes and starts generating
        /// - Backend signals N+1..N+M -> generated frame M is ready
        ///
        /// @param sourceImages Pair of VkImages alternated between as source.
        /// @param destImages Vector of VkImages to write generated frames into.
        /// @param syncSemaphore Timeline VkSemaphore shared with the caller.
        /// @param width Width of the images.
        /// @param height Height of the images.
        /// @param hdr Whether the images are HDR.
        /// @param flow Motion flow factor.
        /// @param perf Whether to enable performance mode.
        ///
        /// @throws backend::error on failure
        ///
        Context& openContext(
            std::pair<VkImage, VkImage> sourceImages,
            const std::vector<VkImage>& destImages,
            VkSemaphore syncSemaphore,
            uint32_t width, uint32_t height,
            bool hdr, float flow, bool perf
        );

        ///
        /// Schedule a new set of generated frames.
        ///
        /// @param context Context to use.
        /// @throws backend::error on failure
        ///
        void scheduleFrames(Context& context);

        ///
        /// Close a frame generation context
        ///
        /// @param context Context to close.
        ///
        void closeContext(const Context& context);

        // Non-copyable and non-movable
        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;
        Instance(Instance&&) = delete;
        Instance& operator=(Instance&&) = delete;
        virtual ~Instance();
    private:
        std::unique_ptr<InstanceImpl> m_impl;

        std::vector<std::unique_ptr<Context>> m_contexts;
    };

    ///
    /// Make all lsfg-vk instances leaking.
    /// This is to workaround a bug in the Vulkan loader, which
    /// makes it impossible to destroy Vulkan instances and devices.
    ///
    void makeLeaking();

}
