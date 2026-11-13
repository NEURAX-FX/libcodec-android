// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "hidl.hpp"

#include <array>

namespace c2 {

inline constexpr std::array store_ifaces = {
    "android.hardware.media.c2@1.0::IComponentStore",
    "android.hardware.media.c2@1.1::IComponentStore",
    "android.hardware.media.c2@1.2::IComponentStore",
};
inline constexpr char vendor_xml[] = "/vendor/etc/media_codecs_c2.xml";

enum class Domain : uint32_t { other, video, audio, image };
enum class Kind : uint32_t { other, decoder, encoder };
enum class Acceleration { unknown, software, hardware };

struct Traits {
    std::string name;
    Domain domain;
    Kind kind;
    uint32_t rank;
    std::string mime;
    std::vector<std::string> aliases;
    Acceleration acceleration = Acceleration::unknown;
    bool secure = false;
    bool disabled = false;
};

class Store {
public:
    Store(const char* device, std::string instance);
    std::vector<Traits> list() const;
    GBinderServiceManager* manager() const { return manager_.get(); }
    GBinderRemoteObject* remote() const { return object_.get(); }
    unsigned version() const { return version_; }
    const std::string& service() const { return service_; }
    const std::vector<std::string>& interfaces() const { return interfaces_; }

private:
    Manager manager_;
    Remote object_;
    unsigned version_ = 0;
    std::string service_;
    std::vector<std::string> interfaces_;
};

// Store/XML hardware classification based on Codec2InfoBuilder. Missing
// vendor metadata leaves acceleration unknown.
void classify(std::vector<Traits>& traits, std::string_view instance, const char* xml_path);
bool hardware_video_decoder(const Traits& trait);
const char* to_string(Domain domain);
const char* to_string(Kind kind);
const char* to_string(Acceleration acceleration);

} // namespace c2
