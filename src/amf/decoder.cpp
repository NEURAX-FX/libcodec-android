// SPDX-License-Identifier: Apache-2.0
#include "decoder.hpp"
#include "objects.hpp"
#include "av1_packet.hpp"
#include "component.hpp"
#include "frame.hpp"

#include <AMF/components/VideoDecoderUVD.h>

#include <array>
#include <stdexcept>

namespace c2::amf_bridge {
namespace {

std::mutex dispatch_lock;
thread_local bool dispatching = false;

std::string find_decoder(const Store& store) {
    auto traits = store.list();
    classify(traits, "default", vendor_xml);
    return choose_decoder(traits, "video/av01", "c2.mtk.av1.decoder").name;
}

class Session {
public:
    Session(std::shared_ptr<const Device> device, PictureSize size)
        : device_(std::move(device)), size_(size),
          component_(device_->store, device_->decoder, [this](const WorkResult& work) { done(work); }) {
        configure_readback(component_, size_);
        component_.start();
    }

    ~Session() { closing_ = true; } // Component is destroyed before the callback state.

    AMF_RESULT submit(AMFBuffer& buffer, AMFBuffer* config) {
        if (draining_) return AMF_WRONG_STATE;
        if (ready_count_ == ready_.size()) return AMF_INPUT_FULL;
        const auto size = buffer.GetSize();
        if (!size || size > max_av1_packet) return AMF_INVALID_ARG;
        const auto data = static_cast<const uint8_t*>(buffer.GetNative());
        const auto extra = first_ && config ? static_cast<const uint8_t*>(config->GetNative()) : nullptr;
        const auto extra_size = first_ && config ? config->GetSize() : 0;
        const auto unit = prepare_av1_packet(data, size, extra, extra_size, sequence_seen_);
        AMFVariant dts;
        const Timing timing{buffer.GetPts(), buffer.GetProperty(dts_key, &dts) == AMF_OK &&
                            dts.type == AMF_VARIANT_INT64 ? dts.int64Value : no_pts, buffer.GetDuration()};
        auto& work = next_slot();
        uint32_t capacity;
        const auto dma = upload_packet(device_->heap.get(), unit.size(), capacity,
                                       [&](uint8_t* dst) { unit.copy_to(dst); });
        queue(work, timing, 0, dma.get(), capacity, static_cast<uint32_t>(unit.size()));
        first_ = false;
        return AMF_OK;
    }

    AMF_RESULT drain() {
        if (!draining_) {
            auto& work = next_slot();
            draining_ = true;
            queue(work, {}, flag_eos, -1, 0, 0);
        }
        return AMF_OK;
    }

    AMF_RESULT output(AMFData** out) {
        component_.check();
        const auto ready = [&] { return ready_count_ || (eos_ && !pending_count_); };
        if (draining_) {
            // Stock FFmpeg must not see EAGAIN after it has sent EOS.
            wait(ready);
        } else {
            for (unsigned n = 0; n < 8 && !ready() && g_main_context_iteration(nullptr, FALSE); ++n) {
                component_.check();
            }
        }
        if (!ready_count_) return eos_ && !pending_count_ ? AMF_EOF : AMF_REPEAT;
        *out = ready_[ready_head_].release();
        ready_head_ = (ready_head_ + 1) % ready_.size();
        --ready_count_;
        return AMF_OK;
    }

private:
    static constexpr size_t slots = 32;
    struct Pending {
        uint64_t index = 0;
        Timing timing;
        bool active = false, end = false;
    };

    template<class Ready> void wait(Ready ready) {
        component_.check();
        if (ready()) return;
        bool expired = false;
        const c2::Ref<GSource, g_source_unref> timer(g_timeout_source_new(8000));
        g_source_set_callback(timer.get(), [](gpointer arg) -> gboolean {
            *static_cast<bool*>(arg) = true;
            return G_SOURCE_REMOVE;
        }, &expired, nullptr);
        g_source_attach(timer.get(), nullptr);
        try {
            while (!ready() && !expired) {
                g_main_context_iteration(nullptr, TRUE);
                component_.check();
            }
        } catch (...) {
            g_source_destroy(timer.get());
            throw;
        }
        g_source_destroy(timer.get());
        if (!ready()) throw std::runtime_error("timed out waiting for Codec2 output");
    }

    Pending& next_slot() {
        if (next_ == UINT64_MAX) throw std::runtime_error("Codec2 work index overflow");
        auto& work = pending_[next_ % slots];
        wait([&] { return !work.active; });
        return work;
    }

    void queue(Pending& work, Timing timing, uint32_t flags, int fd, uint32_t capacity, uint32_t length) {
        const auto index = next_++;
        work = {index, timing, true, flags == flag_eos};
        ++pending_count_; // Binder can dispatch onWorkDone before returning the queue reply.
        // Work index carries identity. Keep signed FFmpeg PTS/DTS in the work
        // slot, without interpreting AMF values as Codec2 microseconds.
        component_.queue({index, index + 1, flags, fd, capacity, length});
    }

    void done(const WorkResult& result) {
        if (closing_) return;
        auto& work = pending_[result.index % slots];
        if (!work.active || work.index != result.index || result.blocks.size() > 1) {
            throw std::runtime_error("unexpected Codec2 AV1 completion");
        }
        if (!result.blocks.empty()) {
            if (work.end || ready_count_ == ready_.size()) {
                throw std::runtime_error("Codec2 returned an unassociated frame or filled the output queue");
            }
            const auto tail = (ready_head_ + ready_count_) % ready_.size();
            ready_[tail] = make_surface(reader_.describe(result.blocks.front(), size_), work.timing);
            ++ready_count_;
        }
        if (!(result.flags & flag_incomplete)) {
            work.active = false;
            --pending_count_;
        }
        if (result.flags & flag_eos) {
            if (!draining_ || eos_) throw std::runtime_error("unexpected Codec2 EOS");
            eos_ = true;
        }
    }

    const std::shared_ptr<const Device> device_;
    const PictureSize size_;
    FrameReader reader_;
    std::array<Pending, slots> pending_{};
    std::array<Ptr<AMFSurface>, slots> ready_{};
    uint64_t next_ = 0;
    size_t pending_count_ = 0, ready_count_ = 0, ready_head_ = 0;
    bool first_ = true, sequence_seen_ = false, draining_ = false, eos_ = false, closing_ = false;
    Component component_;
};

class Decoder final : public Object<AMFComponent, Decoder, AMFPropertyStorageEx> {
public:
    Decoder(AMFContext& context, std::shared_ptr<const Device> device)
        : device_(std::move(device)), context_(&context) { context.Acquire(); }

    ~Decoder() {
        if (!session_) return;
        Dispatch dispatch;
        if (dispatch) session_.reset();
        else {
            // Release has no way to report a busy GLib context. Destroy on
            // that context instead, keeping the listener state alive until then.
            g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
                delete static_cast<Session*>(p);
                return G_SOURCE_REMOVE;
            }, session_.release(), nullptr);
        }
    }

    AMF_RESULT SetProperty(const wchar_t* name, AMFVariantStruct value) override {
        if (!name) return AMF_INVALID_POINTER;
        if (std::wcscmp(name, AMF_VIDEO_DECODER_EXTRADATA)) return AMF_NOT_SUPPORTED;
        if (value.type != AMF_VARIANT_INTERFACE) return AMF_INVALID_ARG;
        if (size_.width) return AMF_WRONG_STATE;
        void* buffer = nullptr;
        if (value.pInterface) {
            auto result = value.pInterface->QueryInterface(AMFBuffer::IID(), &buffer);
            if (result != AMF_OK) return result;
            if (!buffer) return AMF_INVALID_POINTER;
        }
        config_.reset(static_cast<AMFBuffer*>(buffer));
        return AMF_OK;
    }

    AMF_RESULT GetProperty(const wchar_t* name, AMFVariantStruct* value) const override {
        if (!name || !value) return AMF_INVALID_POINTER;
        if (!std::wcscmp(name, AMF_VIDEO_DECODER_OUTPUT_FORMAT)) return AMFVariantAssignInt64(value, AMF_SURFACE_YUV420P);
        if (!std::wcscmp(name, AMF_VIDEO_DECODER_CURRENT_SIZE)) {
            const AMFSize size{static_cast<amf_int32>(size_.width), static_cast<amf_int32>(size_.height)};
            return AMFVariantAssignSize(value, &size);
        }
        return AMF_NOT_FOUND;
    }
    amf_size GetPropertiesInfoCount() const override { return 0; }
    AMF_RESULT GetPropertyInfo(amf_size, const AMFPropertyInfo** out) const override { return no_interface(out); }
    AMF_RESULT GetPropertyInfo(const wchar_t*, const AMFPropertyInfo** out) const override { return no_interface(out); }
    AMF_RESULT ValidateProperty(const wchar_t*, AMFVariantStruct, AMFVariantStruct*) const override { return AMF_NOT_SUPPORTED; }

    AMF_RESULT Init(AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height) override {
        if (size_.width) return AMF_ALREADY_INITIALIZED;
        return ReInitWithFormat(format, width, height);
    }
    AMF_RESULT ReInit(amf_int32 width, amf_int32 height) override {
        return ReInitWithFormat(AMF_SURFACE_YUV420P, width, height);
    }
    AMF_RESULT Terminate() override {
        return safe_call("Terminate", [&] {
            Dispatch dispatch;
            if (!dispatch) return AMF_WRONG_STATE;
            session_.reset();
            size_ = {};
            failed_ = false;
            return AMF_OK;
        });
    }
    AMF_RESULT Flush() override {
        return safe_call("Flush", [&] {
            Dispatch dispatch;
            if (!dispatch) return AMF_WRONG_STATE;
            if (!size_.width) return AMF_NOT_INITIALIZED;
            // Drop the HAL component so delayed work cannot reach the new stream.
            session_.reset();
            failed_ = false;
            return AMF_OK;
        });
    }
    AMF_RESULT Drain() override {
        return with_session("Drain", [](Session& session) { return session.drain(); });
    }
    AMF_RESULT SubmitInput(AMFData* data) override {
        if (!data) return AMF_INVALID_POINTER;
        void* ptr = nullptr;
        const auto result = data->QueryInterface(AMFBuffer::IID(), &ptr);
        if (result != AMF_OK) return result;
        const Ptr<AMFBuffer> buffer(static_cast<AMFBuffer*>(ptr));
        if (!buffer) return AMF_INVALID_POINTER;
        if (buffer->GetMemoryType() != AMF_MEMORY_HOST) return AMF_NOT_SUPPORTED;
        return with_session("SubmitInput", [&](Session& session) { return session.submit(*buffer, config_.get()); });
    }
    AMF_RESULT QueryOutput(AMFData** out) override {
        if (!out) return AMF_INVALID_POINTER;
        *out = nullptr;
        return with_session("QueryOutput", [&](Session& session) { return session.output(out); });
    }
    AMFContext* GetContext() override { return context_.get(); }
    AMF_RESULT SetOutputDataAllocatorCB(AMFDataAllocatorCB*) override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT GetCaps(AMFCaps** out) override { return no_interface(out); }
    AMF_RESULT Optimize(AMFComponentOptimizationCallback*) override { return AMF_NOT_SUPPORTED; }

private:
    AMF_RESULT ReInitWithFormat(AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height) {
        if (format != AMF_SURFACE_UNKNOWN && format != AMF_SURFACE_YUV420P) return AMF_SURFACE_FORMAT_NOT_SUPPORTED;
        if (width < 16 || height < 16 || width > 8192 || height > 4320 || ((width | height) & 1)) {
            return AMF_INVALID_RESOLUTION;
        }
        return safe_call("Init", [&] {
            Dispatch dispatch;
            if (!dispatch) return AMF_WRONG_STATE;
            session_.reset();
            size_ = {};
            const PictureSize size{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
            session_ = std::make_unique<Session>(device_, size);
            size_ = size;
            failed_ = false;
            return AMF_OK;
        });
    }

    template<class Fn> AMF_RESULT with_session(const char* method, Fn fn) {
        return safe_call(method, [&] {
            Dispatch dispatch;
            if (!dispatch) return AMF_WRONG_STATE;
            if (!size_.width) return AMF_NOT_INITIALIZED;
            if (failed_) return AMF_FAIL;
            try {
                if (!session_) session_ = std::make_unique<Session>(device_, size_);
                return fn(*session_);
            } catch (...) {
                failed_ = true;
                throw;
            }
        });
    }

    const std::shared_ptr<const Device> device_;
    const Ptr<AMFContext> context_;
    Ptr<AMFBuffer> config_;
    std::unique_ptr<Session> session_;
    PictureSize size_{};
    bool failed_ = false;
};

} // namespace

Dispatch::Dispatch() : lock_(dispatch_lock, std::defer_lock) {
    if (!dispatching) {
        lock_.lock();
        acquired_ = g_main_context_acquire(g_main_context_default());
        dispatching = acquired_;
    }
}

Dispatch::~Dispatch() {
    if (acquired_) {
        dispatching = false;
        g_main_context_release(g_main_context_default());
    }
}

Device::Device() : heap(open_dma_heap("/dev/dma_heap/system")),
                   store(GBINDER_DEFAULT_HWBINDER, "default"), decoder(find_decoder(store)) {}

AMFComponent* create_av1_decoder(AMFContext& context, std::shared_ptr<const Device> device) {
    return new Decoder(context, std::move(device));
}

} // namespace c2::amf_bridge
