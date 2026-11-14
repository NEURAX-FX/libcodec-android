// SPDX-License-Identifier: Apache-2.0
#include "va_h264.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace c2::va {
namespace {

[[noreturn]] void invalid(const char* reason) {
    throw std::invalid_argument(reason);
}

// Read just the slice header prefix. Track zero bytes in EBSP order, not
// RBSP order, so escaped zero runs cannot be mistaken for start codes.
class SliceBits {
public:
    explicit SliceBits(const AvcSlice& slice) : slice_(slice) {}

    uint32_t bits(unsigned count) {
        uint32_t value = 0;
        while (count--) {
            if (!left_) {
                byte_ = next_byte();
                left_ = 8;
            }
            value = (value << 1) | ((byte_ >> --left_) & 1);
        }
        return value;
    }

    uint32_t ue() {
        unsigned zeros = 0;
        while (!bits(1)) {
            if (++zeros > 31) invalid("AVC slice Exp-Golomb code out of range");
        }
        return ((uint32_t{1} << zeros) - 1) + bits(zeros);
    }

    int32_t se() {
        const auto code = ue(); // At most 2^32 - 2, so either magnitude fits.
        return code & 1 ? static_cast<int32_t>(code / 2 + 1) : -static_cast<int32_t>(code / 2);
    }

private:
    uint8_t next_byte() {
        if (at_ >= slice_.size) invalid("truncated AVC slice prefix");
        auto byte = slice_.data[at_++];
        if (zeros_ == 2 && byte <= 3) {
            if (byte != 3 || at_ >= slice_.size || slice_.data[at_] > 3) {
                invalid("invalid AVC slice emulation prevention");
            }
            byte = slice_.data[at_++];
            zeros_ = 0;
        }
        zeros_ = byte == 0 ? zeros_ + 1 : 0;
        return byte;
    }

    const AvcSlice& slice_;
    size_t at_ = 1;
    unsigned left_ = 0, zeros_ = 0;
    uint8_t byte_ = 0;
};

class NalWriter {
public:
    explicit NalWriter(uint8_t type, size_t capacity = 32) {
        data_.reserve(capacity);
        data_.insert(data_.end(), {0, 0, 0, 1, static_cast<uint8_t>(0x60 | type)});
    }

    void bit(bool value) {
        byte_ = static_cast<uint8_t>((byte_ << 1) | value);
        if (++used_ == 8) {
            if (zeros_ == 2 && byte_ <= 3) {
                data_.push_back(3);
                zeros_ = 0;
            }
            data_.push_back(byte_);
            zeros_ = byte_ == 0 ? zeros_ + 1 : 0;
            used_ = 0;
            byte_ = 0;
        }
    }

    void bits(uint32_t value, unsigned count) {
        while (count) bit((value >> --count) & 1);
    }

    void ue(uint32_t value) {
        const auto code = value + 1; // All parameters written here are strictly below UINT32_MAX.
        unsigned size = 1;
        for (auto v = code; v > 1; v >>= 1) {
            bit(false);
            ++size;
        }
        bits(code, size);
    }

    void se(int value) {
        ue(value > 0 ? 2 * value - 1 : -2 * value);
    }

    std::vector<uint8_t> finish() {
        bit(true); // rbsp_stop_one_bit
        while (used_) bit(false);
        return std::move(data_);
    }

private:
    std::vector<uint8_t> data_;
    unsigned used_ = 0, zeros_ = 0;
    uint8_t byte_ = 0;
};

struct Profile {
    uint8_t idc, constraints;
};

Profile avc_profile(VAProfile profile) {
    switch (profile) {
    case VAProfileH264ConstrainedBaseline: return {66, 0xc0};
    case VAProfileH264Main: return {77, 0};
    case VAProfileH264High: return {100, 0};
    default: invalid("unsupported AVC VA profile");
    }
}

bool has_slice_groups(const VAPictureParameterBufferH264& picture) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    return picture.num_slice_groups_minus1 != 0;
#pragma GCC diagnostic pop
}

void validate_picture(Profile profile, const VAPictureParameterBufferH264& pic) {
    const auto seq = pic.seq_fields.bits;
    const auto pps = pic.pic_fields.bits;
    if (seq.chroma_format_idc != 1 || seq.residual_colour_transform_flag ||
        pic.bit_depth_luma_minus8 || pic.bit_depth_chroma_minus8 || !seq.frame_mbs_only_flag ||
        seq.mb_adaptive_frame_field_flag || pps.field_pic_flag) {
        invalid("AVC bridge requires progressive 8-bit 4:2:0");
    }
    if (seq.pic_order_cnt_type != 0 && seq.pic_order_cnt_type != 2) {
        invalid("AVC bridge supports only POC types 0 and 2");
    }
    if (has_slice_groups(pic) || pps.redundant_pic_cnt_present_flag) {
        invalid("AVC slice groups and redundant pictures are unsupported");
    }
    if (seq.log2_max_frame_num_minus4 > 12 ||
        (seq.pic_order_cnt_type == 0 && seq.log2_max_pic_order_cnt_lsb_minus4 > 12) ||
        pic.num_ref_frames > 16 || pps.weighted_bipred_idc > 2 ||
        pic.pic_init_qp_minus26 < -26 || pic.pic_init_qp_minus26 > 25 ||
        pic.pic_init_qs_minus26 < -26 || pic.pic_init_qs_minus26 > 25 ||
        pic.chroma_qp_index_offset < -12 || pic.chroma_qp_index_offset > 12 ||
        pic.second_chroma_qp_index_offset < -12 || pic.second_chroma_qp_index_offset > 12) {
        invalid("AVC picture parameter out of range");
    }
    if (profile.idc == 66 && (pps.entropy_coding_mode_flag || pps.weighted_pred_flag || pps.weighted_bipred_idc)) {
        invalid("CABAC and weighted prediction are not allowed in AVC Constrained Baseline");
    }
    if (profile.idc != 100 &&
        (pps.transform_8x8_mode_flag || pic.second_chroma_qp_index_offset != pic.chroma_qp_index_offset)) {
        invalid("AVC PPS extensions require the High profile");
    }
}

struct Level {
    unsigned idc, max_frame_mbs, max_dpb_mbs;
};

constexpr Level levels[] = { // H.264 Table A-1; level 1b has the same size limits as 1.
    {10,     99,    396}, {11,    396,    900}, {12,    396,   2376}, {13,    396,   2376},
    {20,    396,   2376}, {21,    792,   4752}, {22,   1620,   8100}, {30,   1620,   8100},
    {31,   3600,  18000}, {32,   5120,  20480}, {40,   8192,  32768}, {41,   8192,  32768},
    {42,   8704,  34816}, {50,  22080, 110400}, {51,  36864, 184320}, {52,  36864, 184320},
    {60, 139264, 696320}, {61, 139264, 696320}, {62, 139264, 696320},
};

struct Geometry {
    uint64_t width, height;

    uint64_t mbs() const { return width * height; }
};

struct SequenceLevel {
    unsigned idc, dpb_frames;
};

SequenceLevel choose_level(Profile profile, const VAPictureParameterBufferH264& pic, Geometry geometry) {
    const auto seq = pic.seq_fields.bits;
    for (const auto& level : levels) {
        // FFmpeg sets this VA bit from level_idc, even for Constrained Baseline.
        if ((level.idc >= 31) != static_cast<bool>(seq.MinLumaBiPredSize8x8)) continue;
        if (profile.idc != 66 && level.idc >= 30 && !seq.direct_8x8_inference_flag) continue;
        if (geometry.mbs() > level.max_frame_mbs || geometry.width * geometry.width > 8 * level.max_frame_mbs ||
            geometry.height * geometry.height > 8 * level.max_frame_mbs) continue;
        const auto dpb_frames = static_cast<unsigned>(std::min<uint64_t>(16, level.max_dpb_mbs / geometry.mbs()));
        if (dpb_frames >= pic.num_ref_frames) return {level.idc, dpb_frames};
    }
    invalid("no AVC level matches the picture size, references and prediction flags");
}

struct SlicePrefix {
    uint32_t pps_id = 0, idr_pic_id = 0, poc_lsb = 0;
    int32_t delta_poc_bottom = 0;
    unsigned type = 0; // Raw slice_type. Values 5 through 9 constrain all slices in the picture.
    bool idr = false;
};

void check_ref_count(SliceBits& bits, bool override, uint8_t count, int& inferred) {
    if (count > 15) invalid("AVC frame reference index out of range");
    if (override) {
        if (bits.ue() != count) invalid("AVC slice reference count does not match VA parameters");
    } else {
        if (inferred >= 0 && inferred != count) invalid("inconsistent AVC PPS default reference count");
        inferred = count;
    }
}

SlicePrefix read_slice(Profile profile, const VAPictureParameterBufferH264& pic, const AvcSlice& slice,
                       int& l0_default, int& l1_default) {
    if (!slice.data || slice.size < 2) invalid("missing AVC slice NAL");
    const auto nal = slice.data[0];
    const auto type = nal & 31;
    const bool reference = nal & 0x60;
    if ((nal & 0x80) || (type != 1 && type != 5) || (type == 5 && !reference)) {
        invalid("unsupported or invalid AVC slice NAL header");
    }
    if (reference != static_cast<bool>(pic.pic_fields.bits.reference_pic_flag)) {
        invalid("AVC slice nal_ref_idc does not match VA picture");
    }

    SliceBits bits(slice);
    if (bits.ue() != slice.first_mb) invalid("AVC first_mb_in_slice does not match VA parameters");
    SlicePrefix prefix{};
    prefix.idr = type == 5;
    prefix.type = bits.ue();
    const auto slice_type = prefix.type % 5;
    if (prefix.type > 9 || slice_type > 2 || (prefix.idr && slice_type != 2) ||
        (profile.idc == 66 && slice_type == 1)) {
        invalid("unsupported or invalid AVC slice type");
    }
    prefix.pps_id = bits.ue();
    if (prefix.pps_id > 255) invalid("AVC slice PPS ID out of range");
    const auto seq = pic.seq_fields.bits;
    if (bits.bits(seq.log2_max_frame_num_minus4 + 4) != pic.frame_num || (prefix.idr && pic.frame_num)) {
        invalid("invalid or inconsistent AVC slice frame_num");
    }
    if (prefix.idr) {
        prefix.idr_pic_id = bits.ue();
        if (prefix.idr_pic_id > 65535) invalid("AVC idr_pic_id out of range");
    }
    if (seq.pic_order_cnt_type == 0) {
        prefix.poc_lsb = bits.bits(seq.log2_max_pic_order_cnt_lsb_minus4 + 4);
        if (pic.pic_fields.bits.pic_order_present_flag) prefix.delta_poc_bottom = bits.se();
    }
    if (slice_type == 1) bits.bits(1); // direct_spatial_mv_pred_flag
    if (slice_type != 2) {
        const bool override = bits.bits(1);
        check_ref_count(bits, override, slice.refs_l0_minus1, l0_default);
        if (slice_type == 1) check_ref_count(bits, override, slice.refs_l1_minus1, l1_default);
    }
    return prefix;
}

// VA stores lists in raster order; H.264 scaling_list() is in zigzag order.
template<size_t Side>
constexpr auto zigzag() {
    std::array<size_t, Side * Side> scan{};
    size_t i = 0;
    for (int sum = 0; sum <= 2 * static_cast<int>(Side - 1); ++sum) {
        const int lo = std::max(0, sum - static_cast<int>(Side - 1));
        const int hi = std::min(sum, static_cast<int>(Side - 1));
        for (int y = lo; y <= hi; ++y) {
            const int row = (sum & 1) ? y : hi + lo - y;
            scan[i++] = row * Side + (sum - row);
        }
    }
    return scan;
}

template<size_t Lists, size_t Size>
bool custom_scaling(const uint8_t (&table)[Lists][Size]) {
    bool custom = false;
    for (const auto& list : table) {
        for (const auto value : list) {
            if (!value) invalid("AVC scaling list contains a zero coefficient");
            custom |= value != 16;
        }
    }
    return custom;
}

template<size_t Side, size_t Lists>
void write_scaling(NalWriter& pps, const uint8_t (&table)[Lists][Side * Side]) {
    constexpr auto scan = zigzag<Side>();
    for (const auto& list : table) {
        pps.bit(true); // Emit every active list to avoid fallback to non-flat standard defaults.
        int last = 8;
        for (const auto index : scan) {
            const int value = list[index];
            pps.se((value - last + 384) % 256 - 128); // The signed modulo-256 delta, in [-128, 127].
            last = value;
        }
    }
}

std::vector<uint8_t> make_sps(Profile profile, const VAPictureParameterBufferH264& pic, SequenceLevel level,
                              bool decode_order_hint) {
    const auto seq = pic.seq_fields.bits;
    NalWriter sps(7);
    sps.bits(profile.idc, 8);
    sps.bits(profile.constraints, 8);
    sps.bits(level.idc, 8);
    sps.ue(0); // seq_parameter_set_id, also used by the synthesized PPS
    if (profile.idc == 100) {
        sps.ue(1); // chroma_format_idc (4:2:0)
        sps.ue(0); // bit_depth_luma_minus8
        sps.ue(0); // bit_depth_chroma_minus8
        sps.bit(false); // qpprime_y_zero_transform_bypass_flag
        sps.bit(false); // seq_scaling_matrix_present_flag (resolved lists go in PPS)
    }
    sps.ue(seq.log2_max_frame_num_minus4);
    sps.ue(seq.pic_order_cnt_type);
    if (seq.pic_order_cnt_type == 0) sps.ue(seq.log2_max_pic_order_cnt_lsb_minus4);
    sps.ue(pic.num_ref_frames);
    sps.bit(seq.gaps_in_frame_num_value_allowed_flag);
    sps.ue(pic.picture_width_in_mbs_minus1);
    sps.ue(pic.picture_height_in_mbs_minus1);
    sps.bit(true); // frame_mbs_only_flag
    sps.bit(seq.direct_8x8_inference_flag);
    sps.bit(false); // frame_cropping_flag: VA clients retain their original crop
    sps.bit(decode_order_hint); // vui_parameters_present_flag
    if (decode_order_hint) {
        sps.bits(0, 8); // No optional VUI fields before bitstream_restriction_flag.
        sps.bit(true); // bitstream_restriction_flag
        sps.bit(true); // motion_vectors_over_pic_boundaries_flag
        sps.ue(0); // max_bytes_per_pic_denom
        sps.ue(0); // max_bits_per_mb_denom
        sps.ue(16); // log2_max_mv_length_horizontal
        sps.ue(16); // log2_max_mv_length_vertical
        // Request immediate Codec2 output without altering the slice POC or references.
        sps.ue(0); // max_num_reorder_frames
        sps.ue(level.dpb_frames); // max_dec_frame_buffering
    }
    return sps.finish();
}

std::vector<uint8_t> make_pps(const VAPictureParameterBufferH264& pic, const VAIQMatrixBufferH264& iq,
                              uint32_t id, unsigned l0_default, unsigned l1_default, bool custom) {
    const auto flags = pic.pic_fields.bits;
    NalWriter pps(8, custom ? (flags.transform_8x8_mode_flag ? 512 : 256) : 32);
    pps.ue(id);
    pps.ue(0); // seq_parameter_set_id
    pps.bit(flags.entropy_coding_mode_flag);
    pps.bit(flags.pic_order_present_flag);
    pps.ue(0); // num_slice_groups_minus1
    pps.ue(l0_default);
    pps.ue(l1_default);
    pps.bit(flags.weighted_pred_flag);
    pps.bits(flags.weighted_bipred_idc, 2);
    pps.se(pic.pic_init_qp_minus26);
    pps.se(pic.pic_init_qs_minus26);
    pps.se(pic.chroma_qp_index_offset);
    pps.bit(flags.deblocking_filter_control_present_flag);
    pps.bit(flags.constrained_intra_pred_flag);
    pps.bit(false); // redundant_pic_cnt_present_flag
    if (flags.transform_8x8_mode_flag || custom || pic.second_chroma_qp_index_offset != pic.chroma_qp_index_offset) {
        pps.bit(flags.transform_8x8_mode_flag);
        pps.bit(custom); // pic_scaling_matrix_present_flag
        if (custom) {
            write_scaling<4>(pps, iq.ScalingList4x4);
            if (flags.transform_8x8_mode_flag) write_scaling<8>(pps, iq.ScalingList8x8);
        }
        pps.se(pic.second_chroma_qp_index_offset);
    }
    return pps.finish();
}

} // namespace

AvcHeaders avc_headers(VAProfile va_profile, const VAPictureParameterBufferH264& picture,
                       const VAIQMatrixBufferH264& iq, const std::vector<AvcSlice>& slices,
                       bool decode_order_hint) {
    const auto profile = avc_profile(va_profile);
    validate_picture(profile, picture);
    const Geometry geometry{picture.picture_width_in_mbs_minus1 + uint64_t{1},
                            picture.picture_height_in_mbs_minus1 + uint64_t{1}};
    const auto level = choose_level(profile, picture, geometry);
    bool custom = custom_scaling(iq.ScalingList4x4);
    if (picture.pic_fields.bits.transform_8x8_mode_flag) custom |= custom_scaling(iq.ScalingList8x8);
    if (custom && profile.idc != 100) invalid("custom AVC scaling matrices require the High profile");
    if (slices.empty()) invalid("AVC picture has no slices");

    int l0_default = -1, l1_default = -1;
    bool mixed_types = false, same_type_declared = false;
    SlicePrefix first{};
    uint16_t previous_mb = 0;
    for (size_t i = 0; i < slices.size(); ++i) {
        const auto& slice = slices[i];
        if ((i ? slice.first_mb <= previous_mb : slice.first_mb != 0) || slice.first_mb >= geometry.mbs()) {
            invalid("invalid AVC slice macroblock order");
        }
        const auto prefix = read_slice(profile, picture, slice, l0_default, l1_default);
        if (!i) {
            first = prefix;
        } else if (prefix.pps_id != first.pps_id || prefix.idr != first.idr ||
                   prefix.idr_pic_id != first.idr_pic_id || prefix.poc_lsb != first.poc_lsb ||
                   prefix.delta_poc_bottom != first.delta_poc_bottom) {
            invalid("AVC slices do not belong to the same picture");
        }
        mixed_types |= prefix.type % 5 != first.type % 5;
        same_type_declared |= prefix.type >= 5;
        if (mixed_types && same_type_declared) invalid("inconsistent AVC slice type declaration");
        previous_mb = slice.first_mb;
    }

    return {make_sps(profile, picture, level, decode_order_hint),
            make_pps(picture, iq, first.pps_id, std::max(0, l0_default), std::max(0, l1_default), custom),
            first.idr};
}

} // namespace c2::va
