// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "dmabuf.hpp"
#include "encode_layout.hpp"

#include <va/va_drmcommon.h>

namespace c2::va {

bool encode_pixel_format(uint32_t fourcc);
unsigned encode_planes(uint32_t fourcc); // zero for unsupported formats
EncodeLayout native_encode_layout(PictureSize size, uint32_t fourcc);

struct EncodeSurface {
    EncodeLayout layout;
    Fd fd;
    bool writable; // file access mode, not a guarantee that CPU mapping is supported
};

EncodeSurface allocate_encode_surface(int heap, EncodeLayout layout);
// A zero requested fourcc uses the descriptor's format. The caller retains its FD.
EncodeSurface import_encode_surface(PictureSize size, uint32_t requested_fourcc,
                                    const VADRMPRIMESurfaceDescriptor& desc);
// The caller owns the returned descriptor's FD.
VADRMPRIMESurfaceDescriptor export_encode_surface(const EncodeSurface& surface, bool separate);
bool direct_encode_layout(const EncodeLayout& surface, const EncodeLayout& expected);
// Copy the common visible area from the top left, without touching padding.
// The source and destination storage must not overlap.
void copy_encode_pixels(const EncodeLayout& dst, uint8_t* dst_data,
                        const EncodeLayout& src, const uint8_t* src_data);

} // namespace c2::va
