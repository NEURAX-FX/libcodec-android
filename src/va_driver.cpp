// SPDX-License-Identifier: Apache-2.0
#include "component.hpp"
#include "dmabuf.hpp"
#include "frame.hpp"
#include "va_encode.hpp"
#include "va_h264.hpp"
#include "va_hevc.hpp"

#include <drm_fourcc.h>
#include <va/va_backend.h>
#include <va/va_drmcommon.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace c2::va {
namespace {

constexpr VAProfile profiles[] = {VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High,
                                  VAProfileHEVCMain};
constexpr VAImageFormat i420{VA_FOURCC_I420, VA_LSB_FIRST, 12, 0, 0, 0, 0, 0, {}};
constexpr VAImageFormat image_formats[] = {i420,
    {VA_FOURCC_NV12, VA_LSB_FIRST, 12, 0, 0, 0, 0, 0, {}},
    {VA_FOURCC_YV12, VA_LSB_FIRST, 12, 0, 0, 0, 0, 0, {}}};
constexpr unsigned enc_min_width = 160, enc_min_height = 128;
constexpr unsigned max_config_attributes = 8;
constexpr unsigned min_dim = 64, max_width = 4096, max_height = 2304;
constexpr size_t max_au = 64 * 1024 * 1024, max_slices = 4096;
constexpr uint64_t sync_limit_ns = 8'000'000'000;
constexpr uint8_t start_code[] = {0, 0, 0, 1};

struct Status { VAStatus code; };
void require(bool ok, VAStatus code) { if (!ok) throw Status{code}; }

class EncodingFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

template<class Map> auto& get(Map& map, VAGenericID id, VAStatus code) {
    const auto it = map.find(id);
    if (it == map.end()) throw Status{code};
    return it->second;
}

bool supported(VAProfile profile) {
    return std::find(std::begin(profiles), std::end(profiles), profile) != std::end(profiles);
}

bool valid_size(unsigned w, unsigned h) {
    return w >= min_dim && w <= max_width && h >= min_dim && h <= max_height && !((w | h) & 1);
}

bool same_size(PictureSize a, PictureSize b) { return a.width == b.width && a.height == b.height; }
struct Config {
    VAProfile profile;
    VAEntrypoint entry;
    unsigned rate = VA_RC_VBR;
    bool encode() const { return entry == VAEntrypointEncSlice; }
};

const VAImageFormat& image_format(unsigned fourcc) {
    for (const auto& f : image_formats) if (f.fourcc == fourcc) return f;
    throw Status{VA_STATUS_ERROR_INVALID_IMAGE_FORMAT};
}

struct Buffer {
    VAContextID context;
    VABufferType type;
    unsigned size, count;
    std::vector<uint8_t> data;
    bool mapped = false;
    std::shared_ptr<EncodeSurface> raw;
    std::unique_ptr<CpuView> mapping;
    VACodedBufferSegment coded{};
};
using Buf = std::shared_ptr<Buffer>;

template<class Picture, class Iq, class Slice> struct Submission {
    Picture picture{};
    std::optional<Iq> iq;
    std::vector<Slice> slices;
    size_t length = 0;
};

// Both VA long-slice layouts pair a parameter array with its data buffer.
// Keep slice payloads borrowed until they can be copied into the input DMA-BUF.
template<class Picture, class Iq, class Param, class Slice, class MakeSlice>
Submission<Picture, Iq, Slice> parse_submission(const std::vector<Buf>& buffers, MakeSlice make_slice) {
    Submission<Picture, Iq, Slice> out;
    bool have_picture = false;
    const Buffer* params = nullptr;
    for (const auto& b : buffers) {
        switch (b->type) {
        case VAPictureParameterBufferType:
            require(!have_picture && !params && out.slices.empty(), VA_STATUS_ERROR_INVALID_PARAMETER);
            std::memcpy(&out.picture, b->data.data(), sizeof(Picture));
            have_picture = true;
            break;
        case VAIQMatrixBufferType:
            require(!out.iq && !params && out.slices.empty(), VA_STATUS_ERROR_INVALID_PARAMETER);
            std::memcpy(&out.iq.emplace(), b->data.data(), sizeof(Iq));
            break;
        case VASliceParameterBufferType:
            require(have_picture && !params, VA_STATUS_ERROR_INVALID_PARAMETER);
            params = b.get();
            break;
        case VASliceDataBufferType: {
            require(params && params->count <= max_slices - out.slices.size(), VA_STATUS_ERROR_INVALID_PARAMETER);
            const auto bytes = size_t(b->size) * b->count;
            for (unsigned i = 0; i < params->count; ++i) {
                Param p;
                std::memcpy(&p, params->data.data() + size_t(i) * params->size, sizeof(p));
                require(p.slice_data_flag == VA_SLICE_DATA_FLAG_ALL && p.slice_data_size &&
                        p.slice_data_offset <= bytes && p.slice_data_size <= bytes - p.slice_data_offset,
                        VA_STATUS_ERROR_INVALID_PARAMETER);
                const auto extra = size_t(p.slice_data_size) + sizeof(start_code);
                require(extra <= max_au - out.length, VA_STATUS_ERROR_MAX_NUM_EXCEEDED);
                out.length += extra;
                out.slices.push_back(make_slice(b->data.data() + p.slice_data_offset, p));
            }
            params = nullptr;
            break;
        }
        default: throw Status{VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE};
        }
    }
    require(have_picture && !params && !out.slices.empty(), VA_STATUS_ERROR_INVALID_PARAMETER);
    return out;
}

enum class State { idle, collecting, queued, ready, failed };
struct Surface {
    PictureSize size;
    State state = State::idle;
    VAContextID owner = VA_INVALID_ID;
    std::optional<I420Buffer> frame;
    uint32_t fourcc = VA_FOURCC_I420;
    bool decode_only = false;
    bool encode_hint = false;
    std::shared_ptr<EncodeSurface> raw;
};
using Surfaces = std::unordered_map<VASurfaceID, Surface>;

EncodeLayout decoded_pixels(const I420Layout& source) {
    EncodeLayout out{source.size, VA_FOURCC_YV12, source.bytes, {}};
    for (size_t i = 0; i < out.planes.size(); ++i) {
        const auto& p = source.planes[i ? 3 - i : 0];
        out.planes[i] = {p.offset, p.pitch};
    }
    return out;
}

EncodeLayout image_pixels(const VAImage& image) {
    EncodeLayout layout{{image.width, image.height}, image.format.fourcc, image.data_size, {}};
    for (unsigned i = 0; i < image.num_planes; ++i) layout.planes[i] = {image.offsets[i], image.pitches[i]};
    return layout;
}

EncodeLayout pixel_region(EncodeLayout layout, int x, int y, unsigned w, unsigned h) {
    require(x >= 0 && y >= 0 && w && h && !((w | h | x | y) & 1) &&
            unsigned(x) <= layout.size.width && w <= layout.size.width - unsigned(x) &&
            unsigned(y) <= layout.size.height && h <= layout.size.height - unsigned(y),
            VA_STATUS_ERROR_INVALID_PARAMETER);
    layout.size = {w, h};
    for (unsigned i = 0; i < encode_planes(layout.fourcc); ++i) {
        const auto sub = i != 0;
        const auto chroma_x = sub && layout.fourcc != VA_FOURCC_NV12 ? 1 : 0;
        layout.planes[i].offset += (unsigned(y) >> sub) * layout.planes[i].pitch + (unsigned(x) >> chroma_x);
    }
    return layout;
}

bool compatible_image_formats(uint32_t dst, uint32_t src) {
    return dst == src || (encode_planes(dst) == 3 && encode_planes(src) == 3);
}

class Context {
public:
    Context(VAContextID id, Config config, PictureSize size, Surfaces& surfaces)
        : id(id), profile(config.profile), size(size), surfaces_(surfaces) {
        if (config.encode()) encoder.emplace(profile, config.rate);
    }
    ~Context() {
        closing_ = true;
        component_.reset();
        if (target != VA_INVALID_ID) {
            if (encoder) surfaces_.at(target).state = source_state;
            else fail(target);
        }
        for (const auto& p : pending_) if (p.surface != VA_INVALID_ID) fail(p.surface);
    }

    void submit(VASurfaceID surface, const std::vector<Buf>& buffers) {
        // VA clients reorder pictures themselves and have no Codec2 EOS
        // call for their last surface. Both adapters request decode order.
        if (profile == VAProfileHEVCMain) {
            const auto picture = parse_submission<VAPictureParameterBufferHEVC, VAIQMatrixBufferHEVC,
                                                  VASliceParameterBufferHEVC, HevcSlice>(buffers,
                [](const uint8_t* data, const VASliceParameterBufferHEVC& p) {
                    return HevcSlice{data, p.slice_data_size, p.slice_segment_address};
                });
            const auto& p = picture.picture;
            require(p.CurrPic.picture_id == surface && p.pic_width_in_luma_samples == size.width &&
                    p.pic_height_in_luma_samples == size.height, VA_STATUS_ERROR_INVALID_PARAMETER);
            const auto h = hevc_headers(p, picture.iq ? &*picture.iq : nullptr, picture.slices);
            queue_picture(surface, picture.slices, picture.length, h.restart, {&h.vps, &h.sps, &h.pps});
        } else {
            const auto picture = parse_submission<VAPictureParameterBufferH264, VAIQMatrixBufferH264,
                                                  VASliceParameterBufferH264, AvcSlice>(buffers,
                [](const uint8_t* data, const VASliceParameterBufferH264& p) {
                    return AvcSlice{data, p.slice_data_size, p.first_mb_in_slice,
                                    p.num_ref_idx_l0_active_minus1, p.num_ref_idx_l1_active_minus1};
                });
            const auto& p = picture.picture;
            require(picture.iq && p.CurrPic.picture_id == surface &&
                    (p.picture_width_in_mbs_minus1 + 1u) * 16 == size.width &&
                    (p.picture_height_in_mbs_minus1 + 1u) * 16 == size.height, VA_STATUS_ERROR_INVALID_PARAMETER);
            const auto h = avc_headers(profile, p, *picture.iq, picture.slices, true);
            queue_picture(surface, picture.slices, picture.length, h.idr, {&h.sps, &h.pps});
        }
    }

private:
    template<class Slice>
    void queue_picture(VASurfaceID surface, const std::vector<Slice>& slices, size_t length, bool restart,
                       std::initializer_list<const std::vector<uint8_t>*> headers) {
        if (!component_) {
            require(restart, VA_STATUS_ERROR_DECODING_ERROR);
            open();
        }
        component_->check();
        for (const auto* header : headers) {
            require(header->size() <= max_au - length, VA_STATUS_ERROR_MAX_NUM_EXCEEDED);
            length += header->size();
        }
        uint32_t capacity;
        const auto dma = upload_packet(heap_.get(), length, capacity, [&](uint8_t* dst) {
            const auto append = [&](const uint8_t* bytes, size_t n) {
                std::memcpy(dst, bytes, n);
                dst += n;
            };
            for (const auto* header : headers) append(header->data(), header->size());
            for (const auto& s : slices) {
                append(start_code, sizeof(start_code));
                append(s.data, s.size);
            }
        });
        require(next_ < UINT64_MAX, VA_STATUS_ERROR_MAX_NUM_EXCEEDED);
        auto& p = pending_[next_ % pending_.size()];
        require(p.surface == VA_INVALID_ID && !broken_, VA_STATUS_ERROR_DECODING_ERROR);
        const auto index = next_++;
        p = {index, surface};
        surfaces_.at(surface).state = State::queued;
        try {
            component_->queue({index, index + 1, 0, dma.get(), capacity, static_cast<uint32_t>(length)});
        } catch (...) {
            broken_ = true;
            for (const auto& item : pending_) if (item.surface != VA_INVALID_ID) fail(item.surface);
            throw;
        }
    }

public:

    void check() const {
        require(!broken_, VA_STATUS_ERROR_DECODING_ERROR);
        if (component_) component_->check();
    }

    const VAContextID id;
    const VAProfile profile;
    const PictureSize size;
    VASurfaceID target = VA_INVALID_ID;
    std::vector<Buf> submitted;
    std::optional<VaEncoder> encoder;
    State source_state = State::idle;

private:
    struct Pending { uint64_t index = 0; VASurfaceID surface = VA_INVALID_ID; };

    void fail(VASurfaceID id) noexcept {
        const auto it = surfaces_.find(id);
        if (it != surfaces_.end() && it->second.owner == this->id) it->second.state = State::failed;
    }

    void open() {
        auto heap = open_dma_heap("/dev/dma_heap/system");
        store_ = std::make_unique<Store>(GBINDER_DEFAULT_HWBINDER, "default");
        auto traits = store_->list();
        classify(traits, "default", vendor_xml);
        const bool hevc = profile == VAProfileHEVCMain;
        const auto& trait = choose_decoder(traits, hevc ? "video/hevc" : "video/avc",
                                           hevc ? "c2.mtk.hevc.decoder" : "c2.mtk.avc.decoder");
        auto component = std::make_unique<Component>(*store_, trait.name,
            [&](const WorkResult& work) { done(work); });
        configure_readback(*component, size);
        component->start();
        heap_ = std::move(heap);
        component_ = std::move(component);
    }

    void done(const WorkResult& work) {
        if (closing_) return;
        auto& p = pending_[work.index % pending_.size()];
        if (p.surface == VA_INVALID_ID || p.index != work.index || (work.flags & flag_eos)) {
            throw std::runtime_error("unexpected Codec2 completion in VA-API decode");
        }
        if (work.flags & flag_incomplete) {
            if (!work.blocks.empty()) throw std::runtime_error("partial Codec2 picture is unsupported");
            return;
        }
        const auto it = surfaces_.find(std::exchange(p.surface, VA_INVALID_ID));
        if (it == surfaces_.end()) return;
        auto& s = it->second;
        s.state = State::failed;
        if (work.blocks.size() != 1) throw std::runtime_error("Codec2 did not return one frame for the VA surface");
        s.frame.emplace(frame_reader_.retain(work.blocks.front(), s.size));
        s.state = State::ready;
    }

    Surfaces& surfaces_;
    std::array<Pending, 32> pending_{};
    uint64_t next_ = 0;
    FrameReader frame_reader_;
    Fd heap_;
    std::unique_ptr<Store> store_;
    std::unique_ptr<Component> component_;
    bool closing_ = false, broken_ = false;
};

class Driver {
public:
    const EncodeRates rates = v4l2_encode_rates();

    bool can_encode(VAProfile profile) const {
        const auto i = static_cast<unsigned>(profile);
        return i < rates.size() && rates[i];
    }
    VAStatus query_profiles(VAProfile* out, int* count) {
        require(out && count, VA_STATUS_ERROR_INVALID_PARAMETER);
        std::copy(std::begin(profiles), std::end(profiles), out);
        *count = std::size(profiles);
        return VA_STATUS_SUCCESS;
    }

    VAStatus query_entrypoints(VAProfile profile, VAEntrypoint* out, int* count) {
        require(supported(profile), VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        require(out && count, VA_STATUS_ERROR_INVALID_PARAMETER);
        *out = VAEntrypointVLD;
        *count = 1;
        if (can_encode(profile)) out[(*count)++] = VAEntrypointEncSlice;
        return VA_STATUS_SUCCESS;
    }

    VAStatus get_config_attributes(VAProfile profile, VAEntrypoint entry, VAConfigAttrib* a, int n) {
        check_profile(profile, entry);
        require(n >= 0 && (!n || a), VA_STATUS_ERROR_INVALID_PARAMETER);
        for (int i = 0; i < n; ++i) a[i].value = config_value(profile, entry, a[i].type);
        return VA_STATUS_SUCCESS;
    }

    VAStatus create_config(VAProfile profile, VAEntrypoint entry, VAConfigAttrib* a, int n, VAConfigID* out) {
        check_profile(profile, entry);
        require(out && n >= 0 && (!n || a), VA_STATUS_ERROR_INVALID_PARAMETER);
        Config config{profile, entry};
        if (config.encode()) {
            const auto mask = rates[profile];
            config.rate = mask & VA_RC_VBR ? VA_RC_VBR : mask & VA_RC_CBR ? VA_RC_CBR : VA_RC_CQP;
        }
        for (int i = 0; i < n; ++i) {
            if (config.encode() && a[i].type == VAConfigAttribRateControl) {
                const auto mode = a[i].value;
                require((mode & rates[profile]) && !(mode & (mode - 1)), VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
                config.rate = a[i].value;
            } else {
                const auto type = a[i].type;
                const bool configurable = type == VAConfigAttribRTFormat ||
                    type == (config.encode() ? VAConfigAttribEncPackedHeaders : VAConfigAttribDecSliceMode);
                require(configurable && a[i].value == config_value(profile, entry, type), VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
            }
        }
        const auto id = allocate_id();
        configs_.emplace(id, config);
        *out = id;
        return VA_STATUS_SUCCESS;
    }

    VAStatus destroy_config(VAConfigID id) {
        require(configs_.erase(id), VA_STATUS_ERROR_INVALID_CONFIG);
        return VA_STATUS_SUCCESS;
    }

    VAStatus query_config(VAConfigID id, VAProfile* profile, VAEntrypoint* entry, VAConfigAttrib* a, int* n) {
        require(profile && entry && a && n, VA_STATUS_ERROR_INVALID_PARAMETER);
        const auto& config = get(configs_, id, VA_STATUS_ERROR_INVALID_CONFIG);
        *profile = config.profile;
        *entry = config.entry;
        a[0] = {VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420};
        a[1] = config.encode() ? VAConfigAttrib{VAConfigAttribRateControl, config.rate}
                               : VAConfigAttrib{VAConfigAttribDecSliceMode, VA_DEC_SLICE_MODE_NORMAL};
        *n = 2;
        return VA_STATUS_SUCCESS;
    }

    VAStatus query_surface_attributes(VAConfigID id, VASurfaceAttrib* a, unsigned* n) {
        const auto& config = get(configs_, id, VA_STATUS_ERROR_INVALID_CONFIG);
        require(n, VA_STATUS_ERROR_INVALID_PARAMETER);
        const bool encode = config.encode();
        const unsigned count = encode ? 8 : 6;
        if (a && *n < count) {
            *n = count;
            return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
        }
        *n = count;
        if (!a) return VA_STATUS_SUCCESS;
        const auto push = [&](VASurfaceAttribType type, int value, bool writable = false) {
            *a = {type, VA_SURFACE_ATTRIB_GETTABLE | (writable ? VA_SURFACE_ATTRIB_SETTABLE : 0u),
                  {VAGenericValueTypeInteger, {value}}};
            ++a;
        };
        if (encode) push(VASurfaceAttribPixelFormat, VA_FOURCC_NV12, true);
        push(VASurfaceAttribPixelFormat, VA_FOURCC_I420, true);
        if (encode) push(VASurfaceAttribPixelFormat, VA_FOURCC_YV12, true);
        push(VASurfaceAttribMemoryType,
             VA_SURFACE_ATTRIB_MEM_TYPE_VA | (encode ? VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 : 0), true);
        push(VASurfaceAttribMinWidth, encode ? enc_min_width : min_dim);
        push(VASurfaceAttribMinHeight, encode ? enc_min_height : min_dim);
        push(VASurfaceAttribMaxWidth, max_width);
        push(VASurfaceAttribMaxHeight, max_height);
        return VA_STATUS_SUCCESS;
    }

    VAStatus create_surfaces_legacy(int w, int h, int format, int n, VASurfaceID* out) {
        return create_surfaces(format, w, h, out, n, nullptr, 0);
    }

    VAStatus create_surfaces(unsigned format, unsigned w, unsigned h, VASurfaceID* out, unsigned n,
                             VASurfaceAttrib* a, unsigned na) {
        require(format == VA_RT_FORMAT_YUV420, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        require(valid_size(w, h), VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
        require(out && n && n <= 128 && (!na || a), VA_STATUS_ERROR_INVALID_PARAMETER);
        unsigned memory = VA_SURFACE_ATTRIB_MEM_TYPE_VA, fourcc = 0, usage = 0;
        const VADRMPRIMESurfaceDescriptor* external = nullptr;
        for (unsigned i = 0; i < na; ++i) {
            if (a[i].type == VASurfaceAttribExternalBufferDescriptor) {
                require(a[i].value.type == VAGenericValueTypePointer && a[i].value.value.p && !external,
                        VA_STATUS_ERROR_INVALID_PARAMETER);
                external = static_cast<const VADRMPRIMESurfaceDescriptor*>(a[i].value.value.p);
                continue;
            }
            require(a[i].value.type == VAGenericValueTypeInteger, VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
            const auto value = static_cast<unsigned>(a[i].value.value.i);
            switch (a[i].type) {
            case VASurfaceAttribPixelFormat:
                require(encode_pixel_format(value), VA_STATUS_ERROR_INVALID_IMAGE_FORMAT);
                fourcc = value;
                break;
            case VASurfaceAttribMemoryType:
                require(value == VA_SURFACE_ATTRIB_MEM_TYPE_VA || value == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                        VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
                memory = value;
                break;
            case VASurfaceAttribUsageHint:
                usage = value;
                break;
            default: throw Status{VA_STATUS_ERROR_ATTR_NOT_SUPPORTED};
            }
        }
        const bool imported = memory == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
        require(imported == bool(external), VA_STATUS_ERROR_INVALID_PARAMETER);
        const bool encode_hint = usage & VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER;
        const bool decode_only = (usage & VA_SURFACE_ATTRIB_USAGE_HINT_DECODER) && !encode_hint;
        require(!imported || !decode_only, VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
        unsigned i = 0;
        try {
            for (; i < n; ++i) {
                auto pixel = fourcc ? fourcc : VA_FOURCC_I420;
                std::shared_ptr<EncodeSurface> raw;
                if (imported) {
                    raw = std::make_shared<EncodeSurface>(import_encode_surface({w, h}, fourcc, external[i]));
                    pixel = raw->layout.fourcc;
                }
                const auto id = allocate_id();
                surfaces_.emplace(id, Surface{{w, h}, State::idle, VA_INVALID_ID, {}, pixel, decode_only,
                                             encode_hint, std::move(raw)});
                out[i] = id;
            }
        } catch (...) {
            while (i) surfaces_.erase(out[--i]);
            throw;
        }
        return VA_STATUS_SUCCESS;
    }

    bool uses_codec2() const { return decode_contexts_ != 0; }

    VAStatus destroy_surfaces(VASurfaceID* ids, int n) {
        require(n >= 0 && (!n || ids), VA_STATUS_ERROR_INVALID_PARAMETER);
        for (int i = 0; i < n; ++i) {
            auto& s = surface(ids[i]);
            require(s.state != State::collecting && s.state != State::queued, VA_STATUS_ERROR_SURFACE_BUSY);
            require(s.raw.use_count() <= 1, VA_STATUS_ERROR_SURFACE_BUSY);
        }
        for (int i = 0; i < n; ++i) surfaces_.erase(ids[i]);
        return VA_STATUS_SUCCESS;
    }

    VAStatus create_context(VAConfigID config, int w, int h, int flag, VASurfaceID* targets, int n,
                            VAContextID* out) {
        const auto cfg = get(configs_, config, VA_STATUS_ERROR_INVALID_CONFIG);
        require(flag == VA_PROGRESSIVE, VA_STATUS_ERROR_FLAG_NOT_SUPPORTED);
        require(valid_size(w, h) && (cfg.encode() ? (unsigned(w) >= enc_min_width && unsigned(h) >= enc_min_height) :
                (cfg.profile == VAProfileHEVCMain || !((w | h) & 15))), VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
        require(out && n >= 0 && (!n || targets), VA_STATUS_ERROR_INVALID_PARAMETER);
        const PictureSize size{static_cast<unsigned>(w), static_cast<unsigned>(h)};
        for (int i = 0; i < n; ++i) require(same_size(surface(targets[i]).size, size), VA_STATUS_ERROR_INVALID_SURFACE);
        const auto id = allocate_id();
        contexts_.try_emplace(id, id, cfg, size, surfaces_);
        if (!cfg.encode()) ++decode_contexts_;
        *out = id;
        return VA_STATUS_SUCCESS;
    }

    VAStatus destroy_context(VAContextID id) {
        if (!context(id).encoder) --decode_contexts_;
        contexts_.erase(id);
        for (auto it = buffers_.begin(); it != buffers_.end();) {
            if (it->second->context == id) it = buffers_.erase(it);
            else ++it;
        }
        return VA_STATUS_SUCCESS;
    }

    VAStatus create_buffer(VAContextID id, VABufferType type, unsigned size, unsigned count, void* data,
                           VABufferID* out) {
        const auto& c = context(id);
        const bool hevc = c.profile == VAProfileHEVCMain;
        require(out && size && count && size <= max_au / count, VA_STATUS_ERROR_INVALID_PARAMETER);
        if (c.encoder) {
            require(encode_buffer_shape(c.profile, type, size, count), VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
            require(type != VAEncCodedBufferType || !data, VA_STATUS_ERROR_INVALID_PARAMETER);
            *out = add_buffer(id, type, size, count, data);
            return VA_STATUS_SUCCESS;
        }
        switch (type) {
        case VAPictureParameterBufferType:
            require(size == (hevc ? sizeof(VAPictureParameterBufferHEVC) : sizeof(VAPictureParameterBufferH264)) &&
                    count == 1, VA_STATUS_ERROR_INVALID_PARAMETER);
            break;
        case VAIQMatrixBufferType:
            require(size == (hevc ? sizeof(VAIQMatrixBufferHEVC) : sizeof(VAIQMatrixBufferH264)) &&
                    count == 1, VA_STATUS_ERROR_INVALID_PARAMETER);
            break;
        case VASliceParameterBufferType:
            require(size == (hevc ? sizeof(VASliceParameterBufferHEVC) : sizeof(VASliceParameterBufferH264)) &&
                    count <= max_slices, VA_STATUS_ERROR_INVALID_PARAMETER);
            break;
        case VASliceDataBufferType: break;
        default: throw Status{VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE};
        }
        *out = add_buffer(id, type, size, count, data);
        return VA_STATUS_SUCCESS;
    }

    VAStatus set_elements(VABufferID id, unsigned count) {
        auto& b = buffer(id);
        require(!b->mapped && b.use_count() == 1, VA_STATUS_ERROR_SURFACE_BUSY);
        require(b->type == VASliceParameterBufferType && count && count <= b->data.size() / b->size,
                VA_STATUS_ERROR_INVALID_PARAMETER);
        b->count = count;
        return VA_STATUS_SUCCESS;
    }

    VAStatus buffer_info(VABufferID id, VABufferType* type, unsigned* size, unsigned* count) {
        const auto& b = *buffer(id);
        require(type && size && count, VA_STATUS_ERROR_INVALID_PARAMETER);
        *type = b.type;
        *size = b.size;
        *count = b.count;
        return VA_STATUS_SUCCESS;
    }

    VAStatus map_buffer(VABufferID id, void** out) { return map_buffer_access(id, out, 0); }

    VAStatus map_buffer_access(VABufferID id, void** out, uint32_t flags) {
        constexpr uint32_t read = 1, write = 2; // VA_MAPBUFFER_FLAG_READ/WRITE
        auto& b = buffer(id);
        require(out && !(flags & ~(read | write)), VA_STATUS_ERROR_INVALID_PARAMETER);
        require(!b->mapped && b.use_count() == 1, VA_STATUS_ERROR_SURFACE_BUSY);
        if (b->type == VAEncCodedBufferType) {
            require(!(flags & write), VA_STATUS_ERROR_FLAG_NOT_SUPPORTED);
            sync_buffer(id, 0);
            b->coded.size = static_cast<uint32_t>(b->data.size());
            b->coded.buf = b->data.data();
            *out = &b->coded;
        } else if (b->raw) {
            const auto& pixels = *b->raw;
            const auto access = flags == write ? CpuAccess::write : flags == read || (!flags && !pixels.writable)
                ? CpuAccess::read : CpuAccess::read_write;
            require(access == CpuAccess::read || pixels.writable, VA_STATUS_ERROR_FLAG_NOT_SUPPORTED);
            wait_encode_input(pixels.fd.get());
            b->mapping = std::make_unique<CpuView>(pixels.fd.get(), pixels.layout.bytes, access);
            *out = b->mapping->data();
        } else {
            *out = b->data.data();
        }
        b->mapped = true;
        return VA_STATUS_SUCCESS;
    }

    VAStatus unmap_buffer(VABufferID id) {
        auto& b = *buffer(id);
        require(b.mapped, VA_STATUS_ERROR_INVALID_BUFFER);
        b.mapped = false;
        if (b.mapping) {
            auto mapping = std::move(b.mapping);
            mapping->finish();
        }
        return VA_STATUS_SUCCESS;
    }

    VAStatus destroy_buffer(VABufferID id) {
        const auto& b = *buffer(id);
        require(b.type != VAImageBufferType && !b.mapped, VA_STATUS_ERROR_INVALID_BUFFER);
        buffers_.erase(id);
        return VA_STATUS_SUCCESS;
    }

    VAStatus begin(VAContextID id, VASurfaceID target) {
        auto& c = context(id);
        auto& s = surface(target);
        c.check();
        if (c.encoder && s.state == State::queued) sync(target);
        require(c.target == VA_INVALID_ID && s.state != State::collecting && s.state != State::queued,
                VA_STATUS_ERROR_SURFACE_BUSY);
        if (c.encoder) {
            require(s.state != State::failed && (s.raw || s.frame), VA_STATUS_ERROR_INVALID_SURFACE);
            require(s.raw.use_count() <= 1, VA_STATUS_ERROR_SURFACE_BUSY);
            c.source_state = s.state;
        } else {
            require(same_size(c.size, s.size) && s.fourcc == VA_FOURCC_I420 && !s.raw,
                    VA_STATUS_ERROR_INVALID_SURFACE);
            s.frame.reset(); // previously exported FDs own their reference
            s.owner = id;
        }
        c.target = target;
        s.state = State::collecting;
        return VA_STATUS_SUCCESS;
    }

    VAStatus render(VAContextID id, VABufferID* ids, int n) {
        auto& c = context(id);
        const size_t limit = c.encoder ? 16 : 2 + 2 * max_slices;
        require(c.target != VA_INVALID_ID && n > 0 && ids && size_t(n) <= limit - c.submitted.size(),
                VA_STATUS_ERROR_INVALID_PARAMETER);
        for (int i = 0; i < n; ++i) {
            const auto& b = *buffer(ids[i]);
            require(!b.mapped && b.context == id, VA_STATUS_ERROR_INVALID_BUFFER);
            require(b.type != VAEncCodedBufferType, VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
        }
        c.submitted.reserve(c.submitted.size() + n);
        for (int i = 0; i < n; ++i) c.submitted.push_back(buffer(ids[i]));
        return VA_STATUS_SUCCESS;
    }

    VAStatus end(VAContextID id) {
        auto& c = context(id);
        require(c.target != VA_INVALID_ID, VA_STATUS_ERROR_INVALID_CONTEXT);
        const auto target = std::exchange(c.target, VA_INVALID_ID);
        if (c.encoder) {
            auto& s = surface(target);
            s.state = c.source_state; // this is an input, never a reconstructed picture
            auto params = std::move(c.submitted);
            c.submitted.clear();
            auto picture = c.encoder->picture(s.size);
            for (const auto& b : params) picture.add(b->type, b->data.data(), b->data.size());
            picture.finish(c.size, !c.encoder->started());
            if (picture.recon != VA_INVALID_ID) {
                require(picture.recon != target && same_size(surface(picture.recon).size, c.size),
                        VA_STATUS_ERROR_INVALID_SURFACE);
            }
            auto& b = *buffer(picture.coded);
            require(b.type == VAEncCodedBufferType && b.context == id && !b.mapped, VA_STATUS_ERROR_INVALID_BUFFER);
            const auto layout = s.frame ? decoded_pixels(s.frame->layout) : s.raw->layout;
            const auto fd = s.frame ? s.frame->fd.get() : s.raw->fd.get();
            b.data.clear();
            try {
                c.encoder->encode(picture, layout, fd, b.size, b.data);
            } catch (const std::invalid_argument&) {
                b.data.clear();
                throw;
            } catch (const std::bad_alloc&) {
                b.data.clear();
                throw;
            } catch (const std::exception& e) {
                b.data.clear();
                throw EncodingFailure(e.what());
            }
            return VA_STATUS_SUCCESS;
        }
        try {
            c.submit(target, c.submitted);
        } catch (...) {
            c.submitted.clear();
            surface(target).state = State::failed;
            throw;
        }
        c.submitted.clear();
        return VA_STATUS_SUCCESS;
    }

    VAStatus sync_buffer(VABufferID id, uint64_t) {
        const auto& b = *buffer(id);
        require(b.type == VAEncCodedBufferType && !b.data.empty(), VA_STATUS_ERROR_INVALID_BUFFER);
        return VA_STATUS_SUCCESS; // vaEndPicture waits for both queues to return the picture
    }

    VAStatus sync(VASurfaceID id) { return sync_timed(id, sync_limit_ns); }

    VAStatus sync_timed(VASurfaceID id, uint64_t ns) {
        auto& s = surface(id);
        require(s.state != State::collecting, VA_STATUS_ERROR_SURFACE_BUSY);
        if (s.state == State::queued) {
            auto& c = context(s.owner);
            c.check();
            const auto timeout_ms = (std::min(ns, sync_limit_ns) + 999'999) / 1'000'000;
            if (!timeout_ms) {
                g_main_context_iteration(nullptr, FALSE);
                c.check();
            } else {
                bool expired = false;
                Ref<GSource, g_source_unref> timer(g_timeout_source_new(timeout_ms));
                g_source_set_callback(timer.get(), [](void* p) -> gboolean {
                    *static_cast<bool*>(p) = true;
                    return G_SOURCE_REMOVE;
                }, &expired, nullptr);
                g_source_attach(timer.get(), nullptr);
                while (!expired && s.state == State::queued) g_main_context_iteration(nullptr, TRUE);
                g_source_destroy(timer.get());
                c.check();
            }
        }
        require(s.state != State::queued, VA_STATUS_ERROR_TIMEDOUT);
        require(s.state != State::failed, VA_STATUS_ERROR_DECODING_ERROR);
        return VA_STATUS_SUCCESS;
    }

    VAStatus export_surface(VASurfaceID id, uint32_t type, uint32_t flags, void* out) {
        require(out, VA_STATUS_ERROR_INVALID_PARAMETER);
        require(type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
        constexpr uint32_t layers = VA_EXPORT_SURFACE_SEPARATE_LAYERS | VA_EXPORT_SURFACE_COMPOSED_LAYERS;
        constexpr uint32_t access = VA_EXPORT_SURFACE_READ_WRITE;
        require((flags & access) && !(flags & ~(access | layers)) && (flags & layers) != layers,
                VA_STATUS_ERROR_FLAG_NOT_SUPPORTED);
        auto& s = surface(id);
        require(s.state != State::collecting && s.state != State::queued, VA_STATUS_ERROR_SURFACE_BUSY);
        require(s.state != State::failed, VA_STATUS_ERROR_INVALID_SURFACE);
        if (!s.frame) {
            require(!s.decode_only && (s.raw || (flags & VA_EXPORT_SURFACE_WRITE_ONLY)), VA_STATUS_ERROR_INVALID_SURFACE);
            auto& pixels = raw_surface(s);
            require(!(flags & VA_EXPORT_SURFACE_WRITE_ONLY) || pixels.writable, VA_STATUS_ERROR_FLAG_NOT_SUPPORTED);
            require(s.raw.use_count() == 1, VA_STATUS_ERROR_SURFACE_BUSY);
            *static_cast<VADRMPRIMESurfaceDescriptor*>(out) =
                export_encode_surface(pixels, flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS);
            return VA_STATUS_SUCCESS;
        }
        require((flags & access) == VA_EXPORT_SURFACE_READ_ONLY, VA_STATUS_ERROR_FLAG_NOT_SUPPORTED);
        const auto& frame = *s.frame;
        const auto& layout = frame.layout;
        const bool separate = flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS;

        // The backing is native YV12. Describe logical I420 in both modes
        // by pointing to Y, Cb, Cr in that order; no planes are moved.
        VADRMPRIMESurfaceDescriptor desc{};
        desc.fourcc = VA_FOURCC_I420;
        desc.width = layout.size.width;
        desc.height = layout.size.height;
        desc.num_objects = 1;
        desc.num_layers = separate ? 3 : 1;
        for (size_t i = 0; i < layout.planes.size(); ++i) {
            auto& layer = desc.layers[separate ? i : 0];
            const auto plane = separate ? 0 : i;
            layer.drm_format = separate ? DRM_FORMAT_R8 : DRM_FORMAT_YUV420;
            layer.num_planes = separate ? 1 : 3;
            layer.offset[plane] = layout.planes[i].offset;
            layer.pitch[plane] = layout.planes[i].pitch;
        }
        auto fd = duplicate_fd(frame.fd.get());
        desc.objects[0] = {fd.release(), layout.bytes, DRM_FORMAT_MOD_LINEAR};
        *static_cast<VADRMPRIMESurfaceDescriptor*>(out) = desc;
        return VA_STATUS_SUCCESS;
    }

    VAStatus query_surface(VASurfaceID id, VASurfaceStatus* out) {
        require(out, VA_STATUS_ERROR_INVALID_PARAMETER);
        const auto& s = surface(id);
        if (s.state == State::queued) g_main_context_iteration(nullptr, FALSE);
        require(s.state != State::failed, VA_STATUS_ERROR_DECODING_ERROR);
        *out = s.state == State::queued || s.state == State::collecting ? VASurfaceRendering : VASurfaceReady;
        return VA_STATUS_SUCCESS;
    }

    VAStatus query_image_formats(VAImageFormat* out, int* n) {
        require(out && n, VA_STATUS_ERROR_INVALID_PARAMETER);
        std::copy(std::begin(image_formats), std::end(image_formats), out);
        *n = std::size(image_formats);
        return VA_STATUS_SUCCESS;
    }

    VAStatus create_image(VAImageFormat* format, int w, int h, VAImage* out) {
        require(format && out, VA_STATUS_ERROR_INVALID_PARAMETER);
        image_format(format->fourcc);
        require(valid_size(w, h), VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
        const unsigned width = w, height = h;
        const unsigned luma = width * height;
        EncodeLayout layout{{width, height}, format->fourcc, luma * 3 / 2, {}};
        layout.planes[0] = {0, width};
        if (layout.fourcc == VA_FOURCC_NV12) layout.planes[1] = {luma, width};
        else {
            layout.planes[1] = {luma, width / 2};
            layout.planes[2] = {luma * 5 / 4, width / 2};
        }
        *out = add_image(layout);
        return VA_STATUS_SUCCESS;
    }

    VAStatus derive_image(VASurfaceID id, VAImage* out) {
        auto& s = surface(id);
        require(out, VA_STATUS_ERROR_INVALID_PARAMETER);
        require(s.state == State::idle && !s.decode_only &&
                (s.raw || s.encode_hint || s.fourcc != VA_FOURCC_I420), VA_STATUS_ERROR_OPERATION_FAILED);
        const auto& raw = raw_surface(s);
        require(s.raw.use_count() == 1, VA_STATUS_ERROR_SURFACE_BUSY);
        *out = add_image(raw.layout, s.raw);
        return VA_STATUS_SUCCESS;
    }

    VAStatus get_image(VASurfaceID id, int x, int y, unsigned w, unsigned h, VAImageID image_id) {
        const auto& s = surface(id);
        const auto& image = get(images_, image_id, VA_STATUS_ERROR_INVALID_IMAGE);
        auto& b = *buffer(image.buf);
        require(w == image.width && h == image.height, VA_STATUS_ERROR_INVALID_PARAMETER);
        require(!b.mapped && !b.raw && s.raw.use_count() <= 1, VA_STATUS_ERROR_SURFACE_BUSY);
        sync(id);
        require(s.frame || s.raw, VA_STATUS_ERROR_INVALID_SURFACE);
        const auto source = s.frame ? decoded_pixels(s.frame->layout) : s.raw->layout;
        auto cropped = pixel_region(source, x, y, w, h);
        require(compatible_image_formats(image.format.fourcc, cropped.fourcc), VA_STATUS_ERROR_INVALID_IMAGE_FORMAT);
        const int fd = s.frame ? s.frame->fd.get() : s.raw->fd.get();
        if (!s.frame) wait_encode_input(fd);
        CpuView map(fd, source.bytes, false);
        copy_encode_pixels(image_pixels(image), b.data.data(), cropped, map.data());
        map.finish();
        return VA_STATUS_SUCCESS;
    }

    VAStatus put_image(VASurfaceID id, VAImageID image_id, int sx, int sy, unsigned sw, unsigned sh,
                       int dx, int dy, unsigned dw, unsigned dh) {
        auto& s = surface(id);
        const auto& image = get(images_, image_id, VA_STATUS_ERROR_INVALID_IMAGE);
        const auto& b = *buffer(image.buf);
        require(sw == dw && sh == dh, VA_STATUS_ERROR_UNIMPLEMENTED);
        require(!b.raw && !s.decode_only && s.state == State::idle, VA_STATUS_ERROR_INVALID_SURFACE);
        require(!b.mapped && s.raw.use_count() <= 1, VA_STATUS_ERROR_SURFACE_BUSY);
        auto src = pixel_region(image_pixels(image), sx, sy, sw, sh);
        require(compatible_image_formats(s.fourcc, src.fourcc), VA_STATUS_ERROR_INVALID_IMAGE_FORMAT);
        auto& pixels = raw_surface(s);
        require(pixels.writable, VA_STATUS_ERROR_INVALID_SURFACE);
        auto dst = pixel_region(pixels.layout, dx, dy, dw, dh);
        wait_encode_input(pixels.fd.get());
        CpuView map(pixels.fd.get(), pixels.layout.bytes, true);
        copy_encode_pixels(dst, map.data(), src, b.data.data());
        map.finish();
        return VA_STATUS_SUCCESS;
    }

    VAStatus destroy_image(VAImageID id) {
        const auto& image = get(images_, id, VA_STATUS_ERROR_INVALID_IMAGE);
        require(!buffer(image.buf)->mapped, VA_STATUS_ERROR_SURFACE_BUSY);
        buffers_.erase(image.buf);
        images_.erase(id);
        return VA_STATUS_SUCCESS;
    }

private:
    void check_profile(VAProfile p, VAEntrypoint e) const {
        require(supported(p), VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        require(e == VAEntrypointVLD || (e == VAEntrypointEncSlice && can_encode(p)),
                VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT);
    }

    unsigned config_value(VAProfile profile, VAEntrypoint entry, VAConfigAttribType t) const {
        if (entry == VAEntrypointEncSlice && t == VAConfigAttribRateControl) return rates[profile];
        if (entry == VAEntrypointEncSlice && t != VAConfigAttribMaxPictureWidth && t != VAConfigAttribMaxPictureHeight) {
            return encode_attribute(t);
        }
        switch (t) {
        case VAConfigAttribRTFormat: return VA_RT_FORMAT_YUV420;
        case VAConfigAttribDecSliceMode: return VA_DEC_SLICE_MODE_NORMAL;
        case VAConfigAttribMaxPictureWidth: return max_width;
        case VAConfigAttribMaxPictureHeight: return max_height;
        default: return VA_ATTRIB_NOT_SUPPORTED;
        }
    }

    VAGenericID allocate_id() {
        require(next_id_ != VA_INVALID_ID, VA_STATUS_ERROR_MAX_NUM_EXCEEDED);
        return next_id_++;
    }
    Context& context(VAContextID id) { return get(contexts_, id, VA_STATUS_ERROR_INVALID_CONTEXT); }
    Surface& surface(VASurfaceID id) { return get(surfaces_, id, VA_STATUS_ERROR_INVALID_SURFACE); }
    Buf& buffer(VABufferID id) { return get(buffers_, id, VA_STATUS_ERROR_INVALID_BUFFER); }

    EncodeSurface& raw_surface(Surface& s) {
        if (!s.raw) {
            if (heap_.get() < 0) heap_ = open_dma_heap("/dev/dma_heap/system");
            s.raw = std::make_shared<EncodeSurface>(allocate_encode_surface(heap_.get(),
                native_encode_layout(s.size, s.fourcc)));
        }
        return *s.raw;
    }

    VAImage add_image(const EncodeLayout& layout, std::shared_ptr<EncodeSurface> raw = {}) {
        VAImage image{};
        image.image_id = allocate_id();
        image.format = image_format(layout.fourcc);
        image.width = layout.size.width;
        image.height = layout.size.height;
        image.data_size = layout.bytes;
        image.num_planes = encode_planes(layout.fourcc);
        for (unsigned i = 0; i < image.num_planes; ++i) {
            image.pitches[i] = layout.planes[i].pitch;
            image.offsets[i] = layout.planes[i].offset;
        }
        image.buf = add_buffer(VA_INVALID_ID, VAImageBufferType, image.data_size, 1, nullptr, std::move(raw));
        try { images_.emplace(image.image_id, image); }
        catch (...) { buffers_.erase(image.buf); throw; }
        return image;
    }

    VABufferID add_buffer(VAContextID context, VABufferType type, unsigned size, unsigned count,
                          const void* data, std::shared_ptr<EncodeSurface> raw = {}) {
        const auto id = allocate_id();
        const size_t bytes = size_t(size) * count;
        auto b = std::make_shared<Buffer>();
        b->context = context;
        b->type = type;
        b->size = size;
        b->count = count;
        b->raw = std::move(raw);
        if (data) {
            const auto* src = static_cast<const uint8_t*>(data);
            b->data.assign(src, src + bytes);
        } else if (!b->raw && type != VAEncCodedBufferType) {
            b->data.resize(bytes);
        }
        buffers_.emplace(id, std::move(b));
        return id;
    }

    VAGenericID next_id_ = 1;
    size_t decode_contexts_ = 0;
    Fd heap_;
    std::unordered_map<VAConfigID, Config> configs_;
    Surfaces surfaces_; // must outlive Context destruction and callbacks
    std::unordered_map<VAContextID, Context> contexts_;
    std::unordered_map<VABufferID, Buf> buffers_;
    std::unordered_map<VAImageID, VAImage> images_;
};

// Serialize access to driver state. Only a display with a Codec2 decode
// context dispatches Binder callbacks on GLib's default context; a V4L2
// encode display must also work while the application's main loop owns it.
std::mutex va_lock;
thread_local bool entered = false;

class Dispatch {
public:
    Dispatch() : lock_(va_lock, std::defer_lock) {
        if (entered) return;
        lock_.lock();
        entered = true;
    }
    ~Dispatch() {
        if (acquired_) g_main_context_release(g_main_context_default());
        if (lock_.owns_lock()) entered = false;
    }
    bool for_driver(const Driver& driver) {
        if (!lock_.owns_lock()) return false;
        if (!driver.uses_codec2()) return true;
        acquired_ = g_main_context_acquire(g_main_context_default());
        return acquired_;
    }
private:
    std::unique_lock<std::mutex> lock_;
    bool acquired_ = false;
};

template<auto Method, class... Args>
VAStatus hook(VADriverContextP ctx, Args... args) noexcept {
    try {
        Dispatch dispatch;
        auto* d = static_cast<Driver*>(ctx->pDriverData);
        require(d, VA_STATUS_ERROR_INVALID_DISPLAY);
        require(dispatch.for_driver(*d), VA_STATUS_ERROR_HW_BUSY);
        return (d->*Method)(args...);
    } catch (const Status& status) {
        return status.code;
    } catch (const std::bad_alloc&) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    } catch (const EncodingFailure& e) {
        if (ctx->error_callback) ctx->error_callback(ctx, e.what());
        return VA_STATUS_ERROR_ENCODING_ERROR;
    } catch (const std::invalid_argument& e) {
        if (ctx->error_callback) ctx->error_callback(ctx, e.what());
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    } catch (const std::exception& e) {
        if (ctx->error_callback) ctx->error_callback(ctx, e.what());
        return VA_STATUS_ERROR_DECODING_ERROR;
    } catch (...) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
}

VAStatus terminate(VADriverContextP ctx) {
    auto* driver = static_cast<Driver*>(std::exchange(ctx->pDriverData, nullptr));
    Dispatch dispatch;
    if (!driver || dispatch.for_driver(*driver)) delete driver;
    else {
        // libva tears down the vtable even if termination reports an error.
        // Finish on the owning context without retaining the VA display.
        g_idle_add([](void* p) -> gboolean { delete static_cast<Driver*>(p); return G_SOURCE_REMOVE; }, driver);
    }
    return VA_STATUS_SUCCESS;
}

template<class... Args> VAStatus unsupported(VADriverContextP, Args...) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus empty_subpictures(VADriverContextP, VAImageFormat*, unsigned*, unsigned* count) {
    if (!count) return VA_STATUS_ERROR_INVALID_PARAMETER;
    *count = 0;
    return VA_STATUS_SUCCESS;
}
VAStatus empty_display_attrs(VADriverContextP, VADisplayAttribute*, int* count) {
    if (!count) return VA_STATUS_ERROR_INVALID_PARAMETER;
    *count = 0;
    return VA_STATUS_SUCCESS;
}

} // namespace
} // namespace c2::va

#define VA_INIT_NAME_(major, minor) __vaDriverInit_##major##_##minor
#define VA_INIT_NAME(major, minor) VA_INIT_NAME_(major, minor)
extern "C" __attribute__((visibility("default"))) VAStatus VA_INIT_NAME(VA_MAJOR_VERSION, VA_MINOR_VERSION)(
        VADriverContextP ctx) {
    using namespace c2::va;
    const auto display = ctx->display_type & VA_DISPLAY_MAJOR_MASK;
    // Codec2 decode and DMA-BUF export do not depend on the display backend.
    // Loading an X11/Wayland display does not enable native presentation hooks.
    if (display != VA_DISPLAY_DRM && display != VA_DISPLAY_WAYLAND && display != VA_DISPLAY_X11) {
        return VA_STATUS_ERROR_INVALID_DISPLAY;
    }
    Driver* driver;
    try { ctx->pDriverData = driver = new Driver; }
    catch (const std::bad_alloc&) { return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    ctx->version_major = VA_MAJOR_VERSION;
    ctx->version_minor = VA_MINOR_VERSION;
    ctx->max_profiles = std::size(profiles);
    const bool encoding = std::any_of(driver->rates.begin(), driver->rates.end(), [](unsigned r) { return r != 0; });
    ctx->max_entrypoints = encoding ? 2 : 1;
    ctx->max_attributes = max_config_attributes;
    ctx->max_image_formats = std::size(image_formats);
    ctx->max_subpic_formats = 1; // libva needs nonzero storage even for an empty query
    ctx->max_display_attributes = 1;
    ctx->str_vendor = encoding ? "Codec2 MediaTek MT6989 (AVC/HEVC decode, V4L2 encode)" :
        "Codec2 MediaTek MT6989 (AVC/HEVC decode)";
    auto& v = *ctx->vtable;
    v.vaTerminate = terminate;
    v.vaQueryConfigProfiles = hook<&Driver::query_profiles>;
    v.vaQueryConfigEntrypoints = hook<&Driver::query_entrypoints>;
    v.vaGetConfigAttributes = hook<&Driver::get_config_attributes>;
    v.vaCreateConfig = hook<&Driver::create_config>;
    v.vaDestroyConfig = hook<&Driver::destroy_config>;
    v.vaQueryConfigAttributes = hook<&Driver::query_config>;
    v.vaCreateSurfaces = hook<&Driver::create_surfaces_legacy>;
    v.vaCreateSurfaces2 = hook<&Driver::create_surfaces>;
    v.vaDestroySurfaces = hook<&Driver::destroy_surfaces>;
    v.vaQuerySurfaceAttributes = hook<&Driver::query_surface_attributes>;
    v.vaCreateContext = hook<&Driver::create_context>;
    v.vaDestroyContext = hook<&Driver::destroy_context>;
    v.vaCreateBuffer = hook<&Driver::create_buffer>;
    v.vaBufferSetNumElements = hook<&Driver::set_elements>;
    v.vaBufferInfo = hook<&Driver::buffer_info>;
    v.vaMapBuffer = hook<&Driver::map_buffer>;
#if VA_CHECK_VERSION(1, 21, 0)
    v.vaMapBuffer2 = hook<&Driver::map_buffer_access>;
#endif
    v.vaUnmapBuffer = hook<&Driver::unmap_buffer>;
    v.vaDestroyBuffer = hook<&Driver::destroy_buffer>;
    v.vaBeginPicture = hook<&Driver::begin>;
    v.vaRenderPicture = hook<&Driver::render>;
    v.vaEndPicture = hook<&Driver::end>;
    v.vaSyncSurface = hook<&Driver::sync>;
    v.vaSyncSurface2 = hook<&Driver::sync_timed>;
    v.vaSyncBuffer = hook<&Driver::sync_buffer>;
    v.vaQuerySurfaceStatus = hook<&Driver::query_surface>;
    v.vaExportSurfaceHandle = hook<&Driver::export_surface>;
    v.vaQuerySurfaceError = unsupported;
    v.vaPutSurface = unsupported;
    v.vaQueryImageFormats = hook<&Driver::query_image_formats>;
    v.vaCreateImage = hook<&Driver::create_image>;
    v.vaDeriveImage = hook<&Driver::derive_image>;
    v.vaDestroyImage = hook<&Driver::destroy_image>;
    v.vaSetImagePalette = unsupported;
    v.vaGetImage = hook<&Driver::get_image>;
    v.vaPutImage = hook<&Driver::put_image>;
    v.vaQuerySubpictureFormats = empty_subpictures;
    v.vaCreateSubpicture = unsupported;
    v.vaDestroySubpicture = unsupported;
    v.vaSetSubpictureImage = unsupported;
    v.vaSetSubpictureChromakey = unsupported;
    v.vaSetSubpictureGlobalAlpha = unsupported;
    v.vaAssociateSubpicture = unsupported;
    v.vaDeassociateSubpicture = unsupported;
    v.vaQueryDisplayAttributes = empty_display_attrs;
    v.vaGetDisplayAttributes = unsupported;
    v.vaSetDisplayAttributes = unsupported;
    return VA_STATUS_SUCCESS;
}
