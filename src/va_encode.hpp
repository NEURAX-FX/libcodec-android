// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "va_encode_pixels.hpp"
#include "v4l2_encoder.hpp"

#include <optional>

namespace c2::va {

unsigned encode_attribute(VAConfigAttribType type);
bool encode_buffer_shape(VAProfile profile, VABufferType type, unsigned size, unsigned count);

// The V4L2 encoder owns the reference pictures and writes its own headers.
// Translate the VA stream settings, picture type and output buffer selection.
class EncodePicture {
public:
    EncodePicture(VAProfile profile, const EncodeSettings& previous, PictureSize input);
    void add(VABufferType type, const uint8_t* data, size_t size);
    void finish(PictureSize context, bool first);

    EncodeSettings settings;
    VABufferID coded = VA_INVALID_ID;
    VASurfaceID recon = VA_INVALID_ID;
    bool keyframe = false;
    int qp = -1; // CQP only; otherwise leave quantization to the firmware

private:
    void sequence(const VAEncSequenceParameterBufferH264&);
    void sequence(const VAEncSequenceParameterBufferHEVC&);
    void misc(const uint8_t* data, size_t size);

    VAProfile profile_;
    PictureSize input_;
    bool have_sequence_ = false, have_slice_ = false;
    bool intra_slice_ = false, intra_picture_ = false;
    int pic_qp_ = 0, slice_delta_ = 0;
    unsigned rate_ = 0;
    unsigned fps_num_ = 0, fps_den_ = 0;
};

class VaEncoder {
public:
    VaEncoder(VAProfile profile, unsigned rate_mode);
    EncodePicture picture(PictureSize input) const { return {profile_, settings_, input}; }
    bool started() const { return device_.has_value(); }
    void encode(const EncodePicture&, const EncodeLayout& input, int fd,
                size_t limit, std::vector<uint8_t>& coded);

private:
    VAProfile profile_;
    EncodeSettings settings_;
    std::optional<V4l2Encoder> device_;
    std::optional<EncodeSurface> staging_;
    bool failed_ = false;
};

} // namespace c2::va
