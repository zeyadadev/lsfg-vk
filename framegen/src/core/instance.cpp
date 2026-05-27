#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/instance.hpp"
#include "common/exception.hpp"

#include <memory>

using namespace LSFG::Core;

Instance::Instance(PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance) {
    if (getInstanceProcAddr == nullptr || instance == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Null GIPA or VkInstance provided to Core::Instance");

    // Use volkInitializeCustom so volk dispatches through the caller-provided
    // GIPA instead of the loader's exported symbol. From inside a Vulkan layer
    // the loader's GIPA returns trampolines that expect loader-wrapped handles
    // — but framegen receives next-layer-wrapped handles, so the loader would
    // reject them.
    volkInitializeCustom(getInstanceProcAddr);
    // Use the "Only" variant: loading device-level entrypoints via the
    // instance GIPA causes the Vulkan loader to allocate a trampoline slot
    // per unknown extension function and exhaust its fixed-size array. volk
    // resolves device entrypoints later through volkLoadDevice anyway.
    volkLoadInstanceOnly(instance);

    // non-owning: deleter does nothing; the application owns this VkInstance
    this->instance = std::shared_ptr<VkInstance>(
        new VkInstance(instance),
        [](const VkInstance* handle) { delete handle; }
    );
}
