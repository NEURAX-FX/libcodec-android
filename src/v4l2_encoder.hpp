// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "encode_layout.hpp"

#include <va/va.h>

#include <cstddef>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace c2::va {

// VA rate-control mask for each profile on accessible MediaTek encoders.
using EncodeRates = std::array<unsigned, 32>;
EncodeRates v4l2_encode_rates() noexcept;

// Wait for an implicit DMA-BUF writer before the CPU reads a staging source.
void wait_encode_input(int fd);

struct EncodeSettings {
    PictureSize size;
    uint32_t bitrate, fps_num, fps_den, gop;
    uint32_t level; // H.264 level_idc or HEVC general_level_idc; zero keeps driver default
    unsigned rate_mode;
    bool high_tier;
};

class V4l2Encoder {
public:
    V4l2Encoder(VAProfile, const EncodeSettings&, uint32_t fourcc, size_t coded_capacity);
    ~V4l2Encoder();
    V4l2Encoder(const V4l2Encoder&) = delete;
    V4l2Encoder& operator=(const V4l2Encoder&) = delete;

    const EncodeLayout& layout() const;
    void set_bitrate(uint32_t bitrate);
    // The caller retains the DMA-BUF and must not modify it during this call.
    void encode(int fd, uint32_t extent, bool keyframe, int qp, size_t limit, std::vector<uint8_t>& coded);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace c2::va
