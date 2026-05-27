#include "utils/benchmark.hpp"

#include <unistd.h>

#include <iostream>
#include <cstdint>

using namespace Benchmark;

// LSFG_BENCHMARK pathway. Previously this drove framegen against a
// framegen-owned VkDevice. With the single-device refactor, framegen now
// adopts the application's VkDevice, so a standalone benchmark must
// bootstrap its own Vulkan instance/device, queue, and bridge images.
// That isn't wired up yet — re-enable when a self-contained harness is
// added.
void Benchmark::run(uint32_t width, uint32_t height) {
    (void)width; (void)height;
    std::cerr << "lsfg-vk: LSFG_BENCHMARK is not supported under the "
              << "single-device build. Run a real Vulkan app instead.\n";
    _exit(1);
}
