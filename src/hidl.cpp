// SPDX-License-Identifier: Apache-2.0
#include "hidl.hpp"

#include <stdexcept>

namespace c2 {

static_assert(sizeof(binder_buffer_object) == 40 && sizeof(flat_binder_object) == 24,
              "This client uses the 64-bit HwBinder ABI");
static_assert(G_BYTE_ORDER == G_LITTLE_ENDIAN, "Codec2 client requires little-endian Linux");

namespace {

template<class T> struct Result {
    T value;
    int status = 0;
    bool done = false;
};

bool wait_for_reply(const bool& done) {
    // libgbinder delivers replies and local-object calls on the default GLib
    // context. These clients are its sole dispatcher.
    bool expired = false;
    const auto timer = g_timeout_add(5000, [](gpointer opaque) -> gboolean {
        *static_cast<bool*>(opaque) = true;
        return G_SOURCE_REMOVE;
    }, &expired);
    while (!done && !expired) g_main_context_iteration(nullptr, TRUE);
    if (!expired) g_source_remove(timer);
    return done;
}

} // namespace

Parcel::Parcel(GBinderRemoteReply* reply) {
    gbinder_remote_reply_init_reader(reply, &reader_);
    init();
}

Parcel::Parcel(GBinderRemoteRequest* request) {
    gbinder_remote_request_init_reader(request, &reader_);
    init();
}

void Parcel::init() {
    data_ = static_cast<const std::byte*>(gbinder_reader_get_data(&reader_, &bytes_));
}

void Parcel::malformed() {
    throw std::runtime_error("malformed HIDL parcel");
}

Parcel::Buffer Parcel::read_buffer(size_t size, const Parent* parent, uint64_t pointer) {
    const auto flat = peek<binder_buffer_object>();
    if (flat.hdr.type != BINDER_TYPE_PTR || flat.length != size ||
        flat.flags != (parent ? static_cast<uint32_t>(BINDER_BUFFER_FLAG_HAS_PARENT) : 0u) ||
        (parent && (flat.parent != parent->index || flat.parent_offset != parent->offset ||
                    flat.buffer != pointer))) {
        malformed();
    }
    // Null HIDL buffers have an inline PTR record but no entry in Android's
    // offsets table (Parcel::writeObject/readObject). The parent is still
    // serialized and checked by the generated reader.
    if (!flat.buffer) {
        if (size || !parent) malformed();
        if (gbinder_reader_skip_buffer(&reader_)) {
            ++index_;
        } else {
            for (size_t i = 0; i < sizeof(flat) / sizeof(uint32_t); ++i) u32();
        }
        return {};
    }
    if (!gbinder_reader_skip_buffer(&reader_)) {
        malformed();
    }
    return {reinterpret_cast<const void*>(flat.buffer), size, index_++};
}

Parcel::Buffer Parcel::buffer(size_t size) {
    return read_buffer(size, nullptr, 0);
}

Parcel::Buffer Parcel::child(const Buffer& parent, size_t offset, size_t size) {
    const Parent at{parent.index, offset};
    return read_buffer(size, &at, parent.at<uint64_t>(offset));
}

Parcel::Buffer Parcel::read_vector(const Buffer& parent, size_t offset,
                                   size_t element_size, size_t& count) {
    count = parent.at<GBinderHidlVec>(offset).count;
    if (!element_size || count > SIZE_MAX / element_size) malformed();
    return child(parent, offset, count * element_size);
}

Parcel::Buffer Parcel::vector(size_t element_size, size_t& count) {
    return read_vector(buffer(sizeof(GBinderHidlVec)), 0, element_size, count);
}

Parcel::Buffer Parcel::vector(const Buffer& parent, size_t offset,
                              size_t element_size, size_t& count) {
    return read_vector(parent, offset, element_size, count);
}

std::string_view Parcel::string(const Buffer& parent, size_t offset) {
    const auto header = parent.at<GBinderHidlString>(offset);
    const auto contents = child(parent, offset, static_cast<size_t>(header.len) + 1);
    const auto* text = static_cast<const char*>(contents.data);
    if (text[header.len] != '\0' || std::memchr(text, '\0', header.len)) malformed();
    return {text, header.len};
}

GBinderRemoteObject* Parcel::object() {
    const auto flat = peek<flat_binder_object>();
    GBinderRemoteObject* object = nullptr;
    if (gbinder_reader_read_nullable_object(&reader_, &object)) {
        ++index_;
        return object;
    }
    if (flat.hdr.type != BINDER_TYPE_BINDER || flat.binder || flat.cookie) malformed();
    for (size_t i = 0; i < sizeof(flat) / sizeof(uint32_t); ++i) u32();
    return nullptr;
}

int32_t Parcel::i32() {
    gint32 value;
    if (!gbinder_reader_read_int32(&reader_, &value)) malformed();
    return value;
}

uint32_t Parcel::u32() {
    guint32 value;
    if (!gbinder_reader_read_uint32(&reader_, &value)) malformed();
    return value;
}

void Parcel::status(std::string_view method) {
    const auto code = i32();
    if (!code) return;
    std::unique_ptr<char, decltype(&g_free)> message(gbinder_reader_read_string16(&reader_), g_free);
    throw std::runtime_error(std::string(method) + ": HIDL exception " + std::to_string(code) +
                             (message && *message ? ": " + std::string(message.get()) : ""));
}

void Parcel::end() const {
    if (!gbinder_reader_at_end(&reader_)) malformed();
}

Reply call(GBinderRemoteObject* object, const char* iface, unsigned code,
           std::string_view method, const std::function<void(GBinderWriter&)>& args) {
    Client client(gbinder_client_new(object, iface));
    if (!client) throw std::runtime_error(std::string(method) + ": cannot create Binder client");
    Request request;
    if (args) {
        request.reset(gbinder_client_new_request(client.get()));
        if (!request) throw std::runtime_error(std::string(method) + ": cannot create Binder request");
        GBinderWriter writer;
        gbinder_local_request_init_writer(request.get(), &writer);
        args(writer);
    }
    Result<Reply> result;
    const auto tx = gbinder_client_transact(client.get(), code, 0, request.get(),
        [](GBinderClient*, GBinderRemoteReply* reply, int status, void* opaque) {
            auto& out = *static_cast<Result<Reply>*>(opaque);
            out.value.reset(gbinder_remote_reply_ref(reply));
            out.status = status;
            out.done = true;
        }, nullptr, &result);
    if (!tx) throw std::runtime_error(std::string(method) + ": Binder object unavailable");

    if (!wait_for_reply(result.done)) {
        // Cancels delivery, not an ioctl already in flight. libgbinder keeps
        // the request (including any writer-managed resources) until finished.
        gbinder_client_cancel(client.get(), tx);
        throw std::runtime_error(std::string(method) + ": Binder reply timed out (5000 ms)");
    }
    if (!result.value || result.status) {
        throw std::runtime_error(std::string(method) + ": Binder transport status " +
                                 std::to_string(result.status) + (!result.value ? " (no reply)" : ""));
    }
    return std::move(result.value);
}

Remote get_service(GBinderServiceManager* manager, const std::string& name, int& status) {
    Result<Remote> result;
    const auto tx = gbinder_servicemanager_get_service(manager, name.c_str(),
        [](GBinderServiceManager*, GBinderRemoteObject* object, int status, void* opaque) {
            auto& out = *static_cast<Result<Remote>*>(opaque);
            out.value.reset(gbinder_remote_object_ref(object));
            out.status = status;
            out.done = true;
        }, &result);
    if (!tx) throw std::runtime_error(name + ": cannot schedule getService");
    if (!wait_for_reply(result.done)) {
        gbinder_servicemanager_cancel(manager, tx);
        throw std::runtime_error(name + ": getService timed out (5000 ms)");
    }
    status = result.status;
    return std::move(result.value);
}

std::vector<std::string> chain(GBinderRemoteObject* object) {
    auto reply = call(object, ibase, interface_chain, "IBase::interfaceChain");
    Parcel parcel(reply.get());
    parcel.status("IBase::interfaceChain");
    size_t count;
    const auto names = parcel.vector(sizeof(GBinderHidlString), count);
    std::vector<std::string> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        result.emplace_back(parcel.string(names, i * sizeof(GBinderHidlString)));
    }
    parcel.end();
    return result;
}

} // namespace c2
