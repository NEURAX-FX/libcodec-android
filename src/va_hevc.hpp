// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <va/va.h>
#include <va/va_dec_hevc.h>
#include <vector>

namespace c2::va {

struct HevcSlice {
    const uint8_t* data; // Borrowed EBSP, starting at the two-byte NAL header.
    size_t size;
    uint32_t address; // Raster-scan CTB address from VA.
};

struct HevcHeaders {
    std::vector<uint8_t> vps, sps, pps;
    bool restart; // IDR only: safe to start a fresh decoder without prior DPB state.
};

// One progressive Main 8-bit 4:2:0 picture (layer and temporal ID zero),
// with segments in tile-scan order. Accepts TRAIL, RADL, RASL, BLA, IDR and
// CRA, with no suppressed picture output. Non-IDR slices must carry an inline
// short-term RPS with no SPS RPS templates or long-term references. Tiles and
// WPP are each supported, not together. Supply resolved raster-order VA IQ
// matrices when scaling lists are enabled.
// The caller checks surface bounds and forwards the original slices with start
// codes. This checks slice header prefixes, not slice data.
// Returns Annex B parameter sets without crop, VUI, timing or extensions.
// Zero reorder depth requests decode-order output without rewriting slice POC;
// this is a HAL hint, not conforming signaling for streams that require reorder.
// Throws std::invalid_argument for invalid or unsupported inputs.
HevcHeaders hevc_headers(const VAPictureParameterBufferHEVC& picture,
                         const VAIQMatrixBufferHEVC* iq, const std::vector<HevcSlice>& slices);

} // namespace c2::va
