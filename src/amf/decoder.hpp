// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "dmabuf.hpp"
#include "store.hpp"

#include <AMF/components/Component.h>
#include <mutex>

namespace c2::amf_bridge {

// libgbinder replies/listeners run on GLib's default context. FFmpeg can
// change threads between ABI calls; keep one dispatcher at a time.
class Dispatch {
public:
    Dispatch();
    ~Dispatch();
    explicit operator bool() const { return acquired_; }

private:
    std::unique_lock<std::mutex> lock_;
    bool acquired_ = false;
};

struct Device {
    Device();
    const Fd heap;
    const Store store;
    const std::string decoder;
};

::amf::AMFComponent* create_av1_decoder(::amf::AMFContext& context, std::shared_ptr<const Device> device);

} // namespace c2::amf_bridge
