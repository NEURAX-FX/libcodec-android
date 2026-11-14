// SPDX-License-Identifier: Apache-2.0
#include "input.hpp"
#include "av1_packet.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace c2 {
namespace {

constexpr AVRational microseconds{1, AV_TIME_BASE};
constexpr size_t max_input = 64 * 1024 * 1024;

void check(int error, const char* operation) {
    if (error >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}

[[noreturn]] void file_error(const char* path) {
    throw std::system_error(errno, std::generic_category(), path);
}

void require_regular(const struct stat& info) {
    if (!S_ISREG(info.st_mode)) throw std::invalid_argument("input must be a regular file");
}

// Return the NAL header offset after the leading Annex B start code.
size_t annex_b_header(const uint8_t* data, size_t size) {
    if (size < 4 || data[0] || data[1]) return 0;
    size_t i = 2;
    while (i < size && !data[i]) ++i;
    return i < size - 1 && data[i] == 1 ? i + 1 : 0;
}

// See whether the initial AU already carries the out-of-band parameter sets.
// Only NALs before the first slice count. Keep a leading AUD first if inserting.
unsigned parameter_sets(const uint8_t* data, size_t size, bool hevc, size_t& after_aud) {
    unsigned sets = 0;
    const auto first_header = annex_b_header(data, size);
    for (size_t header = first_header; header && header < size;) {
        const unsigned type = hevc ? (data[header] >> 1) & 63 : data[header] & 31;
        if (hevc ? type < 32 : type >= 1 && type <= 5) break;
        if (hevc ? type >= 32 && type <= 34 : type == 7 || type == 8) {
            sets |= 1u << (type - (hevc ? 32 : 7));
        }
        size_t next = header + 1;
        while (size - next >= 3 && (data[next] || data[next + 1] || data[next + 2] != 1)) {
            ++next;
        }
        if (size - next < 3) break;
        if (header == first_header && type == (hevc ? 35u : 9u)) {
            after_aud = !data[next - 1] ? next - 1 : next;
        }
        header = next + 3;
    }
    return sets;
}

} // namespace

struct Input::Impl {
    int fd = -1;
    AVIOContext* io = nullptr;
    AVFormatContext* demux = nullptr;
    AVStream* stream = nullptr;
    AVPacket* packet = nullptr;
    AVBSFContext* bsf = nullptr;
    const AVCodecParameters* output = nullptr;
    PictureSize dimensions{};
    uint64_t next_timestamp = 0;
    uint64_t frame_step = 33333;
    std::vector<uint8_t> prefixed;
    int read_error = 0;
    bool auxiliary_open = false;
    bool demux_eof = false;
    bool bsf_eof = false;
    bool pending = false;
    bool ready = false;
    bool first = true;
    bool av1_sequence_seen = false;
    AVCodecID codec = AV_CODEC_ID_NONE;

    ~Impl() {
        av_packet_free(&packet);
        av_bsf_free(&bsf);
        avformat_close_input(&demux);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        if (fd >= 0) close(fd);
    }

    static int read_file(void* opaque, uint8_t* data, int size) {
        auto& self = *static_cast<Impl*>(opaque);
        ssize_t count;
        do { count = read(self.fd, data, size); } while (count < 0 && errno == EINTR);
        if (count < 0) return self.read_error = AVERROR(errno);
        return count ? static_cast<int>(count) : AVERROR_EOF;
    }

    static int64_t seek_file(void* opaque, int64_t offset, int whence) {
        const int fd = static_cast<Impl*>(opaque)->fd;
        whence &= ~AVSEEK_FORCE;
        if (whence == AVSEEK_SIZE) {
            struct stat info{};
            return fstat(fd, &info) < 0 ? AVERROR(errno) : info.st_size;
        }
        const auto position = lseek(fd, offset, whence);
        return position < 0 ? AVERROR(errno) : position;
    }

    static int deny_open(AVFormatContext* context, AVIOContext**, const char*, int,
                         AVDictionary**) {
        static_cast<Impl*>(context->opaque)->auxiliary_open = true;
        return AVERROR(EPERM);
    }

    void check_io() const {
        if (auxiliary_open) throw std::runtime_error("input references an auxiliary source");
        check(read_error, "read input file");
        if (io->error != AVERROR_EOF) check(io->error, "input I/O");
    }

    bool read_packet() {
        if (demux_eof) return false;
        const int result = av_read_frame(demux, packet);
        check_io();
        if (result == AVERROR_EOF) {
            demux_eof = true;
            return false;
        }
        check(result, "demux input packet");
        return true;
    }

    AVStream* video_stream() const {
        for (unsigned i = 0; i < demux->nb_streams; ++i) {
            auto* candidate = demux->streams[i];
            const auto* par = candidate->codecpar;
            if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
                !(candidate->disposition & AV_DISPOSITION_ATTACHED_PIC) &&
                (par->codec_id == AV_CODEC_ID_H264 || par->codec_id == AV_CODEC_ID_HEVC ||
                 par->codec_id == AV_CODEC_ID_AV1)) {
                return candidate;
            }
        }
        return nullptr;
    }

    void open_input(const char* path, const char* format) {
        const AVInputFormat* forced = nullptr;
        if (format) {
            const std::string_view name(format);
            if (name != "h264" && name != "hevc" && name != "obu") {
                throw std::invalid_argument("raw input format must be h264, hevc or obu");
            }
            forced = av_find_input_format(format);
            if (!forced) throw std::runtime_error(std::string("raw demuxer unavailable: ") + format);
        }
        if (!path || !*path) throw std::invalid_argument("missing input path");
        struct stat info{};
        // Pin the inode before opening it for reading so a path replacement
        // cannot make us open a FIFO, device or other special file.
        fd = open(path, O_PATH | O_CLOEXEC);
        if (fd < 0 || fstat(fd, &info) < 0) file_error(path);
        require_regular(info);
        char link[32];
        std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        const int source = open(link, O_RDONLY | O_CLOEXEC);
        if (source < 0) file_error(path);
        close(fd);
        fd = source;

        constexpr int buffer_size = 32 * 1024;
        auto* buffer = static_cast<uint8_t*>(av_malloc(buffer_size));
        if (!buffer) throw std::bad_alloc();
        io = avio_alloc_context(buffer, buffer_size, 0, this, read_file, nullptr, seek_file);
        if (!io) {
            av_free(buffer);
            throw std::bad_alloc();
        }
        demux = avformat_alloc_context();
        if (!demux) throw std::bad_alloc();
        demux->pb = io;
        demux->flags |= AVFMT_FLAG_CUSTOM_IO;
        demux->opaque = this;
        demux->io_open = deny_open;
        // Do not invoke playlist, external-source or device demuxers, even if probed.
        check(av_opt_set(demux, "format_whitelist", "mov,matroska,mpegts,mpeg,avi,flv,h264,hevc,ivf,obu", 0),
              "limit input formats");
        check(av_opt_set(demux, "protocol_whitelist", "none", 0), "disable input protocols");
        // The primary file is already open. Never hand the path to a URL handler.
        const int result = avformat_open_input(&demux, nullptr, forced, nullptr);
        check_io();
        check(result, "open input container");

        packet = av_packet_alloc();
        if (!packet) throw std::bad_alloc();
        // Only read headers and compressed packets. find_stream_info can open decoders.
        stream = video_stream();
        if (!stream && (demux->ctx_flags & AVFMTCTX_NOHEADER)) {
            while (read_packet()) {
                stream = video_stream();
                if (stream) {
                    pending = true;
                    break;
                }
                av_packet_unref(packet);
            }
        }
        if (!stream) throw std::runtime_error("input has no H.264, HEVC or AV1 video stream");
        codec = stream->codecpar->codec_id;
        ready = read_sample();
        const auto* par = stream->codecpar;
        if (par->width > 0 && par->height > 0) {
            dimensions = {static_cast<uint32_t>(par->width), static_cast<uint32_t>(par->height)};
        } else if (const auto* parser = av_stream_get_parser(stream);
                   parser && parser->width > 0 && parser->height > 0) {
            dimensions = {static_cast<uint32_t>(parser->width), static_cast<uint32_t>(parser->height)};
        }
        const auto rate = stream->avg_frame_rate;
        if (rate.num > 0 && rate.den > 0) {
            const auto step = av_rescale_q(1, av_inv_q(rate), microseconds);
            if (step > 0) frame_step = static_cast<uint64_t>(step);
        }
    }

    void init_filter() {
        const bool hevc = codec == AV_CODEC_ID_HEVC;
        const auto* par = stream->codecpar;
        output = par;
        size_t extra_size = 0;
        const auto* extra = av_packet_get_side_data(packet, AV_PKT_DATA_NEW_EXTRADATA, &extra_size);
        if (!extra) {
            extra = par->extradata;
            extra_size = par->extradata_size;
        }
        if (!extra_size || annex_b_header(extra, extra_size)) return;
        if (extra[0] != 1 || extra_size < (hevc ? 23u : 7u) ||
            extra_size > static_cast<size_t>(std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE)) {
            throw std::runtime_error("invalid AVC/HEVC configuration record");
        }
        const char* name = hevc ? "hevc_mp4toannexb" : "h264_mp4toannexb";
        const auto* filter = av_bsf_get_by_name(name);
        if (!filter) throw std::runtime_error(std::string("bitstream filter unavailable: ") + name);
        check(av_bsf_alloc(filter, &bsf), "allocate Annex B filter");
        if (extra == par->extradata) {
            check(avcodec_parameters_copy(bsf->par_in, par), "copy video parameters");
        } else {
            // Copy once, using the packet's updated configuration instead of
            // the stream's older extradata. FFmpeg takes an owned deep copy.
            AVCodecParameters parameters = *par;
            parameters.extradata = const_cast<uint8_t*>(extra);
            parameters.extradata_size = static_cast<int>(extra_size);
            check(avcodec_parameters_copy(bsf->par_in, &parameters), "copy video parameters");
        }
        bsf->time_base_in = stream->time_base;
        check(av_bsf_init(bsf), "initialize Annex B filter");
        output = bsf->par_out;
    }

    bool read_sample() {
        if (!pending) av_packet_unref(packet);
        for (;;) {
            if (bsf) {
                if (bsf_eof) return false;
                const int result = av_bsf_receive_packet(bsf, packet);
                if (result == 0) {
                    if (packet->size) return true;
                    av_packet_unref(packet);
                    continue;
                }
                if (result == AVERROR_EOF) {
                    bsf_eof = true;
                    return false;
                }
                if (result != AVERROR(EAGAIN) || demux_eof) check(result, "receive Annex B packet");
            }
            if (!pending && !read_packet()) {
                if (!bsf) return false;
                check(av_bsf_send_packet(bsf, nullptr), "drain Annex B filter");
                continue;
            }
            pending = false;
            if (packet->stream_index != stream->index || !packet->size) {
                av_packet_unref(packet);
                continue;
            }
            if (codec == AV_CODEC_ID_AV1) return true; // Keep the demuxer's temporal unit intact.
            if (!output || (!bsf && packet->side_data_elems)) init_filter();
            if (!bsf) return true;
            // Receive was drained to EAGAIN before reading another input packet.
            check(av_bsf_send_packet(bsf, packet), "send packet to Annex B filter");
        }
    }
    uint64_t timestamp() {
        const auto base = bsf ? bsf->time_base_out : stream->time_base;
        const auto ticks = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        auto value = next_timestamp;
        if (ticks != AV_NOPTS_VALUE) {
            const auto scaled = av_rescale_q(ticks, base, microseconds);
            if (scaled == std::numeric_limits<int64_t>::min()) {
                throw std::runtime_error("video timestamp out of range");
            }
            value = scaled > 0 ? static_cast<uint64_t>(scaled) : 0;
        }
        const auto duration = packet->duration > 0 ? av_rescale_q(packet->duration, base, microseconds) : 0;
        const auto step = duration > 0 ? static_cast<uint64_t>(duration) : frame_step;
        const auto max = std::numeric_limits<uint64_t>::max();
        next_timestamp = std::max(next_timestamp, value > max - step ? max : value + step);
        return value;
    }

    void prepend_annex_b_config(const uint8_t* data, size_t size, const uint8_t* extra, size_t extra_size) {
        if (!annex_b_header(data, size)) throw std::runtime_error("video packet is not Annex B");
        const bool hevc = codec == AV_CODEC_ID_HEVC;
        if (extra_size) {
            // The BSF normally injects configuration at random-access pictures.
            // Supply missing configuration also for an initial non-IDR/IRAP AU,
            // or for already-Annex-B streams with out-of-band parameter sets.
            if (!annex_b_header(extra, extra_size)) {
                throw std::runtime_error("video configuration is not Annex B");
            }
            size_t ignored = 0, split = 0;
            const auto required = parameter_sets(extra, extra_size, hevc, ignored);
            const auto present = parameter_sets(data, size, hevc, split);
            if ((present & required) != required) {
                if (extra_size > max_input - size) throw std::runtime_error("video configuration exceeds packet limit");
                prefixed.reserve(extra_size + size);
                prefixed.insert(prefixed.end(), data, data + split);
                prefixed.insert(prefixed.end(), extra, extra + extra_size);
                prefixed.insert(prefixed.end(), data + split, data + size);
            }
        }
    }

    bool next(Packet& view) {
        view = {};
        prefixed.clear();
        if (!ready && !read_sample()) return false;
        ready = false;
        const auto* data = packet->data;
        const auto size = static_cast<size_t>(packet->size);
        if (size > max_input) throw std::runtime_error("video access unit exceeds 64 MiB");
        size_t extra_size = 0;
        const auto* extra = !bsf && packet->side_data_elems
            ? av_packet_get_side_data(packet, AV_PKT_DATA_NEW_EXTRADATA, &extra_size) : nullptr;
        if (!extra && first) {
            const auto* par = output ? output : stream->codecpar;
            extra = par->extradata;
            extra_size = par->extradata_size;
        }
        if (codec == AV_CODEC_ID_AV1) {
            if (packet->flags & AV_PKT_FLAG_CORRUPT) throw std::runtime_error("corrupt AV1 access unit");
            const auto unit = prepare_av1_packet(data, size, extra, extra_size, av1_sequence_seen);
            if (unit.size() != size) {
                prefixed.resize(unit.size());
                unit.copy_to(prefixed.data());
            }
        } else {
            prepend_annex_b_config(data, size, extra, extra_size);
        }
        first = false;
        view = {prefixed.empty() ? data : prefixed.data(),
                prefixed.empty() ? size : prefixed.size(), timestamp()};
        return true;
    }
};

Input::Input(const char* path, const char* format) : impl_(std::make_unique<Impl>()) {
    impl_->open_input(path, format);
}

Input::~Input() = default;

std::string_view Input::mime() const {
    if (impl_->codec == AV_CODEC_ID_AV1) return "video/av01";
    return impl_->codec == AV_CODEC_ID_HEVC ? "video/hevc" : "video/avc";
}
PictureSize Input::size() const { return impl_->dimensions; }
bool Input::next(Packet& packet) { return impl_->next(packet); }

} // namespace c2
