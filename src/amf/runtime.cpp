// SPDX-License-Identifier: Apache-2.0
#include "decoder.hpp"
#include "objects.hpp"
#include "av1_packet.hpp"

#include <AMF/components/VideoDecoderUVD.h>
#include <AMF/core/Factory.h>
#include <AMF/core/Version.h>

namespace c2::amf_bridge {
namespace {

class Context final : public Object<AMFContext1, Context, AMFContext> {
public:
    AMF_RESULT Terminate() override {
        return safe_call("Context::Terminate", [&] {
            Dispatch dispatch;
            if (!dispatch) return AMF_WRONG_STATE;
            device_.reset();
            return AMF_OK;
        });
    }

    AMF_RESULT InitVulkan(void* external_device) override {
        if (external_device) return AMF_NOT_SUPPORTED;
        return safe_call("InitVulkan", [&] {
            Dispatch dispatch;
            if (!dispatch) return AMF_WRONG_STATE;
            if (device_) return AMF_ALREADY_INITIALIZED;
            // FFmpeg calls this to open the default device on Linux. Our
            // host-output backend opens the Codec2 device, not a Vulkan device.
            device_ = std::make_shared<Device>();
            return AMF_OK;
        });
    }

    AMF_RESULT AllocBuffer(AMF_MEMORY_TYPE type, amf_size size, AMFBuffer** out) override {
        if (!out) return AMF_INVALID_POINTER;
        *out = nullptr;
        if (!device_) return AMF_NOT_INITIALIZED;
        if (type != AMF_MEMORY_HOST) return AMF_NOT_SUPPORTED;
        constexpr size_t ffmpeg_padding = 64;
        if (size > max_av1_packet + ffmpeg_padding) return AMF_OUT_OF_RANGE;
        return safe_call("AllocBuffer", [&] {
            *out = new HostBuffer(size);
            return AMF_OK;
        });
    }

    const std::shared_ptr<const Device>& device() const { return device_; }

    AMF_RESULT InitDX9(void*) override { return AMF_NOT_SUPPORTED; }
    void* GetDX9Device(AMF_DX_VERSION) override { return nullptr; }
    AMF_RESULT LockDX9() override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT UnlockDX9() override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT InitDX11(void*, AMF_DX_VERSION) override { return AMF_NOT_SUPPORTED; }
    void* GetDX11Device(AMF_DX_VERSION) override { return nullptr; }
    AMF_RESULT LockDX11() override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT UnlockDX11() override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT InitOpenCL(void*) override { return AMF_NOT_SUPPORTED; }
    void* GetOpenCLContext() override { return nullptr; }
    void* GetOpenCLCommandQueue() override { return nullptr; }
    void* GetOpenCLDeviceID() override { return nullptr; }
    AMF_RESULT GetOpenCLComputeFactory(AMFComputeFactory** out) override { return no_interface(out); }
    AMF_RESULT InitOpenCLEx(AMFComputeDevice*) override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT LockOpenCL() override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT UnlockOpenCL() override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT InitOpenGL(amf_handle, amf_handle, amf_handle) override { return AMF_NOT_SUPPORTED; }
    amf_handle GetOpenGLContext() override { return nullptr; }
    amf_handle GetOpenGLDrawable() override { return nullptr; }
    AMF_RESULT LockOpenGL() override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT UnlockOpenGL() override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT InitXV(void*) override { return AMF_NOT_SUPPORTED; }
    void* GetXVDevice() override { return nullptr; }
    AMF_RESULT LockXV() override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT UnlockXV() override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT InitGralloc(void*) override { return AMF_NOT_SUPPORTED; }
    void* GetGrallocDevice() override { return nullptr; }
    AMF_RESULT LockGralloc() override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT UnlockGralloc() override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT AllocSurface(AMF_MEMORY_TYPE, AMF_SURFACE_FORMAT, amf_int32, amf_int32,
                            AMFSurface** out) override { return no_interface(out); }
    AMF_RESULT AllocAudioBuffer(AMF_MEMORY_TYPE, AMF_AUDIO_FORMAT, amf_int32, amf_int32, amf_int32,
                                AMFAudioBuffer** out) override { return no_interface(out); }
    AMF_RESULT CreateBufferFromHostNative(void*, amf_size, AMFBuffer** out,
                                          AMFBufferObserver*) override { return no_interface(out); }
    AMF_RESULT CreateSurfaceFromHostNative(AMF_SURFACE_FORMAT, amf_int32, amf_int32, amf_int32, amf_int32,
                                           void*, AMFSurface** out, AMFSurfaceObserver*) override { return no_interface(out); }
    AMF_RESULT CreateSurfaceFromDX9Native(void*, AMFSurface** out, AMFSurfaceObserver*) override { return no_interface(out); }
    AMF_RESULT CreateSurfaceFromDX11Native(void*, AMFSurface** out, AMFSurfaceObserver*) override { return no_interface(out); }
    AMF_RESULT CreateSurfaceFromOpenGLNative(AMF_SURFACE_FORMAT, amf_handle, AMFSurface** out,
                                             AMFSurfaceObserver*) override { return no_interface(out); }
    AMF_RESULT CreateSurfaceFromGrallocNative(amf_handle, AMFSurface** out,
                                              AMFSurfaceObserver*) override { return no_interface(out); }
    AMF_RESULT CreateSurfaceFromOpenCLNative(AMF_SURFACE_FORMAT, amf_int32, amf_int32, void**, AMFSurface** out,
                                             AMFSurfaceObserver*) override { return no_interface(out); }
    AMF_RESULT CreateBufferFromOpenCLNative(void*, amf_size, AMFBuffer** out) override { return no_interface(out); }
    AMF_RESULT GetCompute(AMF_MEMORY_TYPE, AMFCompute** out) override { return no_interface(out); }
    AMF_RESULT CreateBufferFromDX11Native(void*, AMFBuffer** out, AMFBufferObserver*) override { return no_interface(out); }
    AMF_RESULT AllocBufferEx(AMF_MEMORY_TYPE, amf_size, AMF_BUFFER_USAGE, AMF_MEMORY_CPU_ACCESS,
                              AMFBuffer** out) override { return no_interface(out); }
    AMF_RESULT AllocSurfaceEx(AMF_MEMORY_TYPE, AMF_SURFACE_FORMAT, amf_int32, amf_int32,
                               AMF_SURFACE_USAGE, AMF_MEMORY_CPU_ACCESS, AMFSurface** out) override { return no_interface(out); }
    void* GetVulkanDevice() override { return nullptr; }
    AMF_RESULT LockVulkan() override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT UnlockVulkan() override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT CreateSurfaceFromVulkanNative(void*, AMFSurface** out,
                                             AMFSurfaceObserver*) override { return no_interface(out); }
    AMF_RESULT CreateBufferFromVulkanNative(void*, AMFBuffer** out, AMFBufferObserver*) override { return no_interface(out); }
    AMF_RESULT GetVulkanDeviceExtensions(amf_size*, const char**) override { return AMF_NOT_SUPPORTED; }

private:
    std::shared_ptr<const Device> device_;
};

class Factory final : public AMFFactory {
public:
    AMF_RESULT CreateContext(AMFContext** out) override {
        if (!out) return AMF_INVALID_POINTER;
        *out = nullptr;
        return safe_call("CreateContext", [&] { *out = new Context; return AMF_OK; });
    }
    AMF_RESULT CreateComponent(AMFContext* context, const wchar_t* id, AMFComponent** out) override {
        if (!out) return AMF_INVALID_POINTER;
        *out = nullptr;
        if (!context || !id) return AMF_INVALID_POINTER;
        if (std::wcscmp(id, AMFVideoDecoderHW_AV1)) return AMF_CODEC_NOT_SUPPORTED;
        auto* own = dynamic_cast<Context*>(context);
        if (!own) return AMF_INVALID_ARG;
        if (!own->device()) return AMF_NOT_INITIALIZED;
        return safe_call("CreateComponent", [&] {
            *out = create_av1_decoder(*own, own->device());
            return AMF_OK;
        });
    }
    AMF_RESULT SetCacheFolder(const wchar_t*) override { return AMF_NOT_SUPPORTED; }
    const wchar_t* GetCacheFolder() override { return nullptr; }
    AMF_RESULT GetDebug(AMFDebug** out) override { return no_interface(out); }
    AMF_RESULT GetTrace(AMFTrace** out) override { return no_interface(out); }
    AMF_RESULT GetPrograms(AMFPrograms** out) override { return no_interface(out); }
};

Factory factory;

} // namespace
} // namespace c2::amf_bridge

extern "C" __attribute__((visibility("default")))
AMF_RESULT AMF_CDECL_CALL AMFInit(amf_uint64 version, amf::AMFFactory** out) {
    if (!out) return AMF_INVALID_POINTER;
    *out = nullptr;
    if (AMF_GET_MAJOR_VERSION(version) != AMF_VERSION_MAJOR || version > AMF_FULL_VERSION) {
        std::fprintf(stderr, "Codec2 AMF: client requests SDK %llu.%llu.%llu.%llu, unsupported by this build\n",
                     static_cast<unsigned long long>(AMF_GET_MAJOR_VERSION(version)),
                     static_cast<unsigned long long>(AMF_GET_MINOR_VERSION(version)),
                     static_cast<unsigned long long>(AMF_GET_SUBMINOR_VERSION(version)),
                     static_cast<unsigned long long>(AMF_GET_BUILD_VERSION(version)));
        return AMF_NOT_SUPPORTED;
    }
    *out = &c2::amf_bridge::factory;
    return AMF_OK;
}

extern "C" __attribute__((visibility("default")))
AMF_RESULT AMF_CDECL_CALL AMFQueryVersion(amf_uint64* version) {
    if (!version) return AMF_INVALID_POINTER;
    *version = AMF_FULL_VERSION;
    return AMF_OK;
}
