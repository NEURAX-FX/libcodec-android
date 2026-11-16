// SPDX-License-Identifier: Apache-2.0
#include "va_encode_pixels.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace c2::va {
namespace {

bool same_size(PictureSize a, PictureSize b) {
    return a.width == b.width && a.height == b.height;
}

void validate_size(PictureSize size) {
    if (!size.width || !size.height || ((size.width | size.height) & 1)) {
        throw std::invalid_argument("encode surfaces require nonzero, even dimensions");
    }
}

uint32_t row_bytes(uint32_t fourcc, uint32_t width, unsigned plane) {
    return plane && fourcc != VA_FOURCC_NV12 ? width / 2 : width;
}

void validate_layout(const EncodeLayout& layout) {
    validate_size(layout.size);
    const auto count = encode_planes(layout.fourcc);
    if (!count) throw std::invalid_argument("unsupported encode pixel format");
    std::array<uint64_t, 3> ends{};
    for (unsigned i = 0; i < count; ++i) {
        const auto& plane = layout.planes[i];
        const auto width = row_bytes(layout.fourcc, layout.size.width, i);
        const auto height = i ? layout.size.height / 2 : layout.size.height;
        if (plane.pitch < width || plane.offset >= layout.bytes) {
            throw std::invalid_argument("invalid encode plane offset or pitch");
        }
        // Include the gaps between rows, but only require visible bytes on the last row.
        ends[i] = uint64_t(plane.offset) + uint64_t(height - 1) * plane.pitch + width;
        if (ends[i] > layout.bytes) throw std::invalid_argument("encode plane exceeds object extent");
        for (unsigned j = 0; j < i; ++j) {
            if (plane.offset < ends[j] && layout.planes[j].offset < ends[i]) {
                throw std::invalid_argument("encode planes overlap");
            }
        }
    }
}

uint32_t composed_format(uint32_t fourcc) {
    switch (fourcc) {
    case VA_FOURCC_NV12: return DRM_FORMAT_NV12;
    case VA_FOURCC_I420: return DRM_FORMAT_YUV420;
    case VA_FOURCC_YV12: return DRM_FORMAT_YVU420;
    default: return 0;
    }
}

uint32_t separate_format(uint32_t fourcc, unsigned plane) {
    return plane && fourcc == VA_FOURCC_NV12 ? DRM_FORMAT_GR88 : DRM_FORMAT_R8;
}

uint32_t descriptor_format(const VADRMPRIMESurfaceDescriptor& desc, uint32_t requested) {
    if (desc.fourcc) {
        if (requested && requested != desc.fourcc) {
            throw std::invalid_argument("requested format does not match encode descriptor");
        }
        return desc.fourcc;
    }
    if (requested) return requested;
    if (desc.num_layers == 1) {
        for (const auto fourcc : {VA_FOURCC_NV12, VA_FOURCC_I420, VA_FOURCC_YV12}) {
            if (desc.layers[0].drm_format == composed_format(fourcc)) return fourcc;
        }
    } else if (desc.num_layers == 2 && desc.layers[1].drm_format == DRM_FORMAT_GR88) {
        return VA_FOURCC_NV12;
    }
    // Three R8 layers don't identify which chroma plane is U.
    throw std::invalid_argument("cannot infer encode descriptor format");
}

bool validate_object_fd(int fd, uint32_t bytes) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0) throw std::system_error(errno, std::generic_category(), "get encode DMA-BUF access mode");
    const int mode = flags & O_ACCMODE;
    if ((flags & O_PATH) || (mode != O_RDONLY && mode != O_RDWR)) {
        throw std::invalid_argument("encode DMA-BUF is not readable");
    }
    const auto extent = lseek(fd, 0, SEEK_END);
    if (extent < 0) throw std::system_error(errno, std::generic_category(), "read encode DMA-BUF extent");
    if (uint64_t(extent) < bytes) throw std::invalid_argument("encode object exceeds DMA-BUF extent");
    return mode == O_RDWR;
}

} // namespace

bool encode_pixel_format(uint32_t fourcc) {
    return encode_planes(fourcc) != 0;
}

unsigned encode_planes(uint32_t fourcc) {
    switch (fourcc) {
    case VA_FOURCC_NV12: return 2;
    case VA_FOURCC_I420:
    case VA_FOURCC_YV12: return 3;
    default: return 0;
    }
}

EncodeLayout native_encode_layout(PictureSize size, uint32_t fourcc) {
    validate_size(size);
    if (!encode_pixel_format(fourcc)) throw std::invalid_argument("unsupported encode pixel format");
    const uint64_t width = (uint64_t(size.width) + 15) & ~uint64_t(15);
    const uint64_t height = (uint64_t(size.height) + 7) & ~uint64_t(7);
    if (width > UINT32_MAX || height > UINT32_MAX) {
        throw std::invalid_argument("native encode dimensions overflow");
    }
    const uint64_t luma = width * height;
    if (luma > UINT32_MAX || luma + luma / 2 > UINT32_MAX) {
        throw std::invalid_argument("native encode allocation exceeds 32-bit extent");
    }

    EncodeLayout layout{size, fourcc, uint32_t(luma + luma / 2), {}};
    layout.planes[0] = {0, uint32_t(width)};
    if (fourcc == VA_FOURCC_NV12) {
        layout.planes[1] = {uint32_t(luma), uint32_t(width)};
    } else {
        layout.planes[1] = {uint32_t(luma), uint32_t(width / 2)};
        layout.planes[2] = {uint32_t(luma + luma / 4), uint32_t(width / 2)};
    }
    return layout;
}

EncodeSurface allocate_encode_surface(int heap, EncodeLayout layout) {
    validate_layout(layout);
    auto fd = allocate_dma_buffer(heap, layout.bytes, layout.bytes);
    return {layout, std::move(fd), true};
}

EncodeSurface import_encode_surface(PictureSize size, uint32_t requested_fourcc,
                                    const VADRMPRIMESurfaceDescriptor& desc) {
    if (!same_size(size, {desc.width, desc.height})) {
        throw std::invalid_argument("encode descriptor dimensions do not match surface");
    }
    if (desc.num_objects != 1 || desc.objects[0].drm_format_modifier != DRM_FORMAT_MOD_LINEAR ||
        desc.objects[0].fd < 0) {
        throw std::invalid_argument("encode descriptor requires one linear DMA-BUF object");
    }
    const auto fourcc = descriptor_format(desc, requested_fourcc);
    const auto count = encode_planes(fourcc);
    const bool separate = desc.num_layers != 1;
    if (!count || (separate && desc.num_layers != count)) {
        throw std::invalid_argument("invalid encode descriptor format or layer count");
    }
    const auto& object = desc.objects[0];
    EncodeLayout layout{size, fourcc, object.size, {}};
    for (unsigned i = 0; i < count; ++i) {
        const auto& layer = desc.layers[separate ? i : 0];
        const auto plane = separate ? 0 : i;
        if (layer.num_planes != (separate ? 1 : count) || layer.object_index[plane] != 0 ||
            layer.drm_format != (separate ? separate_format(fourcc, i) : composed_format(fourcc))) {
            throw std::invalid_argument("invalid encode descriptor layer format or plane index");
        }
        layout.planes[i] = {layer.offset[plane], layer.pitch[plane]};
    }
    validate_layout(layout);
    auto fd = duplicate_fd(object.fd);
    const bool writable = validate_object_fd(fd.get(), layout.bytes);
    return {layout, std::move(fd), writable};
}

VADRMPRIMESurfaceDescriptor export_encode_surface(const EncodeSurface& surface, bool separate) {
    const auto& layout = surface.layout;
    validate_layout(layout);
    if (surface.fd.get() < 0) throw std::invalid_argument("missing encode DMA-BUF FD");

    const auto count = encode_planes(layout.fourcc);
    VADRMPRIMESurfaceDescriptor desc{};
    desc.fourcc = layout.fourcc;
    desc.width = layout.size.width;
    desc.height = layout.size.height;
    desc.num_objects = 1;
    desc.num_layers = separate ? count : 1;
    for (unsigned i = 0; i < count; ++i) {
        auto& layer = desc.layers[separate ? i : 0];
        const auto plane = separate ? 0 : i;
        layer.num_planes = separate ? 1 : count;
        layer.drm_format = separate ? separate_format(layout.fourcc, i) : composed_format(layout.fourcc);
        layer.offset[plane] = layout.planes[i].offset;
        layer.pitch[plane] = layout.planes[i].pitch;
    }
    auto fd = duplicate_fd(surface.fd.get());
    validate_object_fd(fd.get(), layout.bytes);
    desc.objects[0] = {fd.release(), layout.bytes, DRM_FORMAT_MOD_LINEAR};
    return desc;
}

bool direct_encode_layout(const EncodeLayout& surface, const EncodeLayout& expected) {
    validate_layout(surface);
    validate_layout(expected);
    if (!same_size(surface.size, expected.size) || surface.fourcc != expected.fourcc ||
        surface.bytes < expected.bytes) return false;
    const auto count = encode_planes(expected.fourcc);
    for (unsigned i = 0; i < count; ++i) {
        if (surface.planes[i].offset != expected.planes[i].offset ||
            surface.planes[i].pitch != expected.planes[i].pitch) return false;
    }
    return true;
}

void copy_encode_pixels(const EncodeLayout& dst, uint8_t* dst_data,
                        const EncodeLayout& src, const uint8_t* src_data) {
    validate_layout(dst);
    validate_layout(src);
    if (!dst_data || !src_data) throw std::invalid_argument("missing encode pixel storage");
    const auto count = encode_planes(dst.fourcc);
    if (count != encode_planes(src.fourcc)) {
        throw std::invalid_argument("cannot copy between NV12 and planar encode formats");
    }
    const auto width = std::min(dst.size.width, src.size.width);
    const auto height = std::min(dst.size.height, src.size.height);
    for (unsigned i = 0; i < count; ++i) {
        const auto& to = dst.planes[i];
        const auto& from = src.planes[i && dst.fourcc != src.fourcc ? 3 - i : i];
        const auto cols = row_bytes(dst.fourcc, width, i);
        const auto rows = i ? height / 2 : height;
        for (uint32_t row = 0; row < rows; ++row) {
            std::memcpy(dst_data + to.offset + size_t(row) * to.pitch,
                        src_data + from.offset + size_t(row) * from.pitch, cols);
        }
    }
}

} // namespace c2::va
