#pragma once

#include "core/image.hpp"
#include "core/semaphore.hpp"
#include "core/fence.hpp"
#include "core/commandbuffer.hpp"
#include "shaders/alpha.hpp"
#include "shaders/beta.hpp"
#include "shaders/delta.hpp"
#include "shaders/gamma.hpp"
#include "shaders/generate.hpp"
#include "shaders/mipmaps.hpp"
#include "common/utils.hpp"

#include <vulkan/vulkan_core.h>

#include "lsfg_3_1.hpp"

#include <vector>
#include <cstdint>
#include <array>

namespace LSFG_3_1 {

    using namespace LSFG;

    class Context {
    public:
        ///
        /// Create a context
        ///
        /// @param vk The Vulkan instance to use.
        /// @param in0 File descriptor for the first input image.
        /// @param in1 File descriptor for the second input image.
        /// @param outN File descriptors for the output images.
        /// @param extent The size of the images.
        /// @param format The format of the images.
        ///
        /// @throws LSFG::vulkan_error if the context fails to initialize.
        ///
        Context(Vulkan& vk,
#ifdef LSFGVK_USE_DMA_HEAP
            const ExternalImage& in0, const ExternalImage& in1,
            const std::vector<ExternalImage>& outN,
#else
            int in0, int in1, const std::vector<int>& outN,
#endif
            VkExtent2D extent, VkFormat format);

        ///
        /// Present on the context.
        ///
        /// @param inSem Semaphore to wait on before starting the generation.
        /// @param semaphoreHandleType External semaphore handle type used for FD exchange.
        ///
        /// @throws LSFG::vulkan_error if the context fails to present.
        ///
        [[nodiscard]] std::vector<int> present(
            Vulkan& vk,
            int inSem,
            VkExternalSemaphoreHandleTypeFlagBits semaphoreHandleType);

        // Trivially copyable, moveable and destructible
        Context(const Context&) = default;
        Context& operator=(const Context&) = default;
        Context(Context&&) = default;
        Context& operator=(Context&&) = default;
        ~Context() = default;
    private:
        Core::Image inImg_0, inImg_1; // inImg_0 is next when fc % 2 == 0
        uint64_t frameIdx{0};

        struct RenderData {
            Core::Semaphore inSemaphore; // signaled when input is ready
            std::vector<Core::Semaphore> internalSemaphores; // signaled when first step is done
            std::vector<Core::Semaphore> outSemaphores; // signaled when each pass is done
            std::vector<Core::Fence> completionFences; // fence for completion of each pass

            Core::CommandBuffer cmdBuffer1;
            std::vector<Core::CommandBuffer> cmdBuffers2; // command buffers for second step

            bool shouldWait{false};
        };
        std::array<RenderData, 8> data;

        Shaders::Mipmaps mipmaps;
        std::array<Shaders::Alpha, 7> alpha;
        Shaders::Beta beta;
        std::array<Shaders::Gamma, 7> gamma;
        std::array<Shaders::Delta, 3> delta;
        Shaders::Generate generate;
    };

}
