// SPDX-License-Identifier: Apache-2.0
#include "av1_packet.hpp"

#include <cstring>
#include <stdexcept>

namespace c2 {
namespace {

enum class ObuType : unsigned {
    sequence_header = 1,
    temporal_delimiter = 2,
    frame_header = 3,
    tile_group = 4,
    frame = 6,
    redundant_frame_header = 7,
    tile_list = 8,
};

struct Obu { ObuType type; size_t size; };

Obu read_obu(const uint8_t* data, size_t size, bool require_size_field) {
    if (size && !(data[0] & 0x81)) { // Forbidden and reserved bits.
        const auto type = static_cast<ObuType>((data[0] >> 3) & 15);
        const size_t header = data[0] & 4 ? 2 : 1;
        if (header <= size && (header == 1 || !(data[1] & 7))) {
            // A container sample's last OBU may use the sample boundary.
            // Prefix OBUs need explicit sizes so they can precede other OBUs.
            if (!(data[0] & 2)) {
                if (!require_size_field && type != ObuType::temporal_delimiter &&
                    type != ObuType::sequence_header) return {type, size};
            } else {
                uint64_t payload = 0;
                for (size_t i = header; i < size && i - header < 8; ++i) {
                    const auto byte = data[i];
                    payload |= static_cast<uint64_t>(byte & 0x7f) << (7 * (i - header));
                    if (byte & 0x80) continue;
                    if (payload <= size - i - 1 &&
                        (type != ObuType::temporal_delimiter || !payload) &&
                        (type != ObuType::sequence_header || payload)) {
                        return {type, i + 1 + static_cast<size_t>(payload)};
                    }
                    break;
                }
            }
        }
    }
    throw std::runtime_error("invalid AV1 OBU framing");
}

} // namespace

Av1AccessUnit prepare_av1_packet(const uint8_t* data, size_t size,
                                 const uint8_t* extra, size_t extra_size, bool& sequence_seen) {
    if (!data || !size || size > max_av1_packet) throw std::runtime_error("invalid AV1 packet size");
    size_t after_delimiter = 0, after_sequence = 0;
    bool before_frame = true;
    for (size_t offset = 0; offset < size;) {
        const auto obu = read_obu(data + offset, size - offset, false);
        switch (obu.type) {
        case ObuType::temporal_delimiter:
            if (offset) throw std::runtime_error("AV1 packet contains multiple temporal units");
            after_delimiter = obu.size;
            break;
        case ObuType::sequence_header:
            if (before_frame) after_sequence = offset + obu.size;
            break;
        case ObuType::frame_header:
        case ObuType::tile_group:
        case ObuType::frame:
        case ObuType::redundant_frame_header:
        case ObuType::tile_list:
            before_frame = false;
            break;
        default: break;
        }
        offset += obu.size;
    }

    if (extra_size && !extra) throw std::runtime_error("missing AV1 configuration data");
    // MOV and Matroska may supply av1C rather than raw config OBUs.
    if (extra_size && (extra[0] & 0x80)) {
        constexpr size_t record_size = 4;
        if (extra[0] != 0x81 || extra_size < record_size) {
            throw std::runtime_error("invalid AV1 configuration record");
        }
        extra += record_size;
        extra_size -= record_size;
    }
    if (extra_size > max_av1_packet) throw std::runtime_error("AV1 configuration exceeds packet limit");

    // Prefer the in-band sequence header, but retain the other config OBUs.
    size_t added = 0;
    bool has_sequence = after_sequence != 0;
    for (size_t offset = 0; offset < extra_size;) {
        const auto obu = read_obu(extra + offset, extra_size - offset, true);
        if (obu.type == ObuType::temporal_delimiter) {
            throw std::runtime_error("AV1 configuration contains a temporal delimiter");
        }
        const bool sequence = obu.type == ObuType::sequence_header;
        has_sequence |= sequence;
        if (!after_sequence || !sequence) added += obu.size;
        offset += obu.size;
    }
    if (!before_frame && !sequence_seen && !has_sequence) {
        throw std::runtime_error("missing AV1 sequence header before frame data");
    }
    if (added > max_av1_packet - size) throw std::runtime_error("video configuration exceeds packet limit");
    sequence_seen |= has_sequence;
    return {data, size, after_sequence ? after_sequence : after_delimiter, extra, extra_size, added};
}

void Av1AccessUnit::copy_to(uint8_t* dst) const {
    if (!added_) {
        std::memcpy(dst, data_, size_);
        return;
    }
    std::memcpy(dst, data_, split_);
    dst += split_;
    if (added_ == config_size_) {
        std::memcpy(dst, config_, added_);
        dst += added_;
    } else {
        for (size_t offset = 0; offset < config_size_;) {
            const auto obu = read_obu(config_ + offset, config_size_ - offset, true);
            if (obu.type != ObuType::sequence_header) {
                std::memcpy(dst, config_ + offset, obu.size);
                dst += obu.size;
            }
            offset += obu.size;
        }
    }
    std::memcpy(dst, data_ + split_, size_ - split_);
}

} // namespace c2
