// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <AMF/core/Context.h>

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <limits>
#include <memory>

namespace c2 { struct I420Block; }

namespace c2::amf_bridge {
using namespace ::amf;

inline constexpr wchar_t dts_key[] = L"FFMPEG:dts";
inline constexpr auto no_pts = std::numeric_limits<amf_pts>::min();
struct Timing { amf_pts pts = no_pts, dts = no_pts, duration = 0; };

template<class T> struct ReleaseRef {
    void operator()(T* p) const { p->Release(); }
};
template<class T> using Ptr = std::unique_ptr<T, ReleaseRef<T>>;

template<class Interface> AMF_RESULT no_interface(Interface** out, AMF_RESULT unsupported = AMF_NOT_SUPPORTED) {
    if (!out) return AMF_INVALID_POINTER;
    *out = nullptr;
    return unsupported;
}

inline void unsupported_void(const char* method) {
    std::fprintf(stderr, "Codec2 AMF: %s is not supported\n", method);
}

template<class Fn> AMF_RESULT safe_call(const char* method, Fn&& fn) noexcept {
    try { return fn(); }
    catch (const std::bad_alloc&) { return AMF_OUT_OF_MEMORY; }
    catch (const std::exception& e) { std::fprintf(stderr, "Codec2 AMF %s: %s\n", method, e.what()); }
    catch (...) { std::fprintf(stderr, "Codec2 AMF %s: unknown error\n", method); }
    return AMF_FAIL;
}

template<class... T, class Object>
AMF_RESULT query(Object* object, const AMFGuid& iid, void** out) {
    if (!out) return AMF_INVALID_POINTER;
    *out = nullptr;
    if (!((iid == T::IID() ? (*out = static_cast<T*>(object), true) : false) || ...)) return AMF_NO_INTERFACE;
    object->Acquire();
    return AMF_OK;
}

// The AMD SDK interfaces define the virtual slot order used by FFmpeg's C API.
// Do not add a virtual destructor; Release destroys the concrete type.
template<class Api, class Derived, class... Bases>
class Object : public Api {
public:
    amf_long Acquire() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    amf_long Release() override {
        const auto left = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!left) delete static_cast<Derived*>(this);
        return left;
    }
    AMF_RESULT QueryInterface(const AMFGuid& iid, void** out) override {
        return query<Api, AMFInterface, AMFPropertyStorage, Bases...>(this, iid, out);
    }
    AMF_RESULT SetProperty(const wchar_t*, AMFVariantStruct) override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT GetProperty(const wchar_t*, AMFVariantStruct*) const override { return AMF_NOT_FOUND; }
    amf_bool HasProperty(const wchar_t* name) const override {
        AMFVariant value;
        return this->GetProperty(name, &value) == AMF_OK;
    }
    amf_size GetPropertyCount() const override { return 0; }
    AMF_RESULT GetPropertyAt(amf_size, wchar_t*, amf_size, AMFVariantStruct*) const override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT Clear() override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT AddTo(AMFPropertyStorage*, amf_bool, amf_bool) const override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT CopyTo(AMFPropertyStorage*, amf_bool) const override { return AMF_NOT_SUPPORTED; }
    void AddObserver(AMFPropertyStorageObserver*) override { unsupported_void("AddPropertyObserver"); }
    void RemoveObserver(AMFPropertyStorageObserver*) override { unsupported_void("RemovePropertyObserver"); }

private:
    std::atomic<amf_long> refs_{1};
};

template<class Api, class Derived>
class HostData : public Object<Api, Derived, AMFData> {
public:
    explicit HostData(Timing timing = {}) : timing_(timing) {}
    AMF_MEMORY_TYPE GetMemoryType() override { return AMF_MEMORY_HOST; }
    AMF_RESULT Duplicate(AMF_MEMORY_TYPE, AMFData** out) override { return no_interface(out); }
    AMF_RESULT Convert(AMF_MEMORY_TYPE type) override { return type == AMF_MEMORY_HOST ? AMF_OK : AMF_NOT_SUPPORTED; }
    AMF_RESULT Interop(AMF_MEMORY_TYPE type) override { return Convert(type); }
    amf_bool IsReusable() override { return false; }
    void SetPts(amf_pts pts) override { timing_.pts = pts; }
    amf_pts GetPts() override { return timing_.pts; }
    void SetDuration(amf_pts duration) override { timing_.duration = duration; }
    amf_pts GetDuration() override { return timing_.duration; }
    AMF_RESULT SetProperty(const wchar_t* name, AMFVariantStruct value) override {
        if (!name) return AMF_INVALID_POINTER;
        if (std::wcscmp(name, dts_key)) return AMF_NOT_SUPPORTED;
        if (value.type != AMF_VARIANT_INT64) return AMF_INVALID_ARG;
        timing_.dts = value.int64Value;
        return AMF_OK;
    }
    AMF_RESULT GetProperty(const wchar_t* name, AMFVariantStruct* value) const override {
        if (!name || !value) return AMF_INVALID_POINTER;
        if (std::wcscmp(name, dts_key)) return AMF_NOT_FOUND;
        return AMFVariantAssignInt64(value, timing_.dts);
    }
    amf_size GetPropertyCount() const override { return 1; }
    AMF_RESULT GetPropertyAt(amf_size i, wchar_t* name, amf_size size, AMFVariantStruct* value) const override {
        if (i || size < sizeof(dts_key) / sizeof(dts_key[0])) return AMF_OUT_OF_RANGE;
        if (!name || !value) return AMF_INVALID_POINTER;
        std::wmemcpy(name, dts_key, sizeof(dts_key) / sizeof(dts_key[0]));
        return AMFVariantAssignInt64(value, timing_.dts);
    }
    AMF_RESULT Clear() override { timing_.dts = no_pts; return AMF_OK; }

private:
    Timing timing_;
};

class HostBuffer final : public HostData<AMFBuffer, HostBuffer> {
public:
    explicit HostBuffer(amf_size capacity) : storage_(new uint8_t[capacity]), capacity_(capacity), size_(capacity) {}
    AMF_DATA_TYPE GetDataType() override { return AMF_DATA_BUFFER; }
    AMF_RESULT SetSize(amf_size size) override {
        if (size > capacity_) return AMF_OUT_OF_RANGE;
        size_ = size;
        return AMF_OK;
    }
    amf_size GetSize() override { return size_; }
    void* GetNative() override { return storage_.get(); }
    using HostData::AddObserver;
    using HostData::RemoveObserver;
    void AddObserver(AMFBufferObserver*) override { unsupported_void("AddBufferObserver"); }
    void RemoveObserver(AMFBufferObserver*) override { unsupported_void("RemoveBufferObserver"); }

private:
    std::unique_ptr<uint8_t[]> storage_;
    amf_size capacity_, size_;
};

Ptr<AMFSurface> make_surface(const I420Block& frame, Timing timing);

} // namespace c2::amf_bridge
