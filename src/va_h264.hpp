// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <va/va.h>
#include <vector>

namespace c2::va {

struct AvcSlice {
    const uint8_t* data; // Borrowed EBSP including the NAL header, without a start code.
    size_t size;
    uint16_t first_mb;
    uint8_t refs_l0_minus1, refs_l1_minus1;
};

struct AvcHeaders {
    std::vector<uint8_t> sps, pps;
    bool idr;
};

// One complete progressive 8-bit 4:2:0 picture, in increasing macroblock
// order. Supply resolved VA IQ lists (16 for flat scaling). This checks
// header prefixes, not the slice payload. Both outputs start with 00 00 00 01.
// decode_order_hint advertises zero reorder frames to Codec2 without changing
// slice POC; it is not a conforming VUI for streams that require reordering.
// Throws std::invalid_argument on invalid or unsupported parameters.
AvcHeaders avc_headers(VAProfile profile, const VAPictureParameterBufferH264& picture,
                       const VAIQMatrixBufferH264& iq, const std::vector<AvcSlice>& slices,
                       bool decode_order_hint);

} // namespace c2::va
