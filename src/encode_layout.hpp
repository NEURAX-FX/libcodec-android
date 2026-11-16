// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "params.hpp"

#include <array>

namespace c2::va {

// Planes in VA pixel-format order. For NV12 only Y and interleaved UV are used.
struct EncodeLayout {
    struct Plane { uint32_t offset = 0, pitch = 0; };
    PictureSize size; // visible size, not the dimensions of the allocation
    uint32_t fourcc; // VA_FOURCC_NV12, VA_FOURCC_I420, or VA_FOURCC_YV12
    uint32_t bytes; // accessible DMA-BUF extent, including padding
    std::array<Plane, 3> planes;
};

} // namespace c2::va
