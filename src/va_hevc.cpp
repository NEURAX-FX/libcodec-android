// SPDX-License-Identifier: Apache-2.0
#include "va_hevc.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace c2::va {
namespace {

[[noreturn]] void invalid(const char* reason) {
    throw std::invalid_argument(reason);
}

class SliceBits {
public:
    explicit SliceBits(const HevcSlice& slice) : slice_(slice) {}

    bool bit() {
        if (!left_) {
            byte_ = next_byte();
            left_ = 8;
        }
        return (byte_ >> --left_) & 1;
    }

    uint32_t bits(unsigned count) {
        uint32_t value = 0;
        while (count--) value = (value << 1) | bit();
        return value;
    }

    uint32_t ue() {
        unsigned zeros = 0;
        while (!bit()) {
            if (++zeros > 31) invalid("HEVC slice Exp-Golomb code out of range");
        }
        return ((uint32_t{1} << zeros) - 1) + bits(zeros);
    }

private:
    uint8_t next_byte() {
        if (at_ >= slice_.size) invalid("truncated HEVC slice prefix");
        auto byte = slice_.data[at_++];
        if (zeros_ == 2 && byte <= 3) {
            if (byte != 3 || at_ >= slice_.size || slice_.data[at_] > 3) {
                invalid("invalid HEVC slice emulation prevention");
            }
            byte = slice_.data[at_++];
            zeros_ = 0;
        }
        zeros_ = byte == 0 ? zeros_ + 1 : 0;
        return byte;
    }

    const HevcSlice& slice_;
    size_t at_ = 2;
    unsigned left_ = 0, zeros_ = 0;
    uint8_t byte_ = 0;
};

class NalWriter {
public:
    explicit NalWriter(uint8_t type, size_t capacity = 64) {
        data_.reserve(capacity);
        data_.insert(data_.end(), {0, 0, 0, 1, static_cast<uint8_t>(type << 1), 1});
    }

    void bit(bool value) {
        byte_ = static_cast<uint8_t>((byte_ << 1) | value);
        if (++used_ != 8) return;
        if (zeros_ == 2 && byte_ <= 3) {
            data_.push_back(3);
            zeros_ = 0;
        }
        data_.push_back(byte_);
        zeros_ = byte_ == 0 ? zeros_ + 1 : 0;
        used_ = 0;
        byte_ = 0;
    }

    void bits(uint32_t value, unsigned count) {
        while (count) bit((value >> --count) & 1);
    }

    void ue(uint32_t value) {
        const auto code = value + 1;
        unsigned width = 1;
        for (auto rest = code; rest > 1; rest >>= 1) {
            bit(false);
            ++width;
        }
        bits(code, width);
    }

    void se(int value) { ue(value > 0 ? 2 * value - 1 : -2 * value); }

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

template<size_t N>
struct TileAxis {
    unsigned count = 1;
    uint32_t edge[N + 1];

    void init(uint32_t ctbs, unsigned minus1, const uint16_t (&span_minus1)[N - 1]) {
        if (minus1 >= N || minus1 >= ctbs) invalid("HEVC tile count out of range");
        count = minus1 + 1;
        edge[0] = 0;
        for (unsigned i = 0; i < minus1; ++i) {
            edge[i + 1] = edge[i] + span_minus1[i] + 1;
            if (edge[i + 1] >= ctbs) invalid("HEVC tile dimensions exhaust the picture");
        }
        edge[count] = ctbs;
    }

    unsigned tile(uint32_t ctb) const {
        return std::upper_bound(edge + 1, edge + count, ctb) - edge - 1;
    }
};

struct Geometry {
    unsigned ctb_size, width, height;
    TileAxis<20> columns;
    TileAxis<22> rows;
    bool tiled;

    uint32_t size() const { return width * height; }

    uint32_t scan_address(uint32_t raster) const {
        if (!tiled) return raster;
        const auto x = raster % width, y = raster / width;
        const auto col = columns.tile(x), row = rows.tile(y);
        const auto left = columns.edge[col], top = rows.edge[row];
        const auto tile_width = columns.edge[col + 1] - left;
        const auto tile_height = rows.edge[row + 1] - top;
        return top * width + left * tile_height + (y - top) * tile_width + x - left;
    }
};

Geometry validate_picture(const VAPictureParameterBufferHEVC& pic, const VAIQMatrixBufferHEVC* iq) {
    const auto p = pic.pic_fields.bits;
    const auto sh = pic.slice_parsing_fields.bits;
    constexpr auto field = VA_PICTURE_HEVC_FIELD_PIC | VA_PICTURE_HEVC_BOTTOM_FIELD;
    constexpr auto long_term = VA_PICTURE_HEVC_LONG_TERM_REFERENCE | VA_PICTURE_HEVC_RPS_LT_CURR;
    if (p.chroma_format_idc != 1 || p.separate_colour_plane_flag || pic.bit_depth_luma_minus8 ||
        pic.bit_depth_chroma_minus8 || (pic.CurrPic.flags & field)) {
        invalid("HEVC bridge requires progressive Main 8-bit 4:2:0");
    }
    if (pic.num_short_term_ref_pic_sets) invalid("HEVC SPS short-term RPS templates are unsupported");
    if (sh.long_term_ref_pics_present_flag || pic.num_long_term_ref_pic_sps) {
        invalid("HEVC long-term reference signaling is unsupported");
    }
    for (const auto& ref : pic.ReferenceFrames) {
        if (ref.flags & VA_PICTURE_HEVC_INVALID) continue;
        if (ref.flags & long_term) invalid("HEVC long-term reference pictures are unsupported");
        if (ref.flags & field) invalid("HEVC field references are unsupported");
    }
    if (p.scaling_list_enabled_flag && !iq) invalid("missing HEVC IQ matrices");
    if (p.tiles_enabled_flag && p.entropy_coding_sync_enabled_flag) {
        invalid("HEVC Main does not support tiles combined with WPP");
    }

    const unsigned min_cb = pic.log2_min_luma_coding_block_size_minus3 + 3u;
    const unsigned ctb = min_cb + pic.log2_diff_max_min_luma_coding_block_size;
    const unsigned min_tb = pic.log2_min_transform_block_size_minus2 + 2u;
    const unsigned max_tb = min_tb + pic.log2_diff_max_min_transform_block_size;
    if (ctb < 4 || ctb > 6 || min_tb >= min_cb || max_tb > std::min(ctb, 5u) ||
        pic.max_transform_hierarchy_depth_intra > ctb - min_tb ||
        pic.max_transform_hierarchy_depth_inter > ctb - min_tb) {
        invalid("invalid HEVC coding or transform block sizes");
    }
    if (p.pcm_enabled_flag) {
        const unsigned min_pcm = pic.log2_min_pcm_luma_coding_block_size_minus3 + 3u;
        if (pic.pcm_sample_bit_depth_luma_minus1 > 7 || pic.pcm_sample_bit_depth_chroma_minus1 > 7 ||
            min_pcm < std::min(min_cb, 5u) ||
            min_pcm + pic.log2_diff_max_min_pcm_luma_coding_block_size > std::min(ctb, 5u)) {
            invalid("invalid HEVC PCM parameters");
        }
    }
    if (pic.log2_max_pic_order_cnt_lsb_minus4 > 12 || pic.sps_max_dec_pic_buffering_minus1 > 15 ||
        pic.num_ref_idx_l0_default_active_minus1 > 14 || pic.num_ref_idx_l1_default_active_minus1 > 14 ||
        pic.init_qp_minus26 < -26 || pic.init_qp_minus26 > 25 ||
        pic.pps_cb_qp_offset < -12 || pic.pps_cb_qp_offset > 12 ||
        pic.pps_cr_qp_offset < -12 || pic.pps_cr_qp_offset > 12 ||
        pic.num_extra_slice_header_bits > 7 || pic.log2_parallel_merge_level_minus2 > ctb - 2 ||
        (p.cu_qp_delta_enabled_flag && pic.diff_cu_qp_delta_depth > ctb - min_cb) ||
        (!sh.pps_disable_deblocking_filter_flag &&
         (pic.pps_beta_offset_div2 < -6 || pic.pps_beta_offset_div2 > 6 ||
          pic.pps_tc_offset_div2 < -6 || pic.pps_tc_offset_div2 > 6))) {
        invalid("HEVC picture parameter out of range");
    }

    const unsigned width = pic.pic_width_in_luma_samples, height = pic.pic_height_in_luma_samples;
    const unsigned cb_size = 1u << min_cb, ctb_size = 1u << ctb;
    if (!width || !height || width % cb_size || height % cb_size) {
        invalid("HEVC coded size must be a nonzero multiple of the minimum coding block size");
    }
    Geometry geom{ctb_size, (width + ctb_size - 1) / ctb_size, (height + ctb_size - 1) / ctb_size,
                  {}, {}, static_cast<bool>(p.tiles_enabled_flag)};
    geom.columns.init(geom.width, geom.tiled ? pic.num_tile_columns_minus1 : 0, pic.column_width_minus1);
    geom.rows.init(geom.height, geom.tiled ? pic.num_tile_rows_minus1 : 0, pic.row_height_minus1);
    return geom;
}

unsigned choose_level(const VAPictureParameterBufferHEVC& pic, const Geometry& geom, size_t slices) {
    struct Level {
        unsigned idc, max_luma_ps, max_slices, columns, rows;
    };
    // H.265 Table A.1, omitting levels with identical limits for the known fields.
    constexpr Level levels[] = {
        {30, 36864, 16, 1, 1}, {60, 122880, 16, 1, 1}, {63, 245760, 20, 1, 1},
        {90, 552960, 30, 2, 2}, {93, 983040, 40, 3, 3}, {120, 2228224, 75, 5, 5},
        {150, 8912896, 200, 10, 11}, {180, 35651584, 600, 20, 22},
    };
    const uint64_t width = pic.pic_width_in_luma_samples, height = pic.pic_height_in_luma_samples;
    const auto area = width * height;
    for (const auto& level : levels) {
        const auto max_area = level.max_luma_ps;
        if (area > max_area || width * width > 8u * max_area || height * height > 8u * max_area ||
            (level.idc >= 150 && geom.ctb_size < 32) || slices > level.max_slices ||
            geom.columns.count > level.columns || geom.rows.count > level.rows) continue;
        const unsigned dpb = area <= max_area / 4 ? 16 : area <= max_area / 2 ? 12 :
                             area <= 3u * max_area / 4 ? 8 : 6;
        if (pic.sps_max_dec_pic_buffering_minus1 < dpb) return level.idc;
    }
    invalid("no HEVC Main level matches the coded size, CTBs, segments, tiles and DPB");
}

struct SlicePrefix {
    unsigned nal_type, pps_id;
    bool no_output_of_prior_pics, dependent;
    uint32_t poc_lsb = 0;

    bool rap() const { return nal_type >= 16; }
    bool idr() const { return nal_type == 19 || nal_type == 20; }
};

void read_inline_rps(SliceBits& bits, unsigned dpb_minus1, bool rap, bool intra) {
    if (bits.bit()) invalid("HEVC slice refers to an unsupported SPS short-term RPS");
    // st_ref_pic_set(0): zero SPS templates means there is no prediction flag.
    const auto negative = bits.ue(), positive = bits.ue();
    if (negative > dpb_minus1 || positive > dpb_minus1 - negative) {
        invalid("HEVC inline short-term RPS exceeds the DPB");
    }
    unsigned used = 0;
    for (unsigned i = 0; i < negative + positive; ++i) {
        if (bits.ue() > 32767) invalid("HEVC short-term RPS delta out of range");
        used += bits.bit();
    }
    if (used > 8 || (rap && used) || (!intra && !used)) invalid("invalid HEVC active short-term references");
}

SlicePrefix read_slice(const VAPictureParameterBufferHEVC& pic, const HevcSlice& slice, unsigned address_bits) {
    if (!slice.data || slice.size < 3) invalid("missing HEVC slice NAL");
    const unsigned nal = slice.data[0], layer_tid = slice.data[1];
    const unsigned type = (nal >> 1) & 63;
    // forbidden_zero_bit = 0, nuh_layer_id = 0, nuh_temporal_id_plus1 = 1.
    if ((nal & 0x81) || layer_tid != 1 || (type > 9 && (type < 16 || type > 21))) {
        invalid("unsupported or invalid HEVC slice NAL header");
    }
    // TSA/STSA refine access to higher temporal sublayers, which we do not signal.
    if (type >= 2 && type <= 5) invalid("HEVC temporal switching pictures are unsupported");

    SlicePrefix prefix{type, 0, false, false};
    const auto sh = pic.slice_parsing_fields.bits;
    if (prefix.rap() != sh.RapPicFlag || prefix.idr() != sh.IdrPicFlag) {
        invalid("HEVC BLA/IDR/CRA NAL type does not match VA picture flags");
    }
    SliceBits bits(slice);
    const bool first = bits.bit();
    if (first != (slice.address == 0)) invalid("HEVC first slice flag does not match the VA address");
    if (prefix.rap()) prefix.no_output_of_prior_pics = bits.bit();
    prefix.pps_id = bits.ue();
    if (prefix.pps_id > 63) invalid("HEVC slice PPS ID out of range");
    if (!first) {
        prefix.dependent = sh.dependent_slice_segments_enabled_flag && bits.bit();
        if (bits.bits(address_bits) != slice.address) invalid("HEVC slice segment address does not match VA parameters");
        if (prefix.dependent) return prefix;
    }

    bits.bits(pic.num_extra_slice_header_bits); // Reserved bits are not interpreted in Main.
    const auto slice_type = bits.ue(); // B = 0, P = 1, I = 2.
    if (slice_type > 2 || ((prefix.rap() || sh.IntraPicFlag) && slice_type != 2) ||
        (pic.pic_fields.bits.NoBiPredFlag && slice_type == 0)) {
        invalid("invalid or inconsistent HEVC slice type");
    }
    if (sh.output_flag_present_flag && !bits.bit()) invalid("HEVC non-output pictures are unsupported");
    if (!prefix.idr()) {
        prefix.poc_lsb = bits.bits(pic.log2_max_pic_order_cnt_lsb_minus4 + 4u);
        read_inline_rps(bits, pic.sps_max_dec_pic_buffering_minus1, prefix.rap(), slice_type == 2);
    }
    return prefix;
}

void write_profile_tier_level(NalWriter& nal, unsigned level) {
    nal.bits(0, 2); // general_profile_space
    nal.bit(false); // general_tier_flag (Main)
    nal.bits(1, 5); // general_profile_idc (Main)
    nal.bits(0x60000000u, 32); // Main and Main 10 compatibility
    nal.bit(true);  // general_progressive_source_flag
    nal.bit(false); // general_interlaced_source_flag
    nal.bit(false); // general_non_packed_constraint_flag
    nal.bit(true);  // general_frame_only_constraint_flag
    nal.bits(0, 32);
    nal.bits(0, 12); // reserved/constraint bits and general_inbld_flag
    nal.bits(level, 8);
}

void write_ordering(NalWriter& nal, const VAPictureParameterBufferHEVC& pic) {
    nal.bit(false); // sub_layer_ordering_info_present_flag (only one sub-layer)
    nal.ue(pic.sps_max_dec_pic_buffering_minus1);
    nal.ue(0); // max_num_reorder_pics: decode-order HAL output, without altering POC
    nal.ue(0); // max_latency_increase_plus1
}

std::vector<uint8_t> make_vps(const VAPictureParameterBufferHEVC& pic, unsigned level) {
    NalWriter nal(32);
    nal.bits(0, 4);   // vps_video_parameter_set_id
    nal.bit(true);   // vps_base_layer_internal_flag
    nal.bit(true);   // vps_base_layer_available_flag
    nal.bits(0, 6);  // vps_max_layers_minus1
    nal.bits(0, 3);  // vps_max_sub_layers_minus1
    nal.bit(true);   // vps_temporal_id_nesting_flag
    nal.bits(0xffff, 16);
    write_profile_tier_level(nal, level);
    write_ordering(nal, pic);
    nal.bits(0, 6);  // vps_max_layer_id
    nal.ue(0);       // vps_num_layer_sets_minus1
    nal.bit(false);  // vps_timing_info_present_flag
    nal.bit(false);  // vps_extension_flag
    return nal.finish();
}

std::vector<uint8_t> make_sps(const VAPictureParameterBufferHEVC& pic, unsigned level) {
    const auto p = pic.pic_fields.bits;
    const auto sh = pic.slice_parsing_fields.bits;
    NalWriter nal(33);
    nal.bits(0, 4);  // sps_video_parameter_set_id
    nal.bits(0, 3);  // sps_max_sub_layers_minus1
    nal.bit(true);   // sps_temporal_id_nesting_flag
    write_profile_tier_level(nal, level);
    nal.ue(0);       // sps_seq_parameter_set_id
    nal.ue(p.chroma_format_idc);
    nal.ue(pic.pic_width_in_luma_samples);
    nal.ue(pic.pic_height_in_luma_samples);
    nal.bit(false);  // conformance_window_flag: only coded dimensions are known
    nal.ue(pic.bit_depth_luma_minus8);
    nal.ue(pic.bit_depth_chroma_minus8);
    nal.ue(pic.log2_max_pic_order_cnt_lsb_minus4);
    write_ordering(nal, pic);
    nal.ue(pic.log2_min_luma_coding_block_size_minus3);
    nal.ue(pic.log2_diff_max_min_luma_coding_block_size);
    nal.ue(pic.log2_min_transform_block_size_minus2);
    nal.ue(pic.log2_diff_max_min_transform_block_size);
    nal.ue(pic.max_transform_hierarchy_depth_inter);
    nal.ue(pic.max_transform_hierarchy_depth_intra);
    nal.bit(p.scaling_list_enabled_flag);
    if (p.scaling_list_enabled_flag) nal.bit(false); // resolved VA matrices go in PPS
    nal.bit(p.amp_enabled_flag);
    nal.bit(sh.sample_adaptive_offset_enabled_flag);
    nal.bit(p.pcm_enabled_flag);
    if (p.pcm_enabled_flag) {
        nal.bits(pic.pcm_sample_bit_depth_luma_minus1, 4);
        nal.bits(pic.pcm_sample_bit_depth_chroma_minus1, 4);
        nal.ue(pic.log2_min_pcm_luma_coding_block_size_minus3);
        nal.ue(pic.log2_diff_max_min_pcm_luma_coding_block_size);
        nal.bit(p.pcm_loop_filter_disabled_flag);
    }
    nal.ue(0);      // num_short_term_ref_pic_sets
    nal.bit(false); // long_term_ref_pics_present_flag
    nal.bit(sh.sps_temporal_mvp_enabled_flag);
    nal.bit(p.strong_intra_smoothing_enabled_flag);
    nal.bit(false); // vui_parameters_present_flag
    nal.bit(false); // sps_extension_present_flag
    return nal.finish();
}

void write_matrix(NalWriter& nal, const uint8_t* raster, int width, int dc = 0, bool separate_dc = false) {
    nal.bit(true); // scaling_list_pred_mode_flag: explicit coefficients
    int next_coef = 8;
    if (separate_dc) {
        if (!dc) invalid("HEVC scaling matrix DC coefficient is zero");
        nal.se(dc - 8);
        next_coef = dc;
    }
    // HEVC up-right diagonal order (VA stores even the reduced 16x16 and
    // 32x32 matrices in raster order on their 8x8 grids).
    for (int diagonal = 0; diagonal < 2 * width - 1; ++diagonal) {
        for (int x = std::max(0, diagonal - width + 1); x <= std::min(diagonal, width - 1); ++x) {
            const int coef = raster[(diagonal - x) * width + x];
            if (!coef) invalid("HEVC scaling matrix coefficient is zero");
            int delta = coef - next_coef;
            if (delta < -128) delta += 256;
            if (delta > 127) delta -= 256;
            nal.se(delta);
            next_coef = coef;
        }
    }
}

void write_scaling_lists(NalWriter& nal, const VAIQMatrixBufferHEVC& iq) {
    for (const auto& matrix : iq.ScalingList4x4) write_matrix(nal, matrix, 4);
    for (const auto& matrix : iq.ScalingList8x8) write_matrix(nal, matrix, 8);
    for (unsigned i = 0; i < 6; ++i) write_matrix(nal, iq.ScalingList16x16[i], 8, iq.ScalingListDC16x16[i], true);
    // sizeId 3 uses syntax matrix IDs 0 and 3; VA stores them as slots 0 and 1.
    for (unsigned i = 0; i < 2; ++i) write_matrix(nal, iq.ScalingList32x32[i], 8, iq.ScalingListDC32x32[i], true);
}

std::vector<uint8_t> make_pps(const VAPictureParameterBufferHEVC& pic, const VAIQMatrixBufferHEVC* iq,
                              const Geometry& geom, unsigned pps_id) {
    const auto p = pic.pic_fields.bits;
    const auto sh = pic.slice_parsing_fields.bits;
    NalWriter nal(34, p.scaling_list_enabled_flag ? 512 : 64);
    nal.ue(pps_id);
    nal.ue(0); // pps_seq_parameter_set_id
    nal.bit(sh.dependent_slice_segments_enabled_flag);
    nal.bit(sh.output_flag_present_flag);
    nal.bits(pic.num_extra_slice_header_bits, 3);
    nal.bit(p.sign_data_hiding_enabled_flag);
    nal.bit(sh.cabac_init_present_flag);
    nal.ue(pic.num_ref_idx_l0_default_active_minus1);
    nal.ue(pic.num_ref_idx_l1_default_active_minus1);
    nal.se(pic.init_qp_minus26);
    nal.bit(p.constrained_intra_pred_flag);
    nal.bit(p.transform_skip_enabled_flag);
    nal.bit(p.cu_qp_delta_enabled_flag);
    if (p.cu_qp_delta_enabled_flag) nal.ue(pic.diff_cu_qp_delta_depth);
    nal.se(pic.pps_cb_qp_offset);
    nal.se(pic.pps_cr_qp_offset);
    nal.bit(sh.pps_slice_chroma_qp_offsets_present_flag);
    nal.bit(p.weighted_pred_flag);
    nal.bit(p.weighted_bipred_flag);
    nal.bit(p.transquant_bypass_enabled_flag);
    nal.bit(p.tiles_enabled_flag);
    nal.bit(p.entropy_coding_sync_enabled_flag);
    if (p.tiles_enabled_flag) {
        nal.ue(geom.columns.count - 1);
        nal.ue(geom.rows.count - 1);
        nal.bit(false); // uniform_spacing_flag: VA supplies resolved spans either way
        for (unsigned i = 0; i + 1 < geom.columns.count; ++i) nal.ue(pic.column_width_minus1[i]);
        for (unsigned i = 0; i + 1 < geom.rows.count; ++i) nal.ue(pic.row_height_minus1[i]);
        nal.bit(p.loop_filter_across_tiles_enabled_flag);
    }
    nal.bit(p.pps_loop_filter_across_slices_enabled_flag);
    const bool deblocking_control = sh.deblocking_filter_override_enabled_flag ||
                                    sh.pps_disable_deblocking_filter_flag ||
                                    pic.pps_beta_offset_div2 || pic.pps_tc_offset_div2;
    nal.bit(deblocking_control);
    if (deblocking_control) {
        nal.bit(sh.deblocking_filter_override_enabled_flag);
        nal.bit(sh.pps_disable_deblocking_filter_flag);
        if (!sh.pps_disable_deblocking_filter_flag) {
            nal.se(pic.pps_beta_offset_div2);
            nal.se(pic.pps_tc_offset_div2);
        }
    }
    nal.bit(p.scaling_list_enabled_flag); // pps_scaling_list_data_present_flag
    if (p.scaling_list_enabled_flag) write_scaling_lists(nal, *iq);
    nal.bit(sh.lists_modification_present_flag);
    nal.ue(pic.log2_parallel_merge_level_minus2);
    nal.bit(sh.slice_segment_header_extension_present_flag);
    nal.bit(false); // pps_extension_present_flag
    return nal.finish();
}

} // namespace

HevcHeaders hevc_headers(const VAPictureParameterBufferHEVC& pic, const VAIQMatrixBufferHEVC* iq,
                         const std::vector<HevcSlice>& slices) {
    if (slices.empty()) invalid("HEVC picture has no slices");
    const auto geom = validate_picture(pic, iq);
    const auto level = choose_level(pic, geom, slices.size());
    const auto ctb_count = geom.size();
    unsigned address_bits = 0;
    for (auto last = ctb_count - 1; last; last >>= 1) ++address_bits;

    SlicePrefix first{};
    uint32_t previous = 0;
    for (size_t i = 0; i < slices.size(); ++i) {
        const auto& slice = slices[i];
        if (slice.address >= ctb_count) invalid("HEVC slice segment address out of bounds");
        const auto scan = geom.scan_address(slice.address);
        if (i ? scan <= previous : scan != 0) invalid("invalid HEVC slice segment order");
        const auto prefix = read_slice(pic, slice, address_bits);
        if (!i) {
            first = prefix;
        } else if (prefix.nal_type != first.nal_type || prefix.pps_id != first.pps_id ||
                   prefix.no_output_of_prior_pics != first.no_output_of_prior_pics ||
                   (!prefix.dependent && prefix.poc_lsb != first.poc_lsb)) {
            invalid("HEVC slices do not belong to the same picture");
        }
        previous = scan;
    }
    return {make_vps(pic, level), make_sps(pic, level), make_pps(pic, iq, geom, first.pps_id), first.idr()};
}

} // namespace c2::va
