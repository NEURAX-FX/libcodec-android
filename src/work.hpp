// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "params.hpp"

namespace c2 {

inline constexpr uint32_t flag_drop = 1, flag_eos = 2, flag_discard = 4,
                          flag_incomplete = 8, flag_codec_config = 0x80000000;

struct InputWork {
    uint64_t index, timestamp_us;
    uint32_t flags;
    int fd;
    uint32_t capacity, length;
};

struct Crop { uint32_t left, top, width, height; };

// All handle data is borrowed from the onWorkDone transaction. Copy or use
// within the callback; do not retain the pointers or FD numbers.
struct OutputBlock {
    Crop crop;
    Parcel::Handle handle, fence;
};
struct WorkResult {
    uint64_t index, timestamp_us;
    uint32_t flags;
    std::vector<OutputBlock> blocks;
};
using WorkDone = std::function<void(const WorkResult&)>;

void write_work(GBinderWriter& writer, const InputWork& input);
void read_work_done(Parcel& parcel, const WorkDone& consumer);

} // namespace c2
