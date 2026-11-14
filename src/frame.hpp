// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "dmabuf.hpp"
#include "work.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>

namespace c2 {

struct I420View {
    struct Plane { const uint8_t* data; size_t stride; };
    std::array<Plane, 3> planes;
    PictureSize size;
};

// Logical Y, Cb, Cr planes, with offsets already adjusted to the crop.
struct I420Layout {
    struct Plane { uint32_t offset, pitch; };
    std::array<Plane, 3> planes;
    PictureSize size;
    uint32_t bytes; // actual DMA-BUF extent, including padding

    I420View view(const uint8_t* base) const;
    void read(int fd, const std::function<void(const I420View&)>& consumer) const;
};

struct I420Buffer {
    I420Layout layout;
    Fd fd;
};

struct I420Block {
    I420Layout layout;
    int fd; // borrowed from the onWorkDone transaction
    bool standalone; // a dup pins the pixels, not a producer-owned slot
};

class FrameReader {
public:
    // Validate the layout and wait for its fence. The returned FD is borrowed.
    I420Block describe(const OutputBlock&, PictureSize expected);
    // Plane data is borrowed and valid only during the consumer call.
    void read(const OutputBlock&, PictureSize expected, const std::function<void(const I420View&)>& consumer);
    // Retains the original pixel DMA-BUF of a native BASIC_GRAPHIC block.
    I420Buffer retain(const OutputBlock&, PictureSize expected);
private:
    bool mapper_checked_ = false;
};

class FrameWriter {
public:
    FrameWriter(const char* path, PictureSize size) : path_(path), size_(size) {}
    void write(const OutputBlock& frame);
    void finish();
private:
    const char* path_;
    PictureSize size_;
    struct Closer { void operator()(std::FILE* file) const { std::fclose(file); } };
    std::unique_ptr<std::FILE, Closer> file_;
    FrameReader reader_;
};

} // namespace c2
