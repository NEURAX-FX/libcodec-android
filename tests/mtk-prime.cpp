// SPDX-License-Identifier: Apache-2.0
#include "dmabuf.hpp"
#include "hidl.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext_vaapi.h>
}
#include <drm_fourcc.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <va/va_drmcommon.h>
#include <xf86drm.h>

#include <array>
#include <iostream>
#include <stdexcept>

namespace {

template<class T, void (*Free)(T**)> struct AvFree {
    void operator()(T* p) const { Free(&p); }
};
template<class T, void (*Free)(T**)> using AvRef = std::unique_ptr<T, AvFree<T, Free>>;

void expect(bool pass, const char* reason) {
    if (!pass) throw std::runtime_error(reason);
}

void av_ok(int status, const char* operation) {
    if (status >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(status, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}

void va_ok(VAStatus status, const char* operation) {
    if (status != VA_STATUS_SUCCESS) {
        throw std::runtime_error(std::string(operation) + ": " + vaErrorStr(status));
    }
}

struct Export {
    VADRMPRIMESurfaceDescriptor desc{};
    std::array<c2::Fd, 4> fds;
};

Export export_frame(VADisplay display, VASurfaceID id, uint32_t layers) {
    Export out;
    va_ok(vaExportSurfaceHandle(display, id, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                VA_EXPORT_SURFACE_READ_ONLY | layers, &out.desc), "export PRIME surface");
    expect(out.desc.num_objects <= out.fds.size(), "invalid exported object count");
    for (size_t i = 0; i < out.desc.num_objects; ++i) out.fds[i] = c2::Fd(out.desc.objects[i].fd);
    expect(out.desc.num_objects == 1 && out.desc.fourcc == VA_FOURCC_I420 &&
           out.desc.objects[0].drm_format_modifier == DRM_FORMAT_MOD_LINEAR, "expected one linear I420 object");
    expect((fcntl(out.fds[0].get(), F_GETFD) & FD_CLOEXEC) != 0, "export FD lacks CLOEXEC");
    return out;
}

struct stat identity(int fd) {
    struct stat info{};
    expect(fstat(fd, &info) == 0, "stat PRIME fd failed");
    return info;
}

bool same_object(int a, int b) {
    const auto sa = identity(a), sb = identity(b);
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

void import_gem(int drm, int fd) {
    uint32_t handle;
    expect(drmPrimeFDToHandle(drm, fd, &handle) == 0, "DRM PRIME FD-to-handle import failed");
    int again = -1;
    const auto result = drmPrimeHandleToFD(drm, handle, DRM_CLOEXEC, &again);
    const c2::Fd roundtrip(again);
    expect(drmCloseBufferHandle(drm, handle) == 0, "DRM GEM close failed");
    expect(result == 0 && same_object(fd, roundtrip.get()), "DRM import did not preserve the DMA-BUF");
}

using Hash = c2::Ref<GChecksum, g_checksum_free>;
std::string hash_planes(const Export& frame) {
    const auto& desc = frame.desc;
    const auto& layer = desc.layers[0];
    const auto bytes = desc.objects[0].size;
    expect(identity(frame.fds[0].get()).st_size == bytes, "wrong PRIME size");
    c2::CpuView view(frame.fds[0].get(), bytes, false);
    Hash hash(g_checksum_new(G_CHECKSUM_SHA256));
    for (size_t plane = 0; plane < 3; ++plane) {
        const size_t w = desc.width >> (plane != 0), h = desc.height >> (plane != 0);
        const size_t off = layer.offset[plane], pitch = layer.pitch[plane];
        expect(w && h && layer.object_index[plane] == 0 && pitch >= w &&
               off + (h - 1) * pitch + w <= bytes, "PRIME plane exceeds DMA-BUF");
        for (size_t y = 0; y < h; ++y) g_checksum_update(hash.get(), view.data() + off + y * pitch, w);
    }
    view.finish();
    return g_checksum_get_string(hash.get());
}

} // namespace

int main(int argc, char** argv) try {
    expect(argc == 3, "usage: c2-prime-test input.mp4 /dev/dri/renderD128");
    c2::Fd drm(open(argv[2], O_RDWR | O_CLOEXEC)); // separate GEM namespace from libva
    expect(drm.get() >= 0, "cannot open DRM render node");
    std::vector<std::pair<Export, std::string>> held;
    {
        AVFormatContext* in = nullptr;
        av_ok(avformat_open_input(&in, argv[1], nullptr, nullptr), "open fixture");
        AvRef<AVFormatContext, avformat_close_input> input(in);
        const AVCodec* decoder = nullptr;
        const auto video = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        av_ok(video, "select AVC stream");
        AvRef<AVCodecContext, avcodec_free_context> codec(avcodec_alloc_context3(decoder));
        AvRef<AVPacket, av_packet_free> packet(av_packet_alloc());
        AvRef<AVFrame, av_frame_free> frame(av_frame_alloc());
        expect(codec && packet && frame, "allocate FFmpeg test objects");
        av_ok(avcodec_parameters_to_context(codec.get(), in->streams[video]->codecpar), "configure decoder");
        av_ok(av_hwdevice_ctx_create(&codec->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, argv[2], nullptr, 0), "open VA display");
        const auto* hw = reinterpret_cast<const AVHWDeviceContext*>(codec->hw_device_ctx->data);
        const auto display = static_cast<const AVVAAPIDeviceContext*>(hw->hwctx)->display;
        codec->thread_count = 1;
        codec->get_format = [](AVCodecContext*, const AVPixelFormat* p) {
            for (; *p != AV_PIX_FMT_NONE; ++p) if (*p == AV_PIX_FMT_VAAPI) return *p;
            return AV_PIX_FMT_NONE;
        };
        av_ok(avcodec_open2(codec.get(), decoder, nullptr), "force VA decode");
        auto receive = [&] {
            for (;;) {
                const auto ret = avcodec_receive_frame(codec.get(), frame.get());
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                av_ok(ret, "receive VA frame");
                expect(frame->format == AV_PIX_FMT_VAAPI, "decoder returned a software frame");
                const auto surface = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(frame->data[3]));
                va_ok(vaSyncSurface(display, surface), "sync VA surface before export");
                if (held.empty()) {
                    VADRMPRIMESurfaceDescriptor denied{};
                    expect(vaExportSurfaceHandle(display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                                 VA_EXPORT_SURFACE_READ_WRITE, &denied) == VA_STATUS_ERROR_FLAG_NOT_SUPPORTED,
                           "writable PRIME export was not refused");
                }
                auto composed = export_frame(display, surface, VA_EXPORT_SURFACE_COMPOSED_LAYERS);
                const auto separate = export_frame(display, surface, VA_EXPORT_SURFACE_SEPARATE_LAYERS);
                expect(composed.fds[0].get() != separate.fds[0].get() &&
                       same_object(composed.fds[0].get(), separate.fds[0].get()), "exports must own independent FDs to the same object");
                const auto& layer = composed.desc.layers[0];
                expect(composed.desc.num_layers == 1 && layer.drm_format == DRM_FORMAT_YUV420 && layer.num_planes == 3 &&
                       separate.desc.num_layers == 3, "wrong PRIME layer layouts");
                for (size_t i = 0; i < 3; ++i) {
                    const auto& p = separate.desc.layers[i];
                    expect(p.drm_format == DRM_FORMAT_R8 && p.num_planes == 1 && p.object_index[0] == layer.object_index[i] &&
                           p.offset[0] == layer.offset[i] && p.pitch[0] == layer.pitch[i], "PRIME plane layouts disagree");
                }
                import_gem(drm.get(), composed.fds[0].get());
                auto hash = hash_planes(composed);
                held.emplace_back(std::move(composed), std::move(hash));
                av_frame_unref(frame.get()); // let FFmpeg reuse this VASurfaceID while the FD remains owned here
            }
        };
        int status;
        while ((status = av_read_frame(in, packet.get())) >= 0) {
            if (packet->stream_index == video) {
                av_ok(avcodec_send_packet(codec.get(), packet.get()), "submit AVC packet");
                receive();
            }
            av_packet_unref(packet.get());
        }
        if (status != AVERROR_EOF) av_ok(status, "read fixture");
        av_ok(avcodec_send_packet(codec.get(), nullptr), "drain AVC decoder");
        receive();
    }
    expect(held.size() == 29, "expected 29 hardware frames");
    for (const auto& item : held) expect(hash_planes(item.first) == item.second, "retained DMA-BUF was overwritten");
    std::cout << "DRM PRIME: 29 HAL buffers imported/re-exported as the same objects; pixels unchanged after decoder teardown.\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "c2-prime-test: " << e.what() << '\n';
    return 1;
}
