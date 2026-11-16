// SPDX-License-Identifier: Apache-2.0
#include "v4l2_encoder.hpp"
#include "dmabuf.hpp"
#include "va_encode_pixels.hpp"

#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <linux/videodev2.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <sys/ioctl.h>
#include <sys/mman.h>

namespace c2::va {
namespace {

constexpr auto output = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr auto capture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
constexpr uint32_t h265 = v4l2_fourcc('H', '2', '6', '5'); // MediaTek vendor ABI
// MT6989 mtk-v4l2-vcodec.h: ENCODE_RC_{MAX_QP,MIN_QP,FRAME_LEVEL_QP}.
constexpr uint32_t mtk_base = V4L2_CTRL_CLASS_CODEC | 0x2000;
constexpr uint32_t max_qp = mtk_base + 36, min_qp = mtk_base + 37, frame_qp = mtk_base + 40;
constexpr uint32_t raw_formats[] = {V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_YVU420};
constexpr size_t max_capture_plane = 128 * 1024 * 1024;
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
constexpr auto frame_timeout = std::chrono::seconds(5);

struct Profile {
    VAProfile va;
    uint32_t control, level;
    int32_t value;
    bool hevc;
};
constexpr Profile profiles[] = {
    {VAProfileH264ConstrainedBaseline, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
     V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE, false},
    {VAProfileH264Main, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
     V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_PROFILE_MAIN, false},
    {VAProfileH264High, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
     V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_PROFILE_HIGH, false},
    {VAProfileHEVCMain, V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
     V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN, true},
};
static_assert(static_cast<unsigned>(VAProfileHEVCMain) < 32 &&
              static_cast<unsigned>(VAProfileH264ConstrainedBaseline) < 32 &&
              static_cast<unsigned>(VAProfileH264Main) < 32 && static_cast<unsigned>(VAProfileH264High) < 32);

[[noreturn]] void fail(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

int xioctl(int fd, unsigned long request, void* arg, Deadline deadline = Deadline::max()) noexcept {
    do {
        if (deadline != Deadline::max() && Clock::now() >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        const int result = ioctl(fd, request, arg);
        if (result >= 0 || errno != EINTR) return result;
    } while (true);
}

void checked_ioctl(int fd, unsigned long request, void* arg, const char* what,
                   Deadline deadline = Deadline::max()) {
    if (xioctl(fd, request, arg, deadline) < 0) fail(what);
}

// The callback returns true to stop scanning. A fresh fd is used for each candidate.
template<class Visit>
void visit_devices(Visit visit) {
    const auto close_dir = [](DIR* dir) { closedir(dir); };
    std::unique_ptr<DIR, decltype(close_dir)> directory(opendir("/dev"), close_dir);
    if (!directory) return;
    while (const auto* entry = readdir(directory.get())) {
        const std::string_view name(entry->d_name);
        if (name.size() <= 5 || name.substr(0, 5) != "video" ||
            name.find_first_not_of("0123456789", 5) != name.npos) continue;
        Fd dev(openat(dirfd(directory.get()), entry->d_name, O_RDWR | O_NONBLOCK | O_CLOEXEC));
        if (dev.get() < 0) continue;
        v4l2_capability info{};
        if (xioctl(dev.get(), VIDIOC_QUERYCAP, &info) < 0) continue;
        const auto* driver = reinterpret_cast<const char*>(info.driver);
        if (std::string_view(driver, strnlen(driver, sizeof(info.driver))) != "mtk-vcodec-enc") continue;
        const uint32_t caps = (info.capabilities & V4L2_CAP_DEVICE_CAPS) ? info.device_caps : info.capabilities;
        constexpr uint32_t wanted = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
        if ((caps & wanted) == wanted && visit(std::move(dev))) return;
    }
}

bool has_format(int fd, v4l2_buf_type type, uint32_t fourcc) {
    for (uint32_t i = 0; i < 256; ++i) {
        v4l2_fmtdesc fmt{};
        fmt.index = i;
        fmt.type = type;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0) return false;
        if (fmt.pixelformat == fourcc) return true;
    }
    return false;
}

bool query_control(int fd, uint32_t id, v4l2_queryctrl& ctrl) {
    ctrl = {};
    ctrl.id = id;
    return xioctl(fd, VIDIOC_QUERYCTRL, &ctrl) == 0 &&
           !(ctrl.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY));
}

bool has_control(int fd, uint32_t id) {
    v4l2_queryctrl ctrl{};
    return query_control(fd, id, ctrl);
}

bool accepts(int fd, uint32_t id, int32_t value) {
    v4l2_queryctrl ctrl{};
    if (!query_control(fd, id, ctrl) || value < ctrl.minimum || value > ctrl.maximum) return false;
    if (ctrl.type == V4L2_CTRL_TYPE_MENU || ctrl.type == V4L2_CTRL_TYPE_INTEGER_MENU) {
        v4l2_querymenu menu{};
        menu.id = id;
        menu.index = static_cast<uint32_t>(value);
        return xioctl(fd, VIDIOC_QUERYMENU, &menu) == 0;
    }
    return ctrl.step <= 1 || (int64_t(value) - ctrl.minimum) % ctrl.step == 0;
}

bool encode_controls(int fd, const Profile& profile) {
    return accepts(fd, profile.control, profile.value) && has_control(fd, profile.level) &&
           (!profile.hevc || accepts(fd, V4L2_CID_MPEG_VIDEO_HEVC_TIER, V4L2_MPEG_VIDEO_HEVC_TIER_MAIN)) &&
           has_control(fd, V4L2_CID_MPEG_VIDEO_BITRATE) &&
           has_control(fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE) &&
           accepts(fd, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0) &&
           has_control(fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME);
}

unsigned rate_modes(int fd, bool hevc) {
    unsigned modes = 0;
    constexpr auto mode = V4L2_CID_MPEG_VIDEO_BITRATE_MODE;
    if (accepts(fd, mode, V4L2_MPEG_VIDEO_BITRATE_MODE_CBR)) modes |= VA_RC_CBR;
    if (accepts(fd, mode, V4L2_MPEG_VIDEO_BITRATE_MODE_VBR)) modes |= VA_RC_VBR;
    const auto full_qp_range = [fd](uint32_t ctrl) { return accepts(fd, ctrl, 0) && accepts(fd, ctrl, 51); };
    if (accepts(fd, mode, V4L2_MPEG_VIDEO_BITRATE_MODE_CQ) && accepts(fd, V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 0) &&
        (hevc ? full_qp_range(frame_qp) : full_qp_range(min_qp) && full_qp_range(max_qp))) modes |= VA_RC_CQP;
    return modes;
}

uint32_t raw_format(uint32_t fourcc) {
    switch (fourcc) {
    case VA_FOURCC_NV12: return V4L2_PIX_FMT_NV12;
    case VA_FOURCC_I420: return V4L2_PIX_FMT_YUV420;
    case VA_FOURCC_YV12: return V4L2_PIX_FMT_YVU420;
    default: throw std::invalid_argument("unsupported V4L2 encoder raw format");
    }
}

int32_t level_control(bool hevc, uint32_t level) {
    // Tables are in the order of the V4L2 level menus, not of bitstream level_idc.
    constexpr uint32_t h264_levels[] = {10, 9, 11, 12, 13, 20, 21, 22, 30, 31,
                                       32, 40, 41, 42, 50, 51, 52, 60, 61, 62};
    constexpr uint32_t hevc_levels[] = {30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186};
    const auto* begin = hevc ? std::begin(hevc_levels) : std::begin(h264_levels);
    const auto* end = hevc ? std::end(hevc_levels) : std::end(h264_levels);
    const auto* it = std::find(begin, end, level);
    if (it == end) throw std::invalid_argument("unsupported V4L2 encoder level_idc");
    return static_cast<int32_t>(it - begin);
}

void control(int fd, uint32_t id, int32_t value, const char* what, Deadline deadline = Deadline::max()) {
    v4l2_control ctrl{};
    ctrl.id = id;
    ctrl.value = value;
    checked_ioctl(fd, VIDIOC_S_CTRL, &ctrl, what, deadline);
    if (ctrl.value != value) throw std::runtime_error(what);
}

void program_controls(int fd, const Profile& profile, const EncodeSettings& s) {
    v4l2_streamparm parm{};
    parm.type = output;
    parm.parm.output.timeperframe = {s.fps_den, s.fps_num};
    checked_ioctl(fd, VIDIOC_S_PARM, &parm, "set V4L2 encoder framerate");
    const auto& actual = parm.parm.output.timeperframe;
    if (!(parm.parm.output.capability & V4L2_CAP_TIMEPERFRAME) || !actual.numerator || !actual.denominator ||
        uint64_t(actual.numerator) * s.fps_num != uint64_t(actual.denominator) * s.fps_den) {
        throw std::runtime_error("V4L2 encoder changed requested framerate");
    }
    const bool cqp = s.rate_mode == VA_RC_CQP;
    if (has_control(fd, V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE)) {
        control(fd, V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, !cqp, "set V4L2 frame rate control");
    }
    control(fd, V4L2_CID_MPEG_VIDEO_BITRATE_MODE, cqp ? V4L2_MPEG_VIDEO_BITRATE_MODE_CQ :
            s.rate_mode == VA_RC_CBR ? V4L2_MPEG_VIDEO_BITRATE_MODE_CBR : V4L2_MPEG_VIDEO_BITRATE_MODE_VBR,
            "set V4L2 encoder bitrate mode");
    if (!cqp) control(fd, V4L2_CID_MPEG_VIDEO_BITRATE, static_cast<int32_t>(s.bitrate), "set V4L2 encoder bitrate");
    control(fd, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, "disable V4L2 encoder B frames");
    control(fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, static_cast<int32_t>(s.gop), "set V4L2 encoder GOP size");
    if (!profile.hevc && has_control(fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD)) {
        control(fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, static_cast<int32_t>(s.gop), "set V4L2 H.264 I-period");
    }
    control(fd, profile.control, profile.value, "set V4L2 encoder profile");
    if (s.level) control(fd, profile.level, level_control(profile.hevc, s.level), "set V4L2 encoder level");
    if (profile.hevc) {
        control(fd, V4L2_CID_MPEG_VIDEO_HEVC_TIER,
                s.high_tier ? V4L2_MPEG_VIDEO_HEVC_TIER_HIGH : V4L2_MPEG_VIDEO_HEVC_TIER_MAIN,
                "set V4L2 HEVC tier");
    }
    if (accepts(fd, V4L2_CID_MPEG_VIDEO_HEADER_MODE, V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME)) {
        control(fd, V4L2_CID_MPEG_VIDEO_HEADER_MODE, V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME,
                "join V4L2 encoder headers with the first frame");
    }
}

v4l2_pix_format_mplane set_format(int fd, v4l2_buf_type type, PictureSize size, uint32_t fourcc,
                                 uint32_t pitch, uint32_t bytes) {
    v4l2_format format{};
    format.type = type;
    auto& pix = format.fmt.pix_mp;
    pix.width = size.width;
    pix.height = size.height;
    pix.pixelformat = fourcc;
    pix.field = V4L2_FIELD_NONE;
    pix.num_planes = 1;
    pix.plane_fmt[0].bytesperline = pitch;
    pix.plane_fmt[0].sizeimage = bytes;
    checked_ioctl(fd, VIDIOC_S_FMT, &format,
                  type == capture ? "set V4L2 coded format" : "set V4L2 raw format");
    if (pix.pixelformat != fourcc || pix.num_planes != 1 || pix.field != V4L2_FIELD_NONE ||
        !pix.plane_fmt[0].sizeimage) {
        throw std::runtime_error("V4L2 encoder substituted an incompatible format");
    }
    return pix;
}

void verify_crop(int fd, PictureSize size) {
    v4l2_selection sel{};
    sel.type = V4L2_BUF_TYPE_VIDEO_OUTPUT; // selection uses the non-MPLANE type
    sel.target = V4L2_SEL_TGT_CROP;
    checked_ioctl(fd, VIDIOC_G_SELECTION, &sel, "read V4L2 encoder raw crop");
    const auto matches = [&] {
        return !sel.r.left && !sel.r.top && sel.r.width == size.width && sel.r.height == size.height;
    };
    if (!matches()) {
        sel.r = {0, 0, size.width, size.height};
        checked_ioctl(fd, VIDIOC_S_SELECTION, &sel, "set V4L2 encoder raw crop");
        checked_ioctl(fd, VIDIOC_G_SELECTION, &sel, "verify V4L2 encoder raw crop");
        if (!matches()) throw std::runtime_error("V4L2 encoder changed requested crop");
    }
}

v4l2_buffer buffer(v4l2_buf_type type, v4l2_memory memory, v4l2_plane& plane, uint32_t index = 0) {
    v4l2_buffer buf{};
    buf.index = index;
    buf.type = type;
    buf.memory = memory;
    buf.length = 1;
    buf.m.planes = &plane;
    return buf;
}

uint32_t request_buffers(int fd, v4l2_buf_type type, v4l2_memory memory, uint32_t count) {
    v4l2_requestbuffers req{};
    req.type = type;
    req.memory = memory;
    req.count = count;
    checked_ioctl(fd, VIDIOC_REQBUFS, &req, "allocate V4L2 encoder queue slots");
    if (!req.count || req.count > VIDEO_MAX_FRAME) throw std::runtime_error("invalid V4L2 queue size");
    return req.count;
}

struct Mapping {
    void* addr = MAP_FAILED;
    uint32_t length = 0;
    void reset() noexcept {
        if (addr != MAP_FAILED) munmap(addr, length);
        addr = MAP_FAILED;
    }
    ~Mapping() { reset(); }
};

void wait_ready(int fd, short events, Deadline deadline, const char* what) {
    pollfd pfd{fd, events, 0};
    for (;;) {
        const auto left = deadline - Clock::now();
        if (left <= Clock::duration::zero()) break;
        const auto ms = std::chrono::ceil<std::chrono::milliseconds>(left).count();
        const int result = poll(&pfd, 1, static_cast<int>(ms));
        if (result < 0) {
            if (errno == EINTR) continue;
            fail(what);
        }
        if (!result) break;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error(what);
        if (pfd.revents & events) return;
    }
    throw std::system_error(ETIMEDOUT, std::generic_category(), what);
}

void wait_for_producer(int fd, Deadline deadline) {
#ifdef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
    dma_buf_export_sync_file snapshot{DMA_BUF_SYNC_READ, -1};
    if (xioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &snapshot, deadline) == 0) {
        Fd fence(snapshot.fd);
        if (fence.get() < 0) throw std::runtime_error("invalid DMA-BUF producer fence");
        wait_ready(fence.get(), POLLIN, deadline, "wait for DMA-BUF producer fence");
        sync_file_info info{};
        checked_ioctl(fence.get(), SYNC_IOC_FILE_INFO, &info, "read DMA-BUF producer fence status", deadline);
        if (info.status <= 0) throw std::runtime_error("DMA-BUF producer fence failed");
        return;
    }
    if (errno != ENOTTY && errno != EINVAL && errno != EOPNOTSUPP) fail("export DMA-BUF producer fence");
#endif
    // Older kernels do not export sync files. POLLIN waits for implicit writers
    // without starting a CPU access or requiring the allocation to be mappable.
    wait_ready(fd, POLLIN, deadline, "wait for DMA-BUF producer");
}

struct NalKinds { bool vcl = false, idr = false; };

// The stateful driver delivers whole NAL units and one picture per VCL-bearing
// CAPTURE buffer. Check slice-start markers and preserve the original bytes.
// A V4L2 keyframe flag cannot distinguish an HEVC IDR from another IRAP.
NalKinds inspect_annex_b(const uint8_t* data, size_t size, bool hevc) {
    const auto start_code = [&](size_t from) {
        for (size_t i = from; size - i >= 3; ++i) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return i;
        }
        return size;
    };
    size_t start = start_code(0);
    if (start == size || std::any_of(data, data + start, [](uint8_t byte) { return byte != 0; })) {
        throw std::runtime_error("V4L2 encoder did not return Annex B data");
    }
    NalKinds kinds;
    while (start != size) {
        const size_t head = start + 3;
        const size_t next = start_code(head);
        if (next - head <= (hevc ? 2u : 1u) || (data[head] & 0x80) ||
            (hevc && !(data[head + 1] & 7))) throw std::runtime_error("invalid V4L2 Annex B NAL unit");
        const unsigned type = hevc ? (data[head] >> 1) & 63u : data[head] & 31u;
        if (!hevc && !type) throw std::runtime_error("invalid V4L2 H.264 NAL type");
        if (!hevc && type >= 2 && type <= 4) {
            throw std::runtime_error("unexpected H.264 data partition from V4L2 encoder");
        }
        if (hevc ? type < 32 : type == 1 || type == 5) {
            // HEVC first_slice_segment_in_pic_flag and the H.264 Exp-Golomb
            // code for first_mb_in_slice == 0 both start with a set bit.
            const bool first = (data[head + (hevc ? 2 : 1)] & 0x80) != 0;
            const bool idr = hevc ? type == 19 || type == 20 : type == 5;
            if (first == kinds.vcl || (kinds.vcl && kinds.idr != idr)) {
                throw std::runtime_error("V4L2 CAPTURE buffer does not contain exactly one complete picture");
            }
            kinds.vcl = true;
            kinds.idr = idr;
        }
        start = next;
    }
    return kinds;
}

} // namespace

EncodeRates v4l2_encode_rates() noexcept {
    EncodeRates rates{};
    visit_devices([&](Fd dev) {
        const int fd = dev.get();
        if (!std::any_of(std::begin(raw_formats), std::end(raw_formats),
                         [&](uint32_t raw) { return has_format(fd, output, raw); })) return false;
        const bool avc = has_format(fd, capture, V4L2_PIX_FMT_H264);
        const bool hevc = has_format(fd, capture, h265) || has_format(fd, capture, V4L2_PIX_FMT_HEVC);
        const auto avc_modes = avc ? rate_modes(fd, false) : 0u;
        const auto hevc_modes = hevc ? rate_modes(fd, true) : 0u;
        for (const auto& profile : profiles) {
            const auto modes = profile.hevc ? hevc_modes : avc_modes;
            if (modes && encode_controls(fd, profile)) rates[profile.va] |= modes;
        }
        return false;
    });
    return rates;
}

void wait_encode_input(int fd) {
    wait_for_producer(fd, Clock::now() + frame_timeout);
}

struct V4l2Encoder::Impl {
    Fd dev;
    EncodeLayout layout;
    const bool hevc;
    const size_t coded_capacity;
    std::array<Mapping, VIDEO_MAX_FRAME> mappings;
    uint32_t capture_count = 0;
    uint32_t bitrate = 0;
    bool output_streaming = false;
    bool capture_streaming = false;

    Impl(Fd fd, EncodeLayout raw, bool is_hevc, size_t capacity)
        : dev(std::move(fd)), layout(raw), hevc(is_hevc), coded_capacity(capacity) {}
    ~Impl() { stop(); }

    void stop() noexcept {
        if (dev.get() < 0) return;
        // STREAMOFF returns any borrowed input. Closing also tears down the
        // queues if either ioctl failed, before the caller can reuse its fd.
        auto raw = output;
        auto coded = capture;
        if (output_streaming) xioctl(dev.get(), VIDIOC_STREAMOFF, &raw);
        if (capture_streaming) xioctl(dev.get(), VIDIOC_STREAMOFF, &coded);
        // Release mmap-held references before close so it can release the queues.
        for (auto& map : mappings) map.reset();
        dev = Fd{};
    }

    void setup(const Profile& profile, const EncodeSettings& s, uint32_t raw_fmt, uint32_t coded_fmt) {
        const int fd = dev.get();
        const auto coded = set_format(fd, capture, s.size, coded_fmt, 0, static_cast<uint32_t>(coded_capacity));
        const uint32_t coded_bytes = coded.plane_fmt[0].sizeimage;
        if (coded.width != s.size.width || coded.height != s.size.height || coded_bytes > max_capture_plane) {
            throw std::runtime_error("V4L2 encoder changed coded dimensions or requested an oversized capture plane");
        }
        const auto raw = set_format(fd, output, s.size, raw_fmt, layout.planes[0].pitch, layout.bytes);
        const uint32_t padded_height = layout.planes[1].offset / layout.planes[0].pitch;
        if (raw.width != layout.planes[0].pitch || raw.height != padded_height ||
            raw.plane_fmt[0].bytesperline != layout.planes[0].pitch || raw.plane_fmt[0].sizeimage < layout.bytes) {
            throw std::runtime_error("V4L2 encoder raw layout differs from the native DMA-BUF layout");
        }
        layout.bytes = raw.plane_fmt[0].sizeimage;
        verify_crop(fd, s.size);
        program_controls(fd, profile, s);
        bitrate = s.bitrate;

        request_buffers(fd, output, V4L2_MEMORY_DMABUF, 1);
        capture_count = request_buffers(fd, capture, V4L2_MEMORY_MMAP, 2);
        for (uint32_t i = 0; i < capture_count; ++i) {
            v4l2_plane plane{};
            auto buf = buffer(capture, V4L2_MEMORY_MMAP, plane, i);
            checked_ioctl(fd, VIDIOC_QUERYBUF, &buf, "query V4L2 coded buffer");
            if (buf.length != 1 || plane.length < coded_bytes || plane.length > max_capture_plane) {
                throw std::runtime_error("invalid V4L2 coded buffer size");
            }
            auto& map = mappings[i];
            map.length = plane.length;
            map.addr = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
            if (map.addr == MAP_FAILED) fail("map V4L2 coded buffer");
            queue_capture(i);
        }
        auto type = capture;
        checked_ioctl(fd, VIDIOC_STREAMON, &type, "start V4L2 CAPTURE queue");
        capture_streaming = true;
        type = output;
        checked_ioctl(fd, VIDIOC_STREAMON, &type, "start V4L2 OUTPUT queue");
        output_streaming = true;
    }

    void queue_capture(uint32_t index, Deadline deadline = Deadline::max()) {
        v4l2_plane plane{};
        plane.length = mappings[index].length;
        auto buf = buffer(capture, V4L2_MEMORY_MMAP, plane, index);
        checked_ioctl(dev.get(), VIDIOC_QBUF, &buf, "queue V4L2 coded buffer", deadline);
    }

    bool dequeue(v4l2_buffer& buf, Deadline deadline) {
        if (xioctl(dev.get(), VIDIOC_DQBUF, &buf, deadline) < 0) {
            if (errno == EAGAIN) return false;
            fail("dequeue V4L2 encoder buffer");
        }
        if (buf.length != 1 || (buf.flags & (V4L2_BUF_FLAG_ERROR | V4L2_BUF_FLAG_LAST))) {
            throw std::runtime_error("V4L2 encoder returned an erroneous or final buffer");
        }
        return true;
    }

    void encode(int fd, uint32_t extent, bool keyframe, int qp, size_t limit, std::vector<uint8_t>& coded) {
        coded.clear();
        if (dev.get() < 0) throw std::runtime_error("V4L2 encoder is no longer streaming");
        limit = std::min({limit, coded_capacity, coded.max_size()});
        if (fd < 0 || extent < layout.bytes || !limit) throw std::invalid_argument("invalid V4L2 frame extent or limit");
        const auto deadline = Clock::now() + frame_timeout;
        wait_for_producer(fd, deadline);
        try {
            if (qp >= 0) {
                // AVC firmware ignores FRAME_LEVEL_QP; pin both quantizer bounds instead.
                if (hevc) control(dev.get(), frame_qp, qp, "set MediaTek frame QP", deadline);
                else {
                    control(dev.get(), min_qp, qp, "set MediaTek minimum QP", deadline);
                    control(dev.get(), max_qp, qp, "set MediaTek maximum QP", deadline);
                }
            }
            if (keyframe) {
                control(dev.get(), V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0, "force V4L2 encoder IDR", deadline);
            }
            v4l2_plane raw_plane{};
            raw_plane.m.fd = fd;
            raw_plane.length = extent;
            raw_plane.bytesused = layout.bytes;
            auto raw = buffer(output, V4L2_MEMORY_DMABUF, raw_plane);
            raw.field = V4L2_FIELD_NONE;
            checked_ioctl(dev.get(), VIDIOC_QBUF, &raw, "queue borrowed V4L2 DMA-BUF", deadline);

            bool raw_done = false, coded_done = false;
            while (!raw_done || !coded_done) {
                bool progressed = false;
                if (!raw_done) {
                    raw_plane = {};
                    raw = buffer(output, V4L2_MEMORY_DMABUF, raw_plane);
                    if (dequeue(raw, deadline)) {
                        if (raw.index != 0) throw std::runtime_error("V4L2 dequeued the wrong raw buffer");
                        raw_done = progressed = true;
                    }
                }
                if (!coded_done) {
                    v4l2_plane plane{};
                    auto out = buffer(capture, V4L2_MEMORY_MMAP, plane);
                    if (dequeue(out, deadline)) {
                        if (out.index >= capture_count || plane.bytesused <= plane.data_offset ||
                            plane.bytesused > mappings[out.index].length) {
                            throw std::runtime_error("invalid V4L2 coded payload extent");
                        }
                        const size_t size = plane.bytesused - plane.data_offset;
                        if (size > limit - coded.size()) throw std::length_error("V4L2 coded AU exceeds output limit");
                        const auto* data = static_cast<const uint8_t*>(mappings[out.index].addr) + plane.data_offset;
                        const auto nals = inspect_annex_b(data, size, hevc);
                        if (nals.vcl && keyframe && !nals.idr) throw std::runtime_error("V4L2 encoder did not produce an IDR");
                        coded.insert(coded.end(), data, data + size);
                        coded_done = nals.vcl;
                        queue_capture(out.index, deadline);
                        progressed = true;
                    }
                }
                if (!progressed) {
                    wait_ready(dev.get(), static_cast<short>((raw_done ? 0 : POLLOUT) | (coded_done ? 0 : POLLIN)),
                               deadline, "wait for V4L2 encoder buffers");
                }
            }
        } catch (...) {
            coded.clear();
            stop();
            throw;
        }
    }
};

V4l2Encoder::V4l2Encoder(VAProfile va_profile, const EncodeSettings& s, uint32_t fourcc, size_t coded_capacity) {
    const auto* profile = std::find_if(std::begin(profiles), std::end(profiles),
                                       [&](const Profile& p) { return p.va == va_profile; });
    if (profile == std::end(profiles)) throw std::invalid_argument("unsupported V4L2 encoder profile");
    constexpr uint32_t max_control = std::numeric_limits<int32_t>::max();
    if (!s.bitrate || s.bitrate > max_control || s.gop > max_control || !s.fps_num || !s.fps_den ||
        !coded_capacity || coded_capacity > max_capture_plane || (!profile->hevc && s.high_tier)) {
        throw std::invalid_argument("invalid V4L2 encoder settings");
    }
    const auto raw_fmt = raw_format(fourcc);
    const auto raw_layout = native_encode_layout(s.size, fourcc);
    const uint32_t codecs[] = {profile->hevc ? h265 : V4L2_PIX_FMT_H264, V4L2_PIX_FMT_HEVC};
    std::exception_ptr error;
    for (unsigned i = 0; i < (profile->hevc ? 2u : 1u); ++i) {
        visit_devices([&](Fd fd) {
            if (!has_format(fd.get(), capture, codecs[i]) || !has_format(fd.get(), output, raw_fmt) ||
                !encode_controls(fd.get(), *profile) || !(rate_modes(fd.get(), profile->hevc) & s.rate_mode)) return false;
            try {
                auto candidate = std::make_unique<Impl>(std::move(fd), raw_layout, profile->hevc, coded_capacity);
                candidate->setup(*profile, s, raw_fmt, codecs[i]);
                impl_ = std::move(candidate);
                return true;
            } catch (const std::runtime_error&) {
                if (!error) error = std::current_exception();
                return false;
            }
        });
        if (impl_) return;
    }
    if (error) std::rethrow_exception(error);
    throw std::runtime_error("no accessible MediaTek V4L2 encoder with the requested formats and controls");
}

V4l2Encoder::~V4l2Encoder() = default;
const EncodeLayout& V4l2Encoder::layout() const { return impl_->layout; }

void V4l2Encoder::set_bitrate(uint32_t bitrate) {
    if (impl_->dev.get() < 0) throw std::runtime_error("V4L2 encoder is no longer streaming");
    if (!bitrate || bitrate > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        throw std::invalid_argument("invalid V4L2 encoder bitrate");
    }
    if (bitrate == impl_->bitrate) return;
    try {
        control(impl_->dev.get(), V4L2_CID_MPEG_VIDEO_BITRATE, static_cast<int32_t>(bitrate),
                "update V4L2 encoder bitrate");
        impl_->bitrate = bitrate;
    } catch (...) {
        impl_->stop();
        throw;
    }
}

void V4l2Encoder::encode(int fd, uint32_t extent, bool keyframe, int qp, size_t limit, std::vector<uint8_t>& coded) {
    impl_->encode(fd, extent, keyframe, qp, limit, coded);
}

} // namespace c2::va
