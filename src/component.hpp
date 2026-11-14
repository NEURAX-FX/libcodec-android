// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "work.hpp"
#include "store.hpp"

#include <exception>

namespace c2 {

struct ParamDescriptor {
    uint32_t index, flags;
    std::string name;
    std::vector<uint32_t> dependencies;
};

// Internal lifecycle client. Call from the sole dispatcher of GLib's default
// context; the Store/device must remain alive for the life of the component.
class Component {
public:
    Component(const Store& store, const std::string& name, WorkDone consumer = {});
    ~Component();
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;

    std::vector<ParamDescriptor> describe() const;
    std::vector<Param> query(const std::vector<uint32_t>& indices) const;
    std::vector<Param> configure(const std::vector<Param>& params);
    void start();
    void queue(const InputWork& input);
    void stop();
    void release();
    void check() const;

private:
    static GBinderLocalReply* callback(GBinderLocalObject*, GBinderRemoteRequest*, guint,
                                       guint, int*, void*) noexcept;
    void configure_object();
    enum class State { unknown, stopped, running, released };
    int32_t operation(unsigned code, std::string_view method);
    void cleanup() noexcept;

    std::exception_ptr callback_error_;
    WorkDone consumer_;
    Ref<GBinderLocalObject, gbinder_local_object_drop> listener_;
    Remote object_;
    Remote configurable_;
    State state_ = State::unknown;
};

const Traits& choose_decoder(const std::vector<Traits>& traits, std::string_view mime,
                             std::string_view requested = {});
PictureSize configure_size(Component& component, PictureSize requested);
void configure_readback(Component& component, PictureSize size);

} // namespace c2
