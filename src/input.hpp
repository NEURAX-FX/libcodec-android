// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "params.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace c2 {

struct Packet {
    const uint8_t* data;
    size_t size;
    uint64_t timestamp_us;
};

class Input {
public:
    // A null format probes the file. Force raw Annex B with "h264" or "hevc",
    // or raw low-overhead AV1 with "obu".
    explicit Input(const char* path, const char* format = nullptr);
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    std::string_view mime() const;
    PictureSize size() const;
    // Returns false only at EOF. The packet data is borrowed until the next call.
    bool next(Packet& packet);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace c2
