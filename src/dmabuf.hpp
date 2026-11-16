// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace c2 {

class Fd {
public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}
    ~Fd();
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept;
    int release() noexcept { return std::exchange(fd_, -1); }
    int get() const { return fd_; }
private:
    int fd_;
};

enum class CpuAccess { read, write, read_write };

class CpuView {
public:
    CpuView(int fd, size_t size, bool write);
    CpuView(int fd, size_t size, CpuAccess access);
    ~CpuView();
    CpuView(const CpuView&) = delete;
    CpuView& operator=(const CpuView&) = delete;
    uint8_t* data() const { return static_cast<uint8_t*>(base_); }
    void finish();
private:
    int fd_;
    size_t size_;
    uint64_t access_;
    void* base_;
    bool active_ = false;
};

Fd duplicate_fd(int fd);
Fd open_dma_heap(const char* path);
// Allocate without mapping or filling; return the page-aligned extent in capacity.
Fd allocate_dma_buffer(int heap, size_t length, uint32_t& capacity);
Fd upload_packet(int heap, size_t length, uint32_t& capacity, const std::function<void(uint8_t*)>& fill);

} // namespace c2
