// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace c2 {

inline constexpr size_t max_av1_packet = 64 * 1024 * 1024;

// Borrows a low-overhead AV1 sample and any configuration OBUs to insert.
class Av1AccessUnit {
public:
    size_t size() const { return size_ + added_; }
    void copy_to(uint8_t* dst) const;

private:
    friend Av1AccessUnit prepare_av1_packet(const uint8_t*, size_t, const uint8_t*, size_t, bool&);
    Av1AccessUnit(const uint8_t* data, size_t size, size_t split,
                  const uint8_t* config, size_t config_size, size_t added)
        : data_(data), size_(size), split_(split), config_(config), config_size_(config_size), added_(added) {}

    const uint8_t* data_;
    size_t size_, split_;
    const uint8_t* config_;
    size_t config_size_, added_;
};

Av1AccessUnit prepare_av1_packet(const uint8_t* data, size_t size,
                                 const uint8_t* extra, size_t extra_size, bool& sequence_seen);

} // namespace c2
