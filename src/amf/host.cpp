// SPDX-License-Identifier: Apache-2.0
#include "objects.hpp"
#include "frame.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <optional>

namespace c2::amf_bridge {
namespace {

// Bound the number of pinned HAL allocations, even across decoder teardown.
// Downstream filters can retain arbitrary numbers of CPU frames.
constexpr unsigned max_mapped_surfaces = 8;
std::atomic<unsigned> mapped_surfaces{0};

class MappingSlot {
public:
    explicit MappingSlot(bool eligible) {
        if (!eligible) return;
        auto count = mapped_surfaces.load(std::memory_order_relaxed);
        while (count < max_mapped_surfaces) {
            if (mapped_surfaces.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {
                acquired_ = true;
                break;
            }
        }
    }
    ~MappingSlot() {
        if (acquired_) mapped_surfaces.fetch_sub(1, std::memory_order_relaxed);
    }
    MappingSlot(const MappingSlot&) = delete;
    MappingSlot& operator=(const MappingSlot&) = delete;
    explicit operator bool() const { return acquired_; }

private:
    bool acquired_ = false;
};

bool can_map(const I420Block& frame) {
    if (!frame.standalone) return false;
    const auto& layout = frame.layout;
    // FFmpeg filters can read up to 16 bytes past a plane. Require aligned
    // pointers/strides and space beyond the last row within the mapped FD.
    constexpr unsigned alignment = 16, overread = 16;
    for (size_t i = 0; i < layout.planes.size(); ++i) {
        const auto& p = layout.planes[i];
        const uint64_t rows = layout.size.height >> (i != 0);
        const auto end = p.offset + rows * p.pitch + overread;
        if ((p.offset | p.pitch) % alignment || p.pitch > std::numeric_limits<amf_int32>::max() ||
            end > layout.bytes) return false;
    }
    return true;
}

class Plane final : public AMFPlane {
public:
    Plane(AMFSurface& surface, AMF_PLANE_TYPE type, int width, int height)
        : surface_(surface), type_(type), width_(width), height_(height) {}
    void set_data(const uint8_t* data, amf_int32 pitch) { data_ = data; pitch_ = pitch; }
    amf_long Acquire() override { return surface_.Acquire(); }
    amf_long Release() override { return surface_.Release(); }
    AMF_RESULT QueryInterface(const AMFGuid& iid, void** out) override {
        return query<AMFPlane, AMFInterface>(this, iid, out);
    }
    AMF_PLANE_TYPE GetType() override { return type_; }
    void* GetNative() override { return const_cast<uint8_t*>(data_); } // AMF's read-only AVFrame still uses void*.
    amf_int32 GetPixelSizeInBytes() override { return 1; }
    amf_int32 GetOffsetX() override { return 0; }
    amf_int32 GetOffsetY() override { return 0; }
    amf_int32 GetWidth() override { return width_; }
    amf_int32 GetHeight() override { return height_; }
    amf_int32 GetHPitch() override { return pitch_; }
    amf_int32 GetVPitch() override { return height_; }
    bool IsTiled() override { return false; }

private:
    AMFSurface& surface_;
    AMF_PLANE_TYPE type_;
    const uint8_t* data_ = nullptr;
    amf_int32 pitch_ = 0;
    int width_, height_;
};

class Surface final : public HostData<AMFSurface, Surface> {
public:
    Surface(const I420Block& frame, Timing timing)
        : HostData(timing), slot_(can_map(frame)),
          planes_{{{*this, AMF_PLANE_Y, int(frame.layout.size.width), int(frame.layout.size.height)},
                   {*this, AMF_PLANE_U, int(frame.layout.size.width / 2), int(frame.layout.size.height / 2)},
                   {*this, AMF_PLANE_V, int(frame.layout.size.width / 2), int(frame.layout.size.height / 2)}}} {
        if (slot_) {
            fd_ = duplicate_fd(frame.fd);
            mapping_.emplace(fd_.get(), frame.layout.bytes, false);
            const auto pixels = frame.layout.view(mapping_->data());
            for (size_t i = 0; i < planes_.size(); ++i) {
                const auto& p = pixels.planes[i];
                planes_[i].set_data(p.data, static_cast<amf_int32>(p.stride));
            }
            return;
        }

        const auto& size = frame.layout.size;
        storage_.reset(new uint8_t[size_t(size.width) * size.height * 3 / 2 + padding]);
        auto* dst = storage_.get();
        frame.layout.read(frame.fd, [&](const I420View& pixels) {
            for (size_t i = 0; i < planes_.size(); ++i) {
                auto& plane = planes_[i];
                const size_t width = plane.GetWidth(), height = plane.GetHeight();
                const auto& src = pixels.planes[i];
                plane.set_data(dst, plane.GetWidth());
                if (width == src.stride) std::memcpy(dst, src.data, width * height);
                else for (size_t row = 0; row < height; ++row) {
                    std::memcpy(dst + row * width, src.data + row * src.stride, width);
                }
                dst += width * height;
            }
        });
        std::memset(dst, 0, padding);
    }
    AMF_DATA_TYPE GetDataType() override { return AMF_DATA_SURFACE; }
    AMF_SURFACE_FORMAT GetFormat() override { return AMF_SURFACE_YUV420P; }
    amf_size GetPlanesCount() override { return planes_.size(); }
    AMFPlane* GetPlaneAt(amf_size i) override { return i < planes_.size() ? &planes_[i] : nullptr; }
    AMFPlane* GetPlane(AMF_PLANE_TYPE type) override {
        for (auto& p : planes_) if (p.GetType() == type) return &p;
        return nullptr;
    }
    AMF_FRAME_TYPE GetFrameType() override { return AMF_FRAME_PROGRESSIVE; }
    void SetFrameType(AMF_FRAME_TYPE type) override {
        if (type != AMF_FRAME_PROGRESSIVE) unsupported_void("interlaced SetFrameType");
    }
    AMF_RESULT SetCrop(amf_int32, amf_int32, amf_int32, amf_int32) override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT CopySurfaceRegion(AMFSurface*, amf_int32, amf_int32, amf_int32, amf_int32,
                                 amf_int32, amf_int32) override { return AMF_NOT_SUPPORTED; }
    using HostData::AddObserver;
    using HostData::RemoveObserver;
    void AddObserver(AMFSurfaceObserver*) override { unsupported_void("AddSurfaceObserver"); }
    void RemoveObserver(AMFSurfaceObserver*) override { unsupported_void("RemoveSurfaceObserver"); }

private:
    static constexpr size_t padding = 64;
    // Close ordering: end CPU access and unmap before dropping the FD/slot.
    MappingSlot slot_;
    Fd fd_;
    std::optional<CpuView> mapping_;
    std::unique_ptr<uint8_t[]> storage_;
    std::array<Plane, 3> planes_;
};

} // namespace

Ptr<AMFSurface> make_surface(const I420Block& frame, Timing timing) {
    return Ptr<AMFSurface>(new Surface(frame, timing));
}

} // namespace c2::amf_bridge
