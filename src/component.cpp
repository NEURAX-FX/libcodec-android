// SPDX-License-Identifier: Apache-2.0
#include "component.hpp"

#include <algorithm>
#include <cerrno>
#include <stdexcept>

namespace c2 {
namespace {

// Declared in order in the corresponding .hal files; inherited methods
// always keep the declaring interface's descriptor and transaction number.
constexpr const char* component_iface = "android.hardware.media.c2@1.0::IComponent";
constexpr const char* intf_iface = "android.hardware.media.c2@1.0::IComponentInterface";
constexpr const char* config_iface = "android.hardware.media.c2@1.0::IConfigurable";
constexpr const char* listener_iface = "android.hardware.media.c2@1.0::IComponentListener";
constexpr unsigned create_codes[] = {1, 9, 10};
constexpr unsigned start_code = 10, stop_code = 11, release_code = 13, interface_code = 14;
constexpr unsigned configurable_code = 1, query_code = 3, config_code = 4, describe_code = 5;
constexpr unsigned work_done = 1, tripped = 2, error = 3, rendered = 4, input_released = 5;
constexpr int32_t bad_state = -38, duplicate = -17; // Codec2 HIDL Status

struct alignas(8) WireDescriptor {
    uint32_t index, flags;
    GBinderHidlString name;
    GBinderHidlVec deps;
};
static_assert(sizeof(WireDescriptor) == 40 && offsetof(WireDescriptor, deps) == 24);

void check_status(int32_t status, std::string_view method) {
    if (status) throw std::runtime_error(std::string(method) + ": Codec2 status " + std::to_string(status));
}

Remote read_object(Parcel& parcel, std::string_view method) {
    Remote object(parcel.object());
    parcel.end();
    if (!object) throw std::runtime_error(std::string(method) + ": null Binder object");
    return object;
}

void expect_interface(GBinderRemoteObject* object, const char* descriptor) {
    const auto types = chain(object);
    if (types.empty() || types.back() != ibase ||
        std::find(types.begin(), types.end(), descriptor) == types.end()) {
        throw std::runtime_error(std::string("unexpected interfaceChain for ") + descriptor);
    }
}

std::vector<Param> read_params(Parcel& parcel) {
    size_t count;
    return parse_params(parcel.vector(1, count));
}

} // namespace

Component::Component(const Store& store, const std::string& name)
    : listener_(gbinder_servicemanager_new_local_object(
          store.manager(), listener_iface, callback, this)) {
    if (!listener_) throw std::runtime_error("cannot construct IComponentListener");
    const auto version = store.version();
    const auto method = version == 2 ? "createComponent_1_2" : version == 1 ? "createComponent_1_1" : "createComponent";
    try {
        auto reply = call(store.remote(), store_ifaces[version], create_codes[version], method,
            [&](GBinderWriter& writer) {
                gbinder_writer_append_hidl_string_copy(&writer, name.c_str());
                gbinder_writer_add_cleanup(&writer, [](gpointer object) {
                    gbinder_local_object_unref(static_cast<GBinderLocalObject*>(object));
                }, gbinder_local_object_ref(listener_.get()));
                gbinder_writer_append_local_object(&writer, listener_.get());
                gbinder_writer_append_local_object(&writer, nullptr); // no client BufferPool
            });
        Parcel parcel(reply.get());
        parcel.status(method);
        check_status(parcel.i32(), method);
        auto component = read_object(parcel, method);
        expect_interface(component.get(), component_iface);
        object_ = std::move(component);
        state_ = State::stopped;
        configure_object();
        check();
    } catch (...) {
        cleanup();
        throw;
    }
}

Component::~Component() { cleanup(); }

void Component::configure_object() {
    auto reply = call(object_.get(), component_iface, interface_code, "IComponent::getInterface");
    Parcel parcel(reply.get());
    parcel.status("IComponent::getInterface");
    auto intf = read_object(parcel, "IComponent::getInterface");
    expect_interface(intf.get(), intf_iface);
    reply = call(intf.get(), intf_iface, configurable_code, "IComponentInterface::getConfigurable");
    Parcel config(reply.get());
    config.status("IComponentInterface::getConfigurable");
    auto configurable = read_object(config, "IComponentInterface::getConfigurable");
    expect_interface(configurable.get(), config_iface);
    configurable_ = std::move(configurable);
}

std::vector<ParamDescriptor> Component::describe() const {
    check();
    auto reply = call(configurable_.get(), config_iface, describe_code, "querySupportedParams",
        [](GBinderWriter& writer) {
            gbinder_writer_append_int32(&writer, 0);
            gbinder_writer_append_int32(&writer, -1); // UINT32_MAX, as in AOSP client.cpp
        });
    Parcel parcel(reply.get());
    parcel.status("querySupportedParams");
    check_status(parcel.i32(), "querySupportedParams");
    size_t count;
    const auto rows = parcel.vector(sizeof(WireDescriptor), count);
    std::vector<ParamDescriptor> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t offset = i * sizeof(WireDescriptor);
        const auto row = rows.at<WireDescriptor>(offset);
        ParamDescriptor desc{row.index, row.flags,
            std::string(parcel.string(rows, offset + offsetof(WireDescriptor, name))), {}};
        size_t deps_count;
        const auto deps = parcel.vector(rows, offset + offsetof(WireDescriptor, deps), sizeof(uint32_t), deps_count);
        desc.dependencies.reserve(deps_count);
        for (size_t j = 0; j < deps_count; ++j) desc.dependencies.push_back(deps.at<uint32_t>(j * sizeof(uint32_t)));
        result.push_back(std::move(desc));
    }
    parcel.end();
    check();
    return result;
}

std::vector<Param> Component::query(const std::vector<uint32_t>& indices) const {
    check();
    if (indices.empty() || indices.size() > G_MAXUINT / sizeof(uint32_t)) {
        throw std::invalid_argument("invalid query index count");
    }
    auto reply = call(configurable_.get(), config_iface, query_code, "query",
        [&](GBinderWriter& writer) {
            gbinder_writer_append_hidl_vec(&writer, indices.data(), indices.size(), sizeof(uint32_t));
            gbinder_writer_append_bool(&writer, FALSE);
        });
    Parcel parcel(reply.get());
    parcel.status("query");
    check_status(parcel.i32(), "query");
    auto values = read_params(parcel);
    parcel.end();
    check();
    return values;
}

std::vector<Param> Component::configure(const std::vector<Param>& params) {
    check();
    if (state_ != State::stopped && state_ != State::running) {
        throw std::logic_error("config requires an active component with known state");
    }
    const auto bytes = pack_params(params);
    if (bytes.empty()) throw std::invalid_argument("configuration is empty");
    const auto previous = state_;
    state_ = State::unknown;
    auto reply = call(configurable_.get(), config_iface, config_code, "config",
        [&](GBinderWriter& writer) {
            gbinder_writer_append_hidl_vec(&writer, bytes.data(), bytes.size(), 1);
            gbinder_writer_append_bool(&writer, FALSE);
        });
    Parcel parcel(reply.get());
    parcel.status("config");
    const auto status = parcel.i32();
    state_ = previous; // Only a delivered Codec2 status confirms the operation returned.
    size_t count;
    const auto failures = parcel.vector(56, count); // 1.0 SettingResult (see types.hal)
    if (count) {
        throw std::runtime_error("config: Codec2 status " + std::to_string(status) + ", " +
                                 std::to_string(count) + " SettingResult(s), first index " +
                                 std::to_string(failures.at<uint32_t>(8)) + ", code " +
                                 std::to_string(failures.at<uint32_t>()));
    }
    check_status(status, "config");
    auto values = read_params(parcel);
    parcel.end();
    check();
    return values;
}

int32_t Component::operation(unsigned code, std::string_view method) {
    // Exceptions from transport, HIDL or decoding the reply do not tell us
    // whether the HAL changed state. Never issue another lifecycle write
    // on that assumption, including from the destructor.
    state_ = State::unknown;
    auto reply = call(object_.get(), component_iface, code, method);
    Parcel parcel(reply.get());
    parcel.status(method);
    const auto status = parcel.i32();
    parcel.end();
    return status;
}

void Component::start() {
    check();
    if (state_ != State::stopped) throw std::logic_error("start requires a stopped component");
    const auto status = operation(start_code, "IComponent::start");
    if (!status) state_ = State::running;
    else if (status != bad_state && status != duplicate) state_ = State::stopped;
    check_status(status, "IComponent::start");
    check();
}

void Component::stop() {
    if (state_ != State::running) throw std::logic_error("stop requires a running component");
    const auto status = operation(stop_code, "IComponent::stop");
    if (status != bad_state && status != duplicate) state_ = State::stopped;
    check_status(status, "IComponent::stop");
}

void Component::release() {
    if (state_ == State::released) return;
    if (state_ != State::stopped) throw std::logic_error("release requires a stopped component");
    const auto status = operation(release_code, "IComponent::release");
    if (status != duplicate) check_status(status, "IComponent::release");
    state_ = State::released;
    configurable_.reset();
    object_.reset();
    listener_.reset();
}

void Component::cleanup() noexcept {
    if (!object_ || gbinder_remote_object_is_dead(object_.get())) return;
    try { if (state_ == State::running) stop(); }
    catch (const std::exception& e) { g_printerr("cleanup: %s\n", e.what()); }
    try { if (state_ == State::stopped) release(); }
    catch (const std::exception& e) { g_printerr("cleanup: %s\n", e.what()); }
}

void Component::check() const {
    if (callback_error_) std::rethrow_exception(callback_error_);
    if (object_ && gbinder_remote_object_is_dead(object_.get())) throw std::runtime_error("Codec2 Binder object died");
}

GBinderLocalReply* Component::callback(GBinderLocalObject*, GBinderRemoteRequest* request,
                                      guint code, guint flags, int* status, void* opaque) noexcept {
    auto& self = *static_cast<Component*>(opaque);
    *status = -EBADMSG;
    if (g_strcmp0(gbinder_remote_request_interface(request), listener_iface) ||
        !(flags & GBINDER_TX_FLAG_ONEWAY)) return nullptr;
    try {
        Parcel parcel(request);
        if (code == error) {
            const auto codec_status = parcel.i32();
            const auto detail = parcel.u32();
            parcel.end();
            throw std::runtime_error("IComponentListener::onError: Codec2 status " +
                                     std::to_string(codec_status) + ", detail " + std::to_string(detail));
        }
        if (code == tripped) throw std::runtime_error("IComponentListener::onTripped: configuration conflict");
        // Lifecycle testing queues no work and sets no surface; no frame
        // consumer is installed. Still acknowledge the known one-way events.
        if (code == work_done || code == input_released || code == rendered) *status = GBINDER_STATUS_OK;
    } catch (...) {
        if (!self.callback_error_) self.callback_error_ = std::current_exception();
    }
    return nullptr;
}

const Traits& choose_decoder(const std::vector<Traits>& traits, std::string_view mime,
                             std::string_view requested) {
    const Traits* best = nullptr;
    for (const auto& trait : traits) {
        if (!hardware_video_decoder(trait) || trait.secure || trait.mime != mime ||
            (!requested.empty() && requested != trait.name)) continue;
        // Android codec XML/traits carry the type; for equal rank, prefer the
        // canonical base flavor (shorter name), then deterministic name order.
        if (!best || trait.rank < best->rank || (trait.rank == best->rank &&
            (trait.name.size() < best->name.size() ||
             (trait.name.size() == best->name.size() && trait.name < best->name)))) best = &trait;
    }
    if (!best) throw std::runtime_error("no matching non-secure hardware decoder for " + std::string(mime));
    return *best;
}

PictureSize configure_size(Component& component, PictureSize requested) {
    component.configure({make_param(output_size, requested)});
    const auto params = component.query({output_size});
    for (const auto& param : params) {
        if (param.index == output_size && param.payload.size() == sizeof(PictureSize)) {
            return param_value<PictureSize>(param);
        }
    }
    throw std::runtime_error("query: raw.size missing from component configuration");
}

} // namespace c2
