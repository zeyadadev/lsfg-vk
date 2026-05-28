#include "utils/dmaheap.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/dma-heap.h>

#include <stdexcept>
#include <string>
#include <cstring>
#include <cerrno>
#include <atomic>

namespace {

    /// Open /dev/dma_heap/system once and cache the fd for the process lifetime.
    int getHeapFd() {
        static std::atomic<int> cached{-1};
        int fd = cached.load(std::memory_order_acquire);
        if (fd >= 0) return fd;

        const int opened = ::open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC); // NOLINT
        if (opened < 0)
            throw std::runtime_error(std::string("Unable to open /dev/dma_heap/system: ")
                + std::strerror(errno));

        int expected = -1;
        if (!cached.compare_exchange_strong(expected, opened,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            // Another thread won the race.
            ::close(opened);
            return expected;
        }
        return opened;
    }

}

int Utils::dmaHeapAllocate(std::size_t size) {
    const int heap = getHeapFd();

    dma_heap_allocation_data data{};
    data.len = size;
    data.fd_flags = O_RDWR | O_CLOEXEC; // NOLINT

    if (::ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data) < 0) // NOLINT
        throw std::runtime_error(std::string("DMA_HEAP_IOCTL_ALLOC failed: ")
            + std::strerror(errno));

    return static_cast<int>(data.fd);
}
