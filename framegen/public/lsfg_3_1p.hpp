#pragma once

#include <vulkan/vulkan_core.h>

#include <functional>
#include <cstdint>
#include <string>
#include <vector>

namespace LSFG_3_1P {

#ifdef LSFGVK_USE_DMA_HEAP
    ///
    /// External (dma_buf-backed) image descriptor passed across the LSFG boundary.
    ///
    struct ExternalImage {
        int fd;                          ///< dma_buf file descriptor (importer takes ownership via dup)
        uint64_t drmModifier;            ///< DRM format modifier (0 = LINEAR)
        VkSubresourceLayout planeLayout; ///< plane 0 layout queried on the exporter side
    };
#endif

    ///
    /// Initialize the LSFG library.
    ///
    /// @param deviceUUID The UUID of the Vulkan device to use.
    /// @param isHdr Whether the images are in HDR format.
    /// @param flowScale Internal flow scale factor.
    /// @param generationCount Number of frames to generate.
    /// @param loader Function to load shader source code by name.
    ///
    /// @throws LSFG::vulkan_error if Vulkan objects fail to initialize.
    ///
    __attribute__((visibility("default")))
    void initialize(uint64_t deviceUUID,
        bool isHdr, float flowScale, uint64_t generationCount,
        const std::function<std::vector<uint8_t>(const std::string&)>& loader);

    ///
    /// Create a new LSFG context on a swapchain.
    ///
    /// @param in0 File descriptor for the first input image.
    /// @param in1 File descriptor for the second input image.
    /// @param outN File descriptor for each output image. This defines the LSFG level.
    /// @param extent The size of the images
    /// @param format The format of the images.
    /// @return A unique identifier for the created context.
    ///
    /// @throws LSFG::vulkan_error if the context cannot be created.
    ///
    __attribute__((visibility("default")))
    int32_t createContext(
#ifdef LSFGVK_USE_DMA_HEAP
        const ExternalImage& in0, const ExternalImage& in1,
        const std::vector<ExternalImage>& outN,
#else
        int in0, int in1, const std::vector<int>& outN,
#endif
        VkExtent2D extent, VkFormat format);

    ///
    /// Present a context.
    ///
    /// @param id Unique identifier of the context to present.
    /// @param inSem Semaphore to wait on before starting the generation.
    /// @param semaphoreHandleType External semaphore handle type used for FD exchange.
    /// @return Semaphores to wait on once each output image is ready.
    ///
    /// @throws LSFG::vulkan_error if the context cannot be presented.
    ///
    __attribute__((visibility("default")))
    std::vector<int> presentContext(
        int32_t id,
        int inSem,
        VkExternalSemaphoreHandleTypeFlagBits semaphoreHandleType);

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
