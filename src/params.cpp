// SPDX-License-Identifier: Apache-2.0
#include "params.hpp"

#include <charconv>
#include <limits>
#include <stdexcept>

namespace c2 {
namespace {

size_t padded(size_t size) { return (size + 7u) & ~size_t{7}; }

} // namespace

std::vector<Param> parse_params(const Parcel::Buffer& bytes) {
    std::vector<Param> params;
    const auto* base = static_cast<const uint8_t*>(bytes.data);
    for (size_t offset = 0; offset < bytes.size;) {
        const auto header = bytes.at<ParamHeader>(offset);
        if (header.size < sizeof(header) || padded(header.size) > bytes.size - offset) {
            throw std::runtime_error("invalid C2Param size/alignment");
        }
        const auto* payload = base + offset + sizeof(header);
        params.push_back(make_param(header.index, payload, header.size - sizeof(header)));
        offset += padded(header.size);
    }
    return params;
}

std::vector<uint8_t> pack_params(const std::vector<Param>& params) {
    constexpr auto max_size = std::numeric_limits<uint32_t>::max();
    size_t size = 0;
    for (const auto& param : params) {
        if (param.payload.size() > max_size - sizeof(ParamHeader)) throw std::length_error("C2Param too large");
        size += padded(sizeof(ParamHeader) + param.payload.size());
        if (size > max_size) throw std::length_error("C2Params blob too large");
    }
    std::vector<uint8_t> result(size);
    size_t offset = 0;
    for (const auto& param : params) {
        const ParamHeader header{static_cast<uint32_t>(sizeof(ParamHeader) + param.payload.size()), param.index};
        std::memcpy(result.data() + offset, &header, sizeof(header));
        if (!param.payload.empty()) {
            std::memcpy(result.data() + offset + sizeof(header), param.payload.data(), param.payload.size());
        }
        offset += padded(header.size);
    }
    return result;
}

Param make_param(uint32_t index, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    return {index, size ? std::vector<uint8_t>(bytes, bytes + size) : std::vector<uint8_t>()};
}

PictureSize parse_size(std::string_view value) {
    const auto sep = value.find('x');
    PictureSize size{};
    if (sep == value.npos || !sep || sep + 1 == value.size()) {
        throw std::invalid_argument("expected a nonzero WIDTHxHEIGHT");
    }
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto w = std::from_chars(begin, begin + sep, size.width);
    const auto h = std::from_chars(begin + sep + 1, end, size.height);
    if (w.ec != std::errc{} || w.ptr != begin + sep || h.ec != std::errc{} || h.ptr != end ||
        !size.width || !size.height) throw std::invalid_argument("expected a nonzero WIDTHxHEIGHT");
    return size;
}

} // namespace c2
