#pragma once

#include <cstddef>

namespace Utils {

    /// Allocate a DMA_BUF from /dev/dma_heap/system.
    ///
    /// @param size Number of bytes to allocate. The kernel rounds up to page size.
    /// @return A new O_RDWR | O_CLOEXEC file descriptor owned by the caller.
    /// @throws std::runtime_error if the heap cannot be opened or the ioctl fails.
    int dmaHeapAllocate(std::size_t size);

}
