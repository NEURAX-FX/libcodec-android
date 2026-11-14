// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <gbinder.h>
#include <linux/android/binder.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace c2 {

inline constexpr char ibase[] = "android.hidl.base@1.0::IBase";
inline constexpr unsigned interface_chain = GBINDER_FOURCC(0x0f, 'C', 'H', 'N');

// libgbinder does not expose the parent index of a buffer via GBinderBuffer.
// Keep the public kernel object layout at this boundary; use libgbinder's
// reader to check that every non-null pointer really is a Binder object.
class Parcel {
public:
    struct Buffer {
        const void* data = nullptr;
        size_t size = 0;
        size_t index = 0;

        template<class T> T at(size_t offset = 0) const {
            if (offset > size || sizeof(T) > size - offset) malformed();
            T value;
            std::memcpy(&value, static_cast<const std::byte*>(data) + offset, sizeof(T));
            return value;
        }
    };
    struct Parent {
        size_t index;
        size_t offset;
    };
    struct Handle {
        Buffer data;
        uint32_t fd_count = 0, int_count = 0;
        int fd(size_t i) const;
        uint32_t word(size_t i) const;
        explicit operator bool() const { return data.data != nullptr; }
    };

    explicit Parcel(GBinderRemoteReply* reply);
    explicit Parcel(GBinderRemoteRequest* request);
    Buffer buffer(size_t size);
    Buffer child(const Buffer& parent, size_t offset, size_t size);
    Buffer vector(size_t element_size, size_t& count);
    Buffer vector(const Buffer& parent, size_t offset, size_t element_size, size_t& count);
    std::string_view string(const Buffer& parent, size_t offset);
    Handle handle(const Buffer& parent, size_t offset);
    GBinderRemoteObject* object(); // owned; may be null
    int32_t i32();
    uint32_t u32();
    void status(std::string_view method);
    uint64_t u64();
    void end() const;

private:
    [[noreturn]] static void malformed();
    void init();
    Buffer read_buffer(size_t size, const Parent* parent, uint64_t pointer);
    Buffer read_vector(const Buffer& parent, size_t offset, size_t element_size, size_t& count);
    template<class T> T peek() const {
        const auto remaining = gbinder_reader_bytes_remaining(&reader_);
        if (remaining > bytes_ || sizeof(T) > remaining) malformed();
        T value;
        std::memcpy(&value, data_ + (bytes_ - remaining), sizeof(T));
        return value;
    }
    GBinderReader reader_;
    size_t bytes_ = 0;
    const std::byte* data_ = nullptr;
    size_t index_ = 0;
};

template<class T, void (*unref)(T*)> struct Unref {
    void operator()(T* value) const { unref(value); }
};
template<class T, void (*unref)(T*)> using Ref = std::unique_ptr<T, Unref<T, unref>>;
using Manager = Ref<GBinderServiceManager, gbinder_servicemanager_unref>;
using Remote = Ref<GBinderRemoteObject, gbinder_remote_object_unref>;
using Client = Ref<GBinderClient, gbinder_client_unref>;
using Request = Ref<GBinderLocalRequest, gbinder_local_request_unref>;
using Reply = Ref<GBinderRemoteReply, gbinder_remote_reply_unref>;

Reply call(GBinderRemoteObject* object, const char* iface, unsigned code,
           std::string_view method, const std::function<void(GBinderWriter&)>& args = {});
Remote get_service(GBinderServiceManager* manager, const std::string& name, int& status);
std::vector<std::string> chain(GBinderRemoteObject* object);

} // namespace c2
