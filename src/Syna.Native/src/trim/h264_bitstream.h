#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace syna::h264 {

using Bytes = std::vector<uint8_t>;

// Fields needed to parse slice headers (ITU-T H.264 7.3.2.1 / 7.3.2.2).
struct Sps {
    int id = 0;
    int profile_idc = 0;
    int constraint_flags = 0;
    int level_idc = 0;
    int chroma_format_idc = 1;
    bool separate_colour_plane = false;
    int bit_depth_luma = 8;
    int bit_depth_chroma = 8;
    int log2_max_frame_num = 4;
    int poc_type = 0;
    int log2_max_poc_lsb = 4;
    bool delta_pic_order_always_zero = false;
    bool frame_mbs_only = true;
};

struct Pps {
    int id = 0;
    int sps_id = 0;
    bool cabac = false;
    bool bottom_field_pic_order_present = false;
    int num_slice_groups = 1;
    int num_ref_idx_l0_default = 1;
    int num_ref_idx_l1_default = 1;
    bool weighted_pred = false;
    int weighted_bipred_idc = 0;
    bool deblocking_filter_control_present = false;
    bool redundant_pic_cnt_present = false;
};

struct AvcConfig {
    int profile = 0;
    int compatibility = 0;
    int level = 0;
    int nal_length_size = 4;
    std::vector<Bytes> sps;  // NAL units without start codes or length prefixes
    std::vector<Bytes> pps;
    Bytes high_profile_ext;  // chroma_format/bit-depth trailer for profiles 100+, if present
};

std::optional<AvcConfig> parse_avcc(const uint8_t* data, size_t size);
Bytes build_avcc(const AvcConfig& cfg);

// Splits Annex B (start code) or length-prefixed data into NAL units.
std::vector<Bytes> split_annexb(const uint8_t* data, size_t size);
std::vector<Bytes> split_length_prefixed(const uint8_t* data, size_t size, int length_size);
Bytes join_length_prefixed(const std::vector<Bytes>& nals, int length_size);

inline int nal_type(const Bytes& nal) { return nal.empty() ? -1 : nal[0] & 0x1F; }

std::optional<Sps> parse_sps(const Bytes& nal);
std::optional<Pps> parse_pps(const Bytes& nal);

// Re-number parameter sets. Everything after the rewritten fields is copied bit-exactly.
std::optional<Bytes> rewrite_sps_id(const Bytes& nal, int new_id);
std::optional<Bytes> rewrite_pps_ids(const Bytes& nal, int new_pps_id, int new_sps_id);

// Rewrites pic_parameter_set_id of a coded slice (NAL types 1 and 5). The full slice header is
// parsed so CABAC alignment can be re-established when the field's bit length changes.
std::optional<Bytes> rewrite_slice_pps_id(const Bytes& nal, const Sps& sps, const Pps& pps, int new_pps_id, std::string* error);

}  // namespace syna::h264
