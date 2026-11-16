// SPDX-License-Identifier: Apache-2.0
#include "dmabuf.hpp"

#include <cerrno>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>

namespace c2 {
namespace {

int sync(int fd, uint64_t flags) {
    dma_buf_sync cmd{flags};
    int result;
    do { result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &cmd); } while (result < 0 && errno == EINTR);
    return result;
}

uint64_t sync_access(CpuAccess access) {
    switch (access) {
    case CpuAccess::read: return DMA_BUF_SYNC_READ;
    case CpuAccess::write: return DMA_BUF_SYNC_WRITE;
    case CpuAccess::read_write: return DMA_BUF_SYNC_RW;
    }
    throw std::invalid_argument("invalid DMA-BUF CPU access");
}

int protection(uint64_t access) {
    return (access & DMA_BUF_SYNC_READ ? PROT_READ : 0) |
           (access & DMA_BUF_SYNC_WRITE ? PROT_WRITE : 0);
}

[[noreturn]] void fail(const char* what, int code = errno) {
    throw std::system_error(code, std::generic_category(), what);
}

} // namespace

Fd::~Fd() { if (fd_ >= 0) close(fd_); }

Fd& Fd::operator=(Fd&& other) noexcept {
    std::swap(fd_, other.fd_);
    return *this;
}

Fd duplicate_fd(int fd) {
    const int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (copy < 0) fail("duplicate DMA-BUF");
    return Fd(copy);
}

CpuView::CpuView(int fd, size_t size, bool write)
    : CpuView(fd, size, write ? CpuAccess::write : CpuAccess::read) {}

CpuView::CpuView(int fd, size_t size, CpuAccess access)
    : fd_(fd), size_(size), access_(sync_access(access)),
      base_(mmap(nullptr, size, protection(access_), MAP_SHARED, fd, 0)) {
    if (base_ == MAP_FAILED) fail("map DMA-BUF");
    if (sync(fd_, DMA_BUF_SYNC_START | access_) < 0) {
        const int error = errno;
        munmap(base_, size_);
        fail("DMA-BUF CPU access start", error);
    }
    active_ = true;
}

CpuView::~CpuView() {
    if (active_) sync(fd_, DMA_BUF_SYNC_END | access_);
    munmap(base_, size_);
}

void CpuView::finish() {
    if (sync(fd_, DMA_BUF_SYNC_END | access_) < 0) fail("DMA-BUF CPU access end");
    active_ = false;
}

Fd open_dma_heap(const char* path) {
    const int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) fail(path);
    return Fd(fd);
}

Fd allocate_dma_buffer(int heap, size_t length, uint32_t& capacity) {
    errno = 0;
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) fail("read page size", errno ? errno : EINVAL);
    if (page > UINT32_MAX || !length || length > UINT32_MAX - static_cast<size_t>(page)) {
        throw std::invalid_argument("invalid DMA-BUF allocation size");
    }
    const size_t size = ((length + page - 1) / page) * page;
    dma_heap_allocation_data alloc{};
    alloc.len = size;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) fail("DMA_HEAP_IOCTL_ALLOC");
    capacity = size;
    return Fd(alloc.fd);
}

Fd upload_packet(int heap, size_t length, uint32_t& capacity, const std::function<void(uint8_t*)>& fill) {
    // The range in the Work specifies the original AU size, not the backing allocation.
    uint32_t size;
    auto fd = allocate_dma_buffer(heap, length, size);
    CpuView view(fd.get(), size, true);
    fill(view.data());
    view.finish();
    capacity = size;
    return fd;
}

} // namespace c2
