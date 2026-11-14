// SPDX-License-Identifier: Apache-2.0
#include "frame.hpp"
#include "dmabuf.hpp"

#include <array>
#include <cerrno>
#include <linux/sync_file.h>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>

namespace c2 {
namespace {

[[noreturn]] void fail(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

// This private gralloc layout is not an Android ABI. Restrict it to the
// inspected MT6989 mapper (ELF Build ID 7ce7291c0c8a57912de5a13b19e89137).
// Never load/execute the vendor file from glibc.
void verify_mapper() {
    constexpr char path[] = "/vendor/lib64/hw/mapper.mediatek.so";
    constexpr char sha[] = "3e371a8f075ad011eb5dba4f6cb89b7225d8dc185cd11e3b415bca707ad700ef";
    Ref<GMappedFile, g_mapped_file_unref> image(g_mapped_file_new(path, FALSE, nullptr));
    if (!image) throw std::runtime_error("cannot read the MT6989 mapper to verify the output ABI");
    std::unique_ptr<char, decltype(&g_free)> actual(g_compute_checksum_for_data(G_CHECKSUM_SHA256,
        reinterpret_cast<const guchar*>(g_mapped_file_get_contents(image.get())),
        g_mapped_file_get_length(image.get())), g_free);
    if (g_strcmp0(actual.get(), sha)) throw std::runtime_error("unsupported MT6989 mapper build for frame readback");
}

void wait_fence(const Parcel::Handle& fence) {
    if (!fence) return;
    if (fence.fd_count != 1) throw std::runtime_error("unsupported Codec2 fence handle");
    pollfd event{fence.fd(0), POLLIN, 0};
    const auto deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    int status;
    do {
        const auto remaining = deadline - g_get_monotonic_time();
        if (remaining <= 0) throw std::runtime_error("Codec2 frame fence timed out");
        status = poll(&event, 1, (remaining + 999) / 1000);
    } while (status < 0 && errno == EINTR);
    if (status < 0) fail("wait for Codec2 frame fence");
    if (!status) throw std::runtime_error("Codec2 frame fence timed out");
    if (event.revents != POLLIN) throw std::runtime_error("Codec2 frame fence failed");
    sync_file_info info{};
    if (ioctl(event.fd, SYNC_IOC_FILE_INFO, &info) < 0) fail("validate Codec2 sync-file fence");
    if (info.status <= 0) throw std::runtime_error("Codec2 frame fence did not signal success");
}

struct Plane { uint32_t offset, stride, width, height; };
struct Layout { std::array<Plane, 3> planes; int fd; uint32_t bytes; };

Layout layout(const Parcel::Handle& handle) {
    constexpr uint32_t yv12 = 0x32315659, c2_gralloc_magic = 0xc2677200;
    constexpr uint32_t protected_usage = 0x4000;
    // Native handle header, three FDs, 60 mapper words, then the 11-word
    // C2HandleGralloc suffix. handle_cast(), get_plane_layouts() and
    // allocator_map() establish the magic/shape, per-plane fields, and pixel
    // mapping (FD 1, size at 0xdc). Plane 1 is Cr, plane 2 is Cb for YV12.
    if (handle.fd_count != 3 || handle.int_count != 71 ||
        handle.word(70) != c2_gralloc_magic || handle.word(18) != 0x03141592 ||
        (handle.word(63) & protected_usage)) {
        throw std::runtime_error("unsupported or protected Codec2 graphic handle");
    }
    // MT6989's internal linear YV12 is 0x20e; the adjacent word has its
    // modifier bits. Refuse AFBC, other compression, other layouts and depth.
    if (handle.word(62) != yv12 || handle.word(29) != 0x20e || handle.word(30) != 0) {
        throw std::runtime_error("Codec2 output is not supported linear MT6989 YV12");
    }
    Layout out{{}, handle.fd(1), 0};
    const auto bytes = lseek(out.fd, 0, SEEK_END); // DMA-BUF extent
    if (bytes < 0) fail("read Codec2 output DMA-BUF size");
    const auto allocated = handle.word(49);
    if (!allocated || allocated > bytes || bytes > UINT32_MAX) {
        throw std::runtime_error("invalid MT6989 allocation size");
    }
    out.bytes = static_cast<uint32_t>(bytes);
    uint64_t previous_end = 0;
    for (size_t i = 0; i < out.planes.size(); ++i) {
        const auto at = 31 + i * 6; // private_handle_t::plane_info at 0x94
        const Plane p{handle.word(at), handle.word(at + 3), handle.word(at + 4), handle.word(at + 5)};
        if (!p.width || !p.height || p.width > p.stride || p.offset < previous_end || p.offset >= allocated) {
            throw std::runtime_error("invalid MT6989 plane layout");
        }
        const auto extent = static_cast<uint64_t>(p.height - 1) * p.stride + p.width;
        if (extent > allocated - p.offset) throw std::runtime_error("MT6989 plane exceeds DMA-BUF allocation");
        previous_end = p.offset + extent;
        out.planes[i] = p;
    }
    return out;
}

void check_crop(const Plane& p, const Crop& crop, uint32_t ratio) {
    const auto x = crop.left / ratio, y = crop.top / ratio;
    if (x > p.width || crop.width / ratio > p.width - x ||
        y > p.height || crop.height / ratio > p.height - y) {
        throw std::runtime_error("Codec2 crop exceeds MT6989 plane");
    }
}

void write_plane(std::FILE* out, const I420View::Plane& p, uint32_t width, uint32_t height) {
    const auto* row = p.data;
    if (width == p.stride) {
        const auto bytes = static_cast<size_t>(width) * height;
        if (std::fwrite(row, 1, bytes, out) != bytes) fail("write I420 plane");
    } else {
        for (uint32_t i = 0; i < height; ++i) {
            if (std::fwrite(row + static_cast<size_t>(i) * p.stride, 1, width, out) != width) fail("write I420 row");
        }
    }
}

} // namespace

I420Block FrameReader::describe(const OutputBlock& frame, PictureSize expected) {
    const auto& crop = frame.crop;
    if (crop.width != expected.width || crop.height != expected.height ||
        !crop.width || !crop.height || ((crop.left | crop.top | crop.width | crop.height) & 1)) {
        throw std::runtime_error("unsupported output crop or size change: " + std::to_string(crop.width) + 'x' +
                                 std::to_string(crop.height));
    }
    if (!mapper_checked_) {
        verify_mapper();
        mapper_checked_ = true;
    }
    const auto memory = layout(frame.handle);
    for (size_t i = 0; i < memory.planes.size(); ++i) check_crop(memory.planes[i], crop, i ? 2 : 1);
    wait_fence(frame.fence);
    const auto cropped_plane = [&](size_t i, uint32_t ratio) -> I420Layout::Plane {
        const auto& p = memory.planes[i];
        const auto offset = p.offset + size_t(crop.top / ratio) * p.stride + crop.left / ratio;
        return {static_cast<uint32_t>(offset), p.stride}; // bounded by check_crop() and layout()
    };
    // Duplicating the FD does not reserve a producer-owned BufferQueue slot.
    const bool standalone = !frame.handle.word(67) && !frame.handle.word(68);
    return {I420Layout{{cropped_plane(0, 1), cropped_plane(2, 2), cropped_plane(1, 2)}, expected, memory.bytes},
            memory.fd, standalone};
}

I420View I420Layout::view(const uint8_t* base) const {
    I420View pixels{{}, size};
    for (size_t i = 0; i < planes.size(); ++i) {
        pixels.planes[i] = {base + planes[i].offset, planes[i].pitch};
    }
    return pixels;
}

void I420Layout::read(int fd, const std::function<void(const I420View&)>& consumer) const {
    CpuView mapping(fd, bytes, false);
    consumer(view(mapping.data()));
    mapping.finish();
}

void FrameReader::read(const OutputBlock& block, PictureSize expected,
                       const std::function<void(const I420View&)>& consumer) {
    const auto frame = describe(block, expected);
    frame.layout.read(frame.fd, consumer);
}

I420Buffer FrameReader::retain(const OutputBlock& frame, PictureSize expected) {
    const auto block = describe(frame, expected);
    if (!block.standalone) throw std::runtime_error("cannot retain a producer-owned Codec2 graphic block");
    return {block.layout, duplicate_fd(block.fd)};
}

void FrameWriter::write(const OutputBlock& frame) {
    reader_.read(frame, size_, [this](const I420View& view) {
        if (!file_) {
            file_.reset(std::fopen(path_, "wbx")); // never overwrite the input or an existing output
            if (!file_) fail(path_);
        }
        write_plane(file_.get(), view.planes[0], view.size.width, view.size.height);
        write_plane(file_.get(), view.planes[1], view.size.width / 2, view.size.height / 2);
        write_plane(file_.get(), view.planes[2], view.size.width / 2, view.size.height / 2);
    });
}

void FrameWriter::finish() {
    if (file_ && std::fclose(file_.release()) != 0) fail("close I420 output");
}

} // namespace c2
