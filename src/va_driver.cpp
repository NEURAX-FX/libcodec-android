// SPDX-License-Identifier: Apache-2.0
#include "component.hpp"
#include "dmabuf.hpp"
#include "frame.hpp"
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
constexpr unsigned min_dim = 64, max_width = 4096, max_height = 2304;
constexpr size_t max_au = 64 * 1024 * 1024, max_slices = 4096;
constexpr uint64_t sync_limit_ns = 8'000'000'000;
constexpr uint8_t start_code[] = {0, 0, 0, 1};

struct Status { VAStatus code; };
void require(bool ok, VAStatus code) { if (!ok) throw Status{code}; }

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
size_t bytes_i420(PictureSize s) { return size_t(s.width) * s.height * 3 / 2; }

void copy_rows(uint8_t* dst, size_t dst_pitch, const uint8_t* src, size_t src_pitch,
               size_t width, size_t height) {
    if (width == dst_pitch && width == src_pitch) {
        std::memcpy(dst, src, width * height);
    } else {
        for (size_t y = 0; y < height; ++y) std::memcpy(dst + y * dst_pitch, src + y * src_pitch, width);
    }
}

struct Buffer {
    VAContextID context;
    VABufferType type;
    unsigned size, count;
    std::vector<uint8_t> data;
    bool mapped = false;
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
};
using Surfaces = std::unordered_map<VASurfaceID, Surface>;

class Context {
public:
    Context(VAContextID id, VAProfile profile, PictureSize size, Surfaces& surfaces)
        : id(id), profile(profile), size(size), surfaces_(surfaces) {}
    ~Context() {
        closing_ = true;
        component_.reset();
        if (target != VA_INVALID_ID) fail(target);
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
        return VA_STATUS_SUCCESS;
    }

    VAStatus get_config_attributes(VAProfile profile, VAEntrypoint entry, VAConfigAttrib* a, int n) {
        check_profile(profile, entry);
        require(n >= 0 && (!n || a), VA_STATUS_ERROR_INVALID_PARAMETER);
        for (int i = 0; i < n; ++i) a[i].value = config_value(a[i].type);
        return VA_STATUS_SUCCESS;
    }

    VAStatus create_config(VAProfile profile, VAEntrypoint entry, VAConfigAttrib* a, int n, VAConfigID* out) {
        check_profile(profile, entry);
        require(out && n >= 0 && (!n || a), VA_STATUS_ERROR_INVALID_PARAMETER);
        for (int i = 0; i < n; ++i) {
            require((a[i].type == VAConfigAttribRTFormat || a[i].type == VAConfigAttribDecSliceMode) &&
                    a[i].value == config_value(a[i].type), VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
        }
        const auto id = allocate_id();
        configs_.emplace(id, profile);
        *out = id;
        return VA_STATUS_SUCCESS;
    }

    VAStatus destroy_config(VAConfigID id) {
        require(configs_.erase(id), VA_STATUS_ERROR_INVALID_CONFIG);
        return VA_STATUS_SUCCESS;
    }

    VAStatus query_config(VAConfigID id, VAProfile* profile, VAEntrypoint* entry, VAConfigAttrib* a, int* n) {
        require(profile && entry && a && n, VA_STATUS_ERROR_INVALID_PARAMETER);
        *profile = get(configs_, id, VA_STATUS_ERROR_INVALID_CONFIG);
        *entry = VAEntrypointVLD;
        a[0] = {VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420};
        a[1] = {VAConfigAttribDecSliceMode, VA_DEC_SLICE_MODE_NORMAL};
        *n = 2;
        return VA_STATUS_SUCCESS;
    }

    VAStatus query_surface_attributes(VAConfigID id, VASurfaceAttrib* a, unsigned* n) {
        get(configs_, id, VA_STATUS_ERROR_INVALID_CONFIG);
        require(n, VA_STATUS_ERROR_INVALID_PARAMETER);
        constexpr unsigned gettable = VA_SURFACE_ATTRIB_GETTABLE;
        constexpr unsigned both = gettable | VA_SURFACE_ATTRIB_SETTABLE;
        constexpr std::pair<VASurfaceAttribType, int> values[] = {
            {VASurfaceAttribPixelFormat, VA_FOURCC_I420}, {VASurfaceAttribMemoryType, VA_SURFACE_ATTRIB_MEM_TYPE_VA},
            {VASurfaceAttribMinWidth, min_dim}, {VASurfaceAttribMinHeight, min_dim},
            {VASurfaceAttribMaxWidth, max_width}, {VASurfaceAttribMaxHeight, max_height},
        };
        if (a && *n < std::size(values)) {
            *n = std::size(values);
            return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
        }
        *n = std::size(values);
        if (a) for (unsigned i = 0; i < *n; ++i) {
            a[i] = {};
            a[i].type = values[i].first;
            a[i].flags = i < 2 ? both : gettable;
            a[i].value.type = VAGenericValueTypeInteger;
            a[i].value.value.i = values[i].second;
        }
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
        for (unsigned i = 0; i < na; ++i) {
            require(a[i].value.type == VAGenericValueTypeInteger, VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
            const auto value = static_cast<unsigned>(a[i].value.value.i);
            switch (a[i].type) {
            case VASurfaceAttribPixelFormat:
                require(value == VA_FOURCC_I420, VA_STATUS_ERROR_INVALID_IMAGE_FORMAT);
                break;
            case VASurfaceAttribMemoryType:
                require(value == VA_SURFACE_ATTRIB_MEM_TYPE_VA, VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
                break;
            case VASurfaceAttribUsageHint:
                require(!(value & ~VA_SURFACE_ATTRIB_USAGE_HINT_DECODER), VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
                break;
            default: throw Status{VA_STATUS_ERROR_ATTR_NOT_SUPPORTED};
            }
        }
        unsigned i = 0;
        try {
            for (; i < n; ++i) {
                const auto id = allocate_id();
                surfaces_.emplace(id, Surface{{w, h}, State::idle, VA_INVALID_ID, {}});
                out[i] = id;
            }
        } catch (...) {
            while (i) surfaces_.erase(out[--i]);
            throw;
        }
        return VA_STATUS_SUCCESS;
    }

    VAStatus destroy_surfaces(VASurfaceID* ids, int n) {
        require(n >= 0 && (!n || ids), VA_STATUS_ERROR_INVALID_PARAMETER);
        for (int i = 0; i < n; ++i) {
            auto& s = surface(ids[i]);
            require(s.state != State::collecting && s.state != State::queued, VA_STATUS_ERROR_SURFACE_BUSY);
        }
        for (int i = 0; i < n; ++i) surfaces_.erase(ids[i]);
        return VA_STATUS_SUCCESS;
    }

    VAStatus create_context(VAConfigID config, int w, int h, int flag, VASurfaceID* targets, int n,
                            VAContextID* out) {
        const auto profile = get(configs_, config, VA_STATUS_ERROR_INVALID_CONFIG);
        require(flag == VA_PROGRESSIVE, VA_STATUS_ERROR_FLAG_NOT_SUPPORTED);
        require(valid_size(w, h) && (profile == VAProfileHEVCMain || !((w | h) & 15)),
                VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
        require(out && n >= 0 && (!n || targets), VA_STATUS_ERROR_INVALID_PARAMETER);
        const PictureSize size{static_cast<unsigned>(w), static_cast<unsigned>(h)};
        for (int i = 0; i < n; ++i) require(same_size(surface(targets[i]).size, size), VA_STATUS_ERROR_INVALID_SURFACE);
        const auto id = allocate_id();
        contexts_.try_emplace(id, id, profile, size, surfaces_);
        *out = id;
        return VA_STATUS_SUCCESS;
    }

    VAStatus destroy_context(VAContextID id) {
        require(contexts_.erase(id), VA_STATUS_ERROR_INVALID_CONTEXT);
        for (auto it = buffers_.begin(); it != buffers_.end();) {
            if (it->second->context == id) it = buffers_.erase(it);
            else ++it;
        }
        return VA_STATUS_SUCCESS;
    }

    VAStatus create_buffer(VAContextID id, VABufferType type, unsigned size, unsigned count, void* data,
                           VABufferID* out) {
        const bool hevc = context(id).profile == VAProfileHEVCMain;
        require(out && size && count && size <= max_au / count, VA_STATUS_ERROR_INVALID_PARAMETER);
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

    VAStatus map_buffer(VABufferID id, void** out) {
        auto& b = buffer(id);
        require(out, VA_STATUS_ERROR_INVALID_PARAMETER);
        require(!b->mapped && b.use_count() == 1, VA_STATUS_ERROR_SURFACE_BUSY);
        *out = b->data.data();
        b->mapped = true;
        return VA_STATUS_SUCCESS;
    }

    VAStatus unmap_buffer(VABufferID id) {
        auto& b = *buffer(id);
        require(b.mapped, VA_STATUS_ERROR_INVALID_BUFFER);
        b.mapped = false;
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
        require(c.target == VA_INVALID_ID && s.state != State::collecting && s.state != State::queued,
                VA_STATUS_ERROR_SURFACE_BUSY);
        require(same_size(c.size, s.size), VA_STATUS_ERROR_INVALID_SURFACE);
        s.frame.reset(); // previously exported FDs own their reference
        c.target = target;
        s.owner = id;
        s.state = State::collecting;
        return VA_STATUS_SUCCESS;
    }

    VAStatus render(VAContextID id, VABufferID* ids, int n) {
        auto& c = context(id);
        const size_t limit = 2 + 2 * max_slices;
        require(c.target != VA_INVALID_ID && n > 0 && ids && size_t(n) <= limit - c.submitted.size(),
                VA_STATUS_ERROR_INVALID_PARAMETER);
        for (int i = 0; i < n; ++i) {
            const auto& b = *buffer(ids[i]);
            require(!b.mapped && b.context == id, VA_STATUS_ERROR_INVALID_BUFFER);
        }
        c.submitted.reserve(c.submitted.size() + n);
        for (int i = 0; i < n; ++i) c.submitted.push_back(buffer(ids[i]));
        return VA_STATUS_SUCCESS;
    }

    VAStatus end(VAContextID id) {
        auto& c = context(id);
        require(c.target != VA_INVALID_ID, VA_STATUS_ERROR_INVALID_CONTEXT);
        const auto target = std::exchange(c.target, VA_INVALID_ID);
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
        require((flags & ~layers) == VA_EXPORT_SURFACE_READ_ONLY && (flags & layers) != layers,
                VA_STATUS_ERROR_FLAG_NOT_SUPPORTED);
        const auto& s = surface(id);
        require(s.state != State::collecting && s.state != State::queued, VA_STATUS_ERROR_SURFACE_BUSY);
        require(s.state == State::ready && s.frame, VA_STATUS_ERROR_INVALID_SURFACE);
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
        *out = i420;
        *n = 1;
        return VA_STATUS_SUCCESS;
    }

    VAStatus create_image(VAImageFormat* format, int w, int h, VAImage* out) {
        require(format && out, VA_STATUS_ERROR_INVALID_PARAMETER);
        require(format->fourcc == i420.fourcc, VA_STATUS_ERROR_INVALID_IMAGE_FORMAT);
        require(valid_size(w, h), VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
        const auto size = PictureSize{static_cast<unsigned>(w), static_cast<unsigned>(h)};
        VAImage image{};
        image.image_id = allocate_id();
        image.format = i420;
        image.width = w;
        image.height = h;
        image.data_size = bytes_i420(size);
        image.num_planes = 3;
        image.pitches[0] = w;
        image.pitches[1] = image.pitches[2] = w / 2;
        image.offsets[1] = w * h;
        image.offsets[2] = image.offsets[1] * 5 / 4;
        image.buf = add_buffer(VA_INVALID_ID, VAImageBufferType, image.data_size, 1, nullptr);
        try { images_.emplace(image.image_id, image); }
        catch (...) { buffers_.erase(image.buf); throw; }
        *out = image;
        return VA_STATUS_SUCCESS;
    }

    VAStatus get_image(VASurfaceID id, int x, int y, unsigned w, unsigned h, VAImageID image_id) {
        const auto& s = surface(id);
        auto& image = get(images_, image_id, VA_STATUS_ERROR_INVALID_IMAGE);
        auto& b = *buffer(image.buf);
        const auto sw = s.size.width, sh = s.size.height;
        require(x >= 0 && y >= 0 && unsigned(x) <= sw && unsigned(y) <= sh && w <= sw - x && h <= sh - y &&
                w == image.width && h == image.height && !((w | h | x | y) & 1), VA_STATUS_ERROR_INVALID_PARAMETER);
        require(!b.mapped, VA_STATUS_ERROR_SURFACE_BUSY);
        sync(id);
        require(s.state == State::ready && s.frame, VA_STATUS_ERROR_INVALID_SURFACE);
        s.frame->layout.read(s.frame->fd.get(), [&](const I420View& view) {
            for (size_t i = 0; i < view.planes.size(); ++i) {
                const unsigned sub = i != 0;
                const auto& p = view.planes[i];
                copy_rows(b.data.data() + image.offsets[i], image.pitches[i],
                          p.data + size_t(y >> sub) * p.stride + (x >> sub), p.stride, w >> sub, h >> sub);
            }
        });
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
    static void check_profile(VAProfile p, VAEntrypoint e) {
        require(supported(p), VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        require(e == VAEntrypointVLD, VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT);
    }

    static unsigned config_value(VAConfigAttribType t) {
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

    VABufferID add_buffer(VAContextID context, VABufferType type, unsigned size, unsigned count, const void* data) {
        const auto id = allocate_id();
        const size_t bytes = size_t(size) * count;
        auto b = std::make_shared<Buffer>(Buffer{context, type, size, count, {}, false});
        if (data) {
            const auto* src = static_cast<const uint8_t*>(data);
            b->data.assign(src, src + bytes);
        } else {
            b->data.resize(bytes);
        }
        buffers_.emplace(id, std::move(b));
        return id;
    }

    VAGenericID next_id_ = 1;
    std::unordered_map<VAConfigID, VAProfile> configs_;
    Surfaces surfaces_; // must outlive Context destruction and callbacks
    std::unordered_map<VAContextID, Context> contexts_;
    std::unordered_map<VABufferID, Buf> buffers_;
    std::unordered_map<VAImageID, VAImage> images_;
};

// libgbinder delivers this client's callbacks on GLib's default context.
// Serialize VA entry points, and own that context for the entire call.
std::mutex va_lock;
thread_local bool entered = false;

class Dispatch {
public:
    Dispatch() : lock_(va_lock, std::defer_lock) {
        if (!entered) {
            lock_.lock();
            acquired_ = g_main_context_acquire(g_main_context_default());
            entered = acquired_;
        }
    }
    ~Dispatch() {
        if (acquired_) {
            entered = false;
            g_main_context_release(g_main_context_default());
        }
    }
    explicit operator bool() const { return acquired_; }
private:
    std::unique_lock<std::mutex> lock_;
    bool acquired_ = false;
};

template<auto Method, class... Args>
VAStatus hook(VADriverContextP ctx, Args... args) noexcept {
    try {
        Dispatch dispatch;
        require(bool(dispatch), VA_STATUS_ERROR_HW_BUSY);
        auto* d = static_cast<Driver*>(ctx->pDriverData);
        require(d, VA_STATUS_ERROR_INVALID_DISPLAY);
        return (d->*Method)(args...);
    } catch (const Status& status) {
        return status.code;
    } catch (const std::bad_alloc&) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
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
    if (dispatch) delete driver;
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
    try { ctx->pDriverData = new Driver; }
    catch (const std::bad_alloc&) { return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    ctx->version_major = VA_MAJOR_VERSION;
    ctx->version_minor = VA_MINOR_VERSION;
    ctx->max_profiles = std::size(profiles);
    ctx->max_entrypoints = 1;
    ctx->max_attributes = 4;
    ctx->max_image_formats = 1;
    ctx->max_subpic_formats = 1; // libva needs nonzero storage even for an empty query
    ctx->max_display_attributes = 1;
    ctx->str_vendor = "Codec2 MediaTek MT6989 (AVC/HEVC decode)";
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
    v.vaUnmapBuffer = hook<&Driver::unmap_buffer>;
    v.vaDestroyBuffer = hook<&Driver::destroy_buffer>;
    v.vaBeginPicture = hook<&Driver::begin>;
    v.vaRenderPicture = hook<&Driver::render>;
    v.vaEndPicture = hook<&Driver::end>;
    v.vaSyncSurface = hook<&Driver::sync>;
    v.vaSyncSurface2 = hook<&Driver::sync_timed>;
    v.vaQuerySurfaceStatus = hook<&Driver::query_surface>;
    v.vaExportSurfaceHandle = hook<&Driver::export_surface>;
    v.vaQuerySurfaceError = unsupported;
    v.vaPutSurface = unsupported;
    v.vaQueryImageFormats = hook<&Driver::query_image_formats>;
    v.vaCreateImage = hook<&Driver::create_image>;
    v.vaDeriveImage = unsupported;
    v.vaDestroyImage = hook<&Driver::destroy_image>;
    v.vaSetImagePalette = unsupported;
    v.vaGetImage = hook<&Driver::get_image>;
    v.vaPutImage = unsupported;
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
