// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "hidl.hpp"

namespace c2 {

// Binary C2Param has size then index, regardless of the prose in types.hal.
struct ParamHeader { uint32_t size, index; };
static_assert(sizeof(ParamHeader) == 8);
struct Param {
    uint32_t index;
    std::vector<uint8_t> payload;
};

inline constexpr uint32_t output_size = 0xd2001800;       // C2StreamPictureSizeInfo::output, stream 0

std::vector<Param> parse_params(const Parcel::Buffer& bytes);
std::vector<uint8_t> pack_params(const std::vector<Param>& params);
Param make_param(uint32_t index, const void* data, size_t size);

template<class T> Param make_param(uint32_t index, const T& value) {
    return make_param(index, &value, sizeof(value));
}
template<class T> T param_value(const Param& param) {
    return Parcel::Buffer{param.payload.data(), param.payload.size(), 0}.at<T>();
}
struct PictureSize { uint32_t width, height; };
PictureSize parse_size(std::string_view value);

} // namespace c2
