#pragma once

#include <vulkan/vulkan_core.h>

#include <functional>
#include <cstdint>
#include <string>
#include <vector>

namespace LSFG_3_1P {

    ///
    /// Initialize the LSFG library against an application-provided Vulkan device.
    ///
    /// @param getInstanceProcAddr Caller-provided vkGetInstanceProcAddr. Must
    ///        accept @p instance and return function pointers valid for the
    ///        same dispatch view the caller uses (i.e. if the caller is a
    ///        Vulkan layer, this should be the *next layer's* GIPA — the
    ///        handles framegen receives have already been unwrapped at the
    ///        layer boundary, so the loader's GIPA would reject them).
    /// @param getDeviceProcAddr Caller-provided vkGetDeviceProcAddr. From
    ///        inside a Vulkan layer this must be the next layer's GDPA for
    ///        the same reason as @p getInstanceProcAddr.
    /// @param instance Application Vulkan instance.
    /// @param physicalDevice Application physical device.
    /// @param device Application logical device.
    /// @param computeQueueFamily Index of a compute-capable queue family in @p device.
    /// @param computeQueue The corresponding compute queue handle.
    /// @param isHdr Whether the images are in HDR format.
    /// @param flowScale Internal flow scale factor.
    /// @param generationCount Number of frames to generate.
    /// @param loader Function to load shader source code by name.
    ///
    /// @throws LSFG::vulkan_error if Vulkan objects fail to initialize.
    ///
    __attribute__((visibility("default")))
    void initialize(PFN_vkGetInstanceProcAddr getInstanceProcAddr,
        PFN_vkGetDeviceProcAddr getDeviceProcAddr,
        VkInstance instance,
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        uint32_t computeQueueFamily,
        VkQueue computeQueue,
        bool isHdr, float flowScale, uint64_t generationCount,
        const std::function<std::vector<uint8_t>(const std::string&)>& loader);

    ///
    /// Create a new LSFG context on a swapchain.
    ///
    /// Caller retains ownership of the supplied images; framegen wraps them
    /// (creates views, tracks layout) but does not destroy them. Images must
    /// have been created with VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT
    /// at minimum.
    ///
    /// @param in0 First input image handle.
    /// @param in1 Second input image handle.
    /// @param outN Output image handles. The count defines the LSFG level.
    /// @param extent The size of the images.
    /// @param format The format of the images.
    /// @return A unique identifier for the created context.
    ///
    /// @throws LSFG::vulkan_error if the context cannot be created.
    ///
    __attribute__((visibility("default")))
    int32_t createContext(
        VkImage in0, VkImage in1, const std::vector<VkImage>& outN,
        VkExtent2D extent, VkFormat format);

    ///
    /// Present a context.
    ///
    /// @param id Unique identifier of the context to present.
    /// @param inSem Semaphore to wait on before starting the generation.
    ///              VK_NULL_HANDLE skips the wait.
    /// @param outSems Semaphores to signal once each output image is ready.
    ///                May be empty to skip signalling.
    ///
    /// @throws LSFG::vulkan_error if the context cannot be presented.
    ///
    __attribute__((visibility("default")))
    void presentContext(int32_t id, VkSemaphore inSem,
        const std::vector<VkSemaphore>& outSems);

    ///
    /// Delete an LSFG context.
    ///
    /// @param id Unique identifier of the context to delete.
    ///
    __attribute__((visibility("default")))
    void deleteContext(int32_t id);

    ///
    /// Deinitialize the LSFG library.
    ///
    __attribute__((visibility("default")))
    void finalize();

}
