// SPDX-License-Identifier: Apache-2.0
#include "va_encode.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace c2::va {
namespace {

void validate(bool valid, const char* what) {
    if (!valid) throw std::invalid_argument(what);
}

bool same_size(PictureSize a, PictureSize b) {
    return a.width == b.width && a.height == b.height;
}

template<class T> T param(const uint8_t* data, size_t size) {
    validate(size == sizeof(T), "unexpected VA encode parameter size");
    T value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

uint32_t idr_period(uint32_t intra, uint32_t idr) {
    const auto gop = idr ? idr : intra;
    validate(gop <= 65535, "unsupported encoder GOP length");
    return gop ? gop : 120;
}

} // namespace

unsigned encode_attribute(VAConfigAttribType type) {
    switch (type) {
    case VAConfigAttribRTFormat: return VA_RT_FORMAT_YUV420;
    case VAConfigAttribEncMaxRefFrames: return 1; // one past picture, no future references
    case VAConfigAttribPredictionDirection: return VA_PREDICTION_DIRECTION_PREVIOUS;
    case VAConfigAttribEncMaxSlices: return 1;
    case VAConfigAttribEncPackedHeaders: return 0;
    case VAConfigAttribEncTileSupport: return 0;
    default: return VA_ATTRIB_NOT_SUPPORTED;
    }
}

bool encode_buffer_shape(VAProfile profile, VABufferType type, unsigned size, unsigned count) {
    if (count != 1) return false;
    const bool hevc = profile == VAProfileHEVCMain;
    switch (type) {
    case VAEncSequenceParameterBufferType:
        return size == (hevc ? sizeof(VAEncSequenceParameterBufferHEVC) : sizeof(VAEncSequenceParameterBufferH264));
    case VAEncPictureParameterBufferType:
        return size == (hevc ? sizeof(VAEncPictureParameterBufferHEVC) : sizeof(VAEncPictureParameterBufferH264));
    case VAEncSliceParameterBufferType:
        return size == (hevc ? sizeof(VAEncSliceParameterBufferHEVC) : sizeof(VAEncSliceParameterBufferH264));
    case VAEncMiscParameterBufferType:
        return size > sizeof(VAEncMiscParameterType) && size <= 4096;
    case VAEncCodedBufferType: return true;
    default: return false;
    }
}

EncodePicture::EncodePicture(VAProfile profile, const EncodeSettings& previous, PictureSize input)
    : settings(previous), profile_(profile), input_(input) {}

void EncodePicture::sequence(const VAEncSequenceParameterBufferH264& p) {
    validate(p.seq_fields.bits.chroma_format_idc == 1 && p.seq_fields.bits.frame_mbs_only_flag &&
             !p.bit_depth_luma_minus8 && !p.bit_depth_chroma_minus8 && p.ip_period <= 1,
             "H.264 encoding requires progressive 8-bit 4:2:0 without B frames");
    const auto full_width = 16u * p.picture_width_in_mbs;
    const auto full_height = 16u * p.picture_height_in_mbs;
    const auto right = p.frame_cropping_flag ? uint64_t(p.frame_crop_right_offset) * 2 : 0;
    const auto bottom = p.frame_cropping_flag ? uint64_t(p.frame_crop_bottom_offset) * 2 : 0;
    validate((!p.frame_cropping_flag || (!p.frame_crop_left_offset && !p.frame_crop_top_offset)) &&
             right < full_width && bottom < full_height, "unsupported H.264 sequence crop");
    settings.size = {static_cast<uint32_t>(full_width - right), static_cast<uint32_t>(full_height - bottom)};
    settings.level = p.level_idc;
    settings.gop = idr_period(p.intra_period, p.intra_idr_period);
    if (p.bits_per_second) settings.bitrate = p.bits_per_second;
}

void EncodePicture::sequence(const VAEncSequenceParameterBufferHEVC& p) {
    validate(p.general_profile_idc == 1 && p.seq_fields.bits.chroma_format_idc == 1 &&
             !p.seq_fields.bits.bit_depth_luma_minus8 && !p.seq_fields.bits.bit_depth_chroma_minus8 &&
             !p.seq_fields.bits.separate_colour_plane_flag && p.ip_period <= 1,
             "HEVC encoding requires Main 8-bit 4:2:0 without B frames");
    // VA's HEVC sequence buffer has coded dimensions, but no conformance
    // window. Use the input surface's visible size, not padded references.
    validate(p.pic_width_in_luma_samples >= input_.width && p.pic_width_in_luma_samples - input_.width < 16 &&
             p.pic_height_in_luma_samples >= input_.height && p.pic_height_in_luma_samples - input_.height < 16,
             "HEVC sequence dimensions disagree with the input surface");
    settings.size = input_;
    settings.level = p.general_level_idc;
    settings.high_tier = p.general_tier_flag != 0;
    settings.gop = idr_period(p.intra_period, p.intra_idr_period);
    if (p.bits_per_second) settings.bitrate = p.bits_per_second;
}

void EncodePicture::misc(const uint8_t* data, size_t size) {
    constexpr auto header = sizeof(VAEncMiscParameterType);
    validate(size >= header, "short VA encode misc parameter");
    switch (param<VAEncMiscParameterType>(data, header)) {
    case VAEncMiscParameterTypeRateControl: {
        const auto p = param<VAEncMiscParameterRateControl>(data + header, size - header);
        validate(settings.rate_mode != VA_RC_CQP && !p.rc_flags.bits.temporal_id && p.bits_per_second && p.target_percentage <= 100,
                 "invalid VA encode bitrate or temporal layer");
        const auto target = settings.rate_mode == VA_RC_CBR ? 100u : p.target_percentage ? p.target_percentage : 100u;
        rate_ = static_cast<unsigned>(uint64_t(p.bits_per_second) * target / 100);
        validate(rate_ != 0, "invalid VA encode target bitrate");
        break;
    }
    case VAEncMiscParameterTypeFrameRate: {
        const auto p = param<VAEncMiscParameterFrameRate>(data + header, size - header);
        fps_num_ = p.framerate & 65535u;
        fps_den_ = p.framerate >> 16;
        if (!fps_den_) fps_den_ = 1;
        validate(fps_num_ && !p.framerate_flags.bits.temporal_id, "invalid VA encode framerate or temporal layer");
        break;
    }
    case VAEncMiscParameterTypeHRD: {
        // Clients submit a CPB description with CBR/VBR. The MTK firmware
        // derives its own buffer and HRD data from the selected rate mode.
        const auto p = param<VAEncMiscParameterHRD>(data + header, size - header);
        validate(p.initial_buffer_fullness <= p.buffer_size, "invalid VA encode CPB size");
        break;
    }
    default: throw std::invalid_argument("unsupported VA encode misc parameter");
    }
}

void EncodePicture::add(VABufferType type, const uint8_t* data, size_t size) {
    const bool hevc = profile_ == VAProfileHEVCMain;
    switch (type) {
    case VAEncSequenceParameterBufferType:
        validate(!have_sequence_, "duplicate VA sequence parameter");
        if (hevc) sequence(param<VAEncSequenceParameterBufferHEVC>(data, size));
        else sequence(param<VAEncSequenceParameterBufferH264>(data, size));
        have_sequence_ = true;
        break;
    case VAEncPictureParameterBufferType:
        validate(coded == VA_INVALID_ID, "duplicate VA picture parameter");
        if (hevc) {
            const auto p = param<VAEncPictureParameterBufferHEVC>(data, size);
            validate((p.pic_fields.bits.coding_type == 1 || p.pic_fields.bits.coding_type == 2) &&
                     !p.pic_fields.bits.tiles_enabled_flag && !p.pic_fields.bits.dependent_slice_segments_enabled_flag,
                     "unsupported HEVC picture structure");
            intra_picture_ = p.pic_fields.bits.coding_type == 1;
            keyframe = p.pic_fields.bits.idr_pic_flag;
            pic_qp_ = p.pic_init_qp;
            coded = p.coded_buf;
            recon = p.decoded_curr_pic.picture_id;
        } else {
            const auto p = param<VAEncPictureParameterBufferH264>(data, size);
            validate(!(p.CurrPic.flags & (VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD)),
                     "interlaced H.264 encoding is unsupported");
            keyframe = p.pic_fields.bits.idr_pic_flag;
            pic_qp_ = p.pic_init_qp;
            coded = p.coded_buf;
            recon = p.CurrPic.picture_id;
        }
        validate(coded != VA_INVALID_ID, "missing VA coded buffer ID");
        break;
    case VAEncSliceParameterBufferType:
        validate(!have_slice_, "multiple VA encode slices are unsupported");
        if (hevc) {
            const auto p = param<VAEncSliceParameterBufferHEVC>(data, size);
            validate(!p.slice_segment_address && (p.slice_type == 1 || p.slice_type == 2), "HEVC supports only whole I/P pictures");
            intra_slice_ = p.slice_type == 2;
            slice_delta_ = p.slice_qp_delta;
        } else {
            const auto p = param<VAEncSliceParameterBufferH264>(data, size);
            validate(!p.macroblock_address && (p.slice_type == 0 || p.slice_type == 2 || p.slice_type == 5 || p.slice_type == 7),
                     "H.264 supports only whole I/P pictures");
            intra_slice_ = p.slice_type % 5 == 2;
            slice_delta_ = p.slice_qp_delta;
        }
        have_slice_ = true;
        break;
    case VAEncMiscParameterBufferType: misc(data, size); break;
    default: throw std::invalid_argument("unsupported VA encode parameter buffer");
    }
}

void EncodePicture::finish(PictureSize context, bool first) {
    validate(coded != VA_INVALID_ID && have_slice_ && (!first || (have_sequence_ && keyframe)),
             "VA encode requires a picture and slice, starting with a sequence and IDR");
    validate(!keyframe || intra_slice_, "an IDR must use an intra slice");
    if (settings.rate_mode == VA_RC_CQP) {
        qp = pic_qp_ + slice_delta_;
        validate(qp >= 0 && qp <= 51, "VA slice QP outside 8-bit range");
    }
    if (profile_ == VAProfileHEVCMain) validate(intra_picture_ == intra_slice_, "HEVC picture and slice types differ");
    // The stateful encoder can force an IDR, not a non-IDR intra picture.
    validate(!intra_slice_ || keyframe, "open GOP intra pictures are unsupported");
    validate(settings.size.width <= input_.width && settings.size.height <= input_.height &&
             settings.size.width <= context.width && settings.size.height <= context.height,
             "encode picture exceeds VA surface or context");
    if (rate_) settings.bitrate = rate_;
    if (fps_num_) {
        settings.fps_num = fps_num_;
        settings.fps_den = fps_den_;
    }
}

VaEncoder::VaEncoder(VAProfile profile, unsigned rate_mode)
    : profile_(profile), settings_{{}, 2'000'000, 30, 1, 120, 0, rate_mode, false} {}

void VaEncoder::encode(const EncodePicture& picture, const EncodeLayout& input, int fd,
                       size_t limit, std::vector<uint8_t>& coded) {
    if (failed_) throw std::runtime_error("VA encoder must be recreated after a V4L2 failure");
    const auto& next = picture.settings;
    if (device_) {
        const auto same_rate = uint64_t(next.fps_num) * settings_.fps_den == uint64_t(settings_.fps_num) * next.fps_den;
        validate(same_size(next.size, settings_.size) && same_rate && next.gop == settings_.gop &&
                 next.level == settings_.level && next.high_tier == settings_.high_tier &&
                 input.fourcc == device_->layout().fourcc, "encoder format, framerate or GOP changed in place");
    }
    try {
        if (!device_) device_.emplace(profile_, next, input.fourcc, limit);
        else if (next.rate_mode != VA_RC_CQP && next.bitrate != settings_.bitrate) device_->set_bitrate(next.bitrate);
        settings_ = next;
        auto view = input;
        view.size = next.size; // crop from (0,0); keep physical pitch and offsets
        const auto& layout = device_->layout();
        if (!direct_encode_layout(view, layout)) {
            if (!staging_) staging_ = allocate_encode_surface(open_dma_heap("/dev/dma_heap/system").get(), layout);
            wait_encode_input(fd);
            CpuView src(fd, input.bytes, false);
            CpuView dst(staging_->fd.get(), staging_->layout.bytes, true);
            copy_encode_pixels(staging_->layout, dst.data(), view, src.data());
            src.finish();
            dst.finish();
            fd = staging_->fd.get();
            view = staging_->layout;
        }
        device_->encode(fd, view.bytes, picture.keyframe, picture.qp, limit, coded);
    } catch (...) {
        failed_ = true;
        device_.reset();
        throw;
    }
}

} // namespace c2::va
