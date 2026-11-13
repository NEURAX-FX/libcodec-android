// SPDX-License-Identifier: Apache-2.0
#include "store.hpp"

#include <cerrno>
#include <stdexcept>

namespace c2 {
namespace {

// IComponentStore.hal + hidl-gen; also verified on the MT6989 generated proxy.
constexpr unsigned list_components = GBINDER_FIRST_CALL_TRANSACTION + 2;

struct alignas(8) WireTraits {
    GBinderHidlString name;
    uint32_t domain, kind, rank;
    GBinderHidlString mime;
    GBinderHidlVec aliases;
};
static_assert(sizeof(WireTraits) == 64 && offsetof(WireTraits, domain) == 16 &&
              offsetof(WireTraits, mime) == 32 && offsetof(WireTraits, aliases) == 48);

} // namespace

Store::Store(const char* device, std::string instance)
    : manager_(gbinder_servicemanager_new2(device, "hidl", "hidl")) {
    if (!manager_) throw std::runtime_error(std::string("cannot open HwBinder manager on ") + device);
    for (int v = static_cast<int>(store_ifaces.size()) - 1; v >= 0; --v) {
        service_ = std::string(store_ifaces[v]) + '/' + instance;
        int status = 0;
        auto object = get_service(manager_.get(), service_, status);
        if (status && status != -ENOENT) {
            throw std::runtime_error(service_ + ": getService transport status " + std::to_string(status));
        }
        if (!object) continue;
        object_ = std::move(object);
        interfaces_ = chain(object_.get());
        for (unsigned minor = store_ifaces.size(); minor-- > static_cast<unsigned>(v);) {
            // HIDL lists the most derived descriptor first, then every ancestor.
            // A vendor extension may precede these descriptors.
            if (interfaces_.size() < minor + 2 || interfaces_.back() != ibase) continue;
            bool match = true;
            for (unsigned n = 0; n <= minor; ++n) {
                if (interfaces_[interfaces_.size() - n - 2] != store_ifaces[n]) match = false;
            }
            if (match) {
                version_ = minor;
                return;
            }
        }
        throw std::runtime_error(service_ + ": unexpected IComponentStore interfaceChain");
    }
    throw std::runtime_error("no HIDL Codec2 IComponentStore/" + instance + " on " + device);
}

std::vector<Traits> Store::list() const {
    auto reply = call(object_.get(), store_ifaces[0], list_components, "listComponents");
    Parcel parcel(reply.get());
    parcel.status("listComponents");
    const auto status = parcel.i32();
    if (status) throw std::runtime_error("listComponents: Codec2 status " + std::to_string(status));

    size_t count;
    const auto rows = parcel.vector(sizeof(WireTraits), count);
    std::vector<Traits> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto offset = i * sizeof(WireTraits);
        const auto wire = rows.at<WireTraits>(offset);
        Traits trait{std::string(parcel.string(rows, offset + offsetof(WireTraits, name))),
                     static_cast<Domain>(wire.domain), static_cast<Kind>(wire.kind), wire.rank,
                     std::string(parcel.string(rows, offset + offsetof(WireTraits, mime))), {}};
        size_t aliases_count;
        const auto aliases = parcel.vector(rows, offset + offsetof(WireTraits, aliases),
                                           sizeof(GBinderHidlString), aliases_count);
        trait.aliases.reserve(aliases_count);
        for (size_t n = 0; n < aliases_count; ++n) {
            trait.aliases.emplace_back(parcel.string(aliases, n * sizeof(GBinderHidlString)));
        }
        result.push_back(std::move(trait));
    }
    parcel.end();
    return result;
}

bool hardware_video_decoder(const Traits& trait) {
    return trait.domain == Domain::video && trait.kind == Kind::decoder &&
           (trait.mime == "video/avc" || trait.mime == "video/hevc") &&
           trait.acceleration == Acceleration::hardware && !trait.disabled;
}

const char* to_string(Domain domain) {
    switch (domain) {
    case Domain::other: return "other";
    case Domain::video: return "video";
    case Domain::audio: return "audio";
    case Domain::image: return "image";
    }
    return "unknown";
}

const char* to_string(Kind kind) {
    switch (kind) {
    case Kind::other: return "other";
    case Kind::decoder: return "decoder";
    case Kind::encoder: return "encoder";
    }
    return "unknown";
}

const char* to_string(Acceleration acceleration) {
    switch (acceleration) {
    case Acceleration::software: return "software";
    case Acceleration::hardware: return "hardware";
    default: return "unknown";
    }
}

} // namespace c2
