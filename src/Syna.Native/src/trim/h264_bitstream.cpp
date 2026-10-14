#include "trim/h264_bitstream.h"

#include <stdexcept>

namespace syna::h264 {
namespace {

struct Truncated : std::runtime_error {
    Truncated() : std::runtime_error("truncated bitstream") {}
};

Bytes to_rbsp(const uint8_t* data, size_t size) {
    Bytes out;
    out.reserve(size);
    int zeros = 0;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t b = data[i];
        if (zeros >= 2 && b == 0x03) {
            zeros = 0;
            continue;  // emulation prevention byte
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

Bytes to_nal(const Bytes& rbsp) {
    Bytes out;
    out.reserve(rbsp.size() + rbsp.size() / 64 + 4);
    int zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 0x03) {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

class BitReader {
public:
    explicit BitReader(const Bytes& data) : d_(data) {}
    size_t pos() const { return pos_; }
    size_t size_bits() const { return d_.size() * 8; }
    uint32_t bit() {
        if (pos_ >= size_bits()) throw Truncated();
        const uint32_t v = (d_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1;
        ++pos_;
        return v;
    }
    uint32_t bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | bit();
        return v;
    }
    uint32_t ue() {
        int zeros = 0;
        while (bit() == 0) {
            if (++zeros > 31) throw Truncated();
        }
        return zeros == 0 ? 0 : ((1u << zeros) - 1) + bits(zeros);
    }
    int32_t se() {
        const uint32_t k = ue();
        return (k & 1) ? static_cast<int32_t>((k + 1) / 2) : -static_cast<int32_t>(k / 2);
    }

private:
    const Bytes& d_;
    size_t pos_ = 0;
};

class BitWriter {
public:
    void bit(uint32_t v) {
        if ((n_ & 7) == 0) out_.push_back(0);
        if (v) out_.back() |= static_cast<uint8_t>(0x80 >> (n_ & 7));
        ++n_;
    }
    void bits(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) bit((v >> i) & 1);
    }
    void ue(uint32_t v) {
        const uint32_t x = v + 1;
        int len = 0;
        while ((x >> len) > 1) ++len;
        bits(0, len);
        bits(x, len + 1);
    }
    bool aligned() const { return (n_ & 7) == 0; }
    void trailing() {
        bit(1);
        while (!aligned()) bit(0);
    }
    void bytes(const uint8_t* p, size_t n) {
        if (!aligned()) throw std::logic_error("unaligned byte copy");
        out_.insert(out_.end(), p, p + n);
        n_ += n * 8;
    }
    Bytes take() { return std::move(out_); }

private:
    Bytes out_;
    size_t n_ = 0;
};

// Copies bits [from, to) of src into the writer.
void copy_bits(const Bytes& src, size_t from, size_t to, BitWriter& w) {
    for (size_t i = from; i < to; ++i) w.bit((src[i >> 3] >> (7 - (i & 7))) & 1);
}

// Bit index of the rbsp_stop_one_bit (the last set bit), or npos.
size_t stop_bit_pos(const Bytes& rbsp) {
    for (size_t i = rbsp.size(); i-- > 0;) {
        if (rbsp[i] == 0) continue;
        for (int b = 0; b < 8; ++b) {
            if (rbsp[i] & (1 << b)) return i * 8 + (7 - b);
        }
    }
    return std::string::npos;
}

bool is_high_profile(int p) {
    switch (p) {
        case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135:
            return true;
        default:
            return false;
    }
}

void skip_scaling_list(BitReader& r, int size) {
    int last = 8, next = 8;
    for (int j = 0; j < size; ++j) {
        if (next != 0) next = (last + r.se() + 256) % 256;
        last = next == 0 ? last : next;
    }
}

// Rewrites the leading ue(v) fields of a parameter set: `fields` receives the parsed values and
// returns the values to write.
template <class F>
std::optional<Bytes> rewrite_leading(const Bytes& nal, int prefix_bits, int count, F&& replace) {
    if (nal.size() < 2) return std::nullopt;
    try {
        Bytes rbsp = to_rbsp(nal.data() + 1, nal.size() - 1);
        BitReader r(rbsp);
        BitWriter w;
        for (int i = 0; i < prefix_bits; ++i) w.bit(r.bit());
        std::vector<uint32_t> values;
        for (int i = 0; i < count; ++i) values.push_back(r.ue());
        replace(values);
        for (uint32_t v : values) w.ue(v);
        const size_t stop = stop_bit_pos(rbsp);
        if (stop == std::string::npos || stop < r.pos()) return std::nullopt;
        copy_bits(rbsp, r.pos(), stop, w);
        w.trailing();
        Bytes out{nal[0]};
        Bytes payload = to_nal(w.take());
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace

std::optional<AvcConfig> parse_avcc(const uint8_t* d, size_t n) {
    if (!d || n < 7 || d[0] != 1) return std::nullopt;
    AvcConfig c;
    c.profile = d[1];
    c.compatibility = d[2];
    c.level = d[3];
    c.nal_length_size = (d[4] & 3) + 1;
    size_t p = 5;
    auto read_list = [&](int count, std::vector<Bytes>& into) {
        for (int i = 0; i < count; ++i) {
            if (p + 2 > n) return false;
            const size_t len = (static_cast<size_t>(d[p]) << 8) | d[p + 1];
            p += 2;
            if (p + len > n) return false;
            into.emplace_back(d + p, d + p + len);
            p += len;
        }
        return true;
    };
    if (!read_list(d[p++] & 0x1F, c.sps)) return std::nullopt;
    if (p >= n) return std::nullopt;
    if (!read_list(d[p++], c.pps)) return std::nullopt;
    if (p < n) c.high_profile_ext.assign(d + p, d + n);
    return c;
}

Bytes build_avcc(const AvcConfig& c) {
    Bytes out = {1, static_cast<uint8_t>(c.profile), static_cast<uint8_t>(c.compatibility), static_cast<uint8_t>(c.level),
                 static_cast<uint8_t>(0xFC | ((c.nal_length_size - 1) & 3)), static_cast<uint8_t>(0xE0 | (c.sps.size() & 0x1F))};
    auto put = [&](const Bytes& b) {
        out.push_back(static_cast<uint8_t>(b.size() >> 8));
        out.push_back(static_cast<uint8_t>(b.size() & 0xFF));
        out.insert(out.end(), b.begin(), b.end());
    };
    for (const Bytes& s : c.sps) put(s);
    out.push_back(static_cast<uint8_t>(c.pps.size()));
    for (const Bytes& p : c.pps) put(p);
    out.insert(out.end(), c.high_profile_ext.begin(), c.high_profile_ext.end());
    return out;
}

std::vector<Bytes> split_annexb(const uint8_t* d, size_t n) {
    std::vector<Bytes> nals;
    size_t i = 0, start = std::string::npos;
    while (i + 2 < n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start != std::string::npos) {
                size_t end = i;
                while (end > start && d[end - 1] == 0) --end;  // trailing_zero_8bits / 4-byte start code
                nals.emplace_back(d + start, d + end);
            }
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != std::string::npos && start < n) nals.emplace_back(d + start, d + n);
    return nals;
}

std::vector<Bytes> split_length_prefixed(const uint8_t* d, size_t n, int ls) {
    std::vector<Bytes> nals;
    size_t p = 0;
    while (p + ls <= n) {
        size_t len = 0;
        for (int i = 0; i < ls; ++i) len = (len << 8) | d[p + i];
        p += ls;
        if (len == 0 || p + len > n) break;
        nals.emplace_back(d + p, d + p + len);
        p += len;
    }
    return nals;
}

Bytes join_length_prefixed(const std::vector<Bytes>& nals, int ls) {
    Bytes out;
    for (const Bytes& nal : nals) {
        for (int i = ls - 1; i >= 0; --i) out.push_back(static_cast<uint8_t>((nal.size() >> (8 * i)) & 0xFF));
        out.insert(out.end(), nal.begin(), nal.end());
    }
    return out;
}

std::optional<Sps> parse_sps(const Bytes& nal) {
    if (nal_type(nal) != 7) return std::nullopt;
    try {
        Bytes rbsp = to_rbsp(nal.data() + 1, nal.size() - 1);
        BitReader r(rbsp);
        Sps s;
        s.profile_idc = static_cast<int>(r.bits(8));
        s.constraint_flags = static_cast<int>(r.bits(8));
        s.level_idc = static_cast<int>(r.bits(8));
        s.id = static_cast<int>(r.ue());
        if (is_high_profile(s.profile_idc)) {
            s.chroma_format_idc = static_cast<int>(r.ue());
            if (s.chroma_format_idc == 3) s.separate_colour_plane = r.bit() != 0;
            s.bit_depth_luma = static_cast<int>(r.ue()) + 8;
            s.bit_depth_chroma = static_cast<int>(r.ue()) + 8;
            r.bit();  // qpprime_y_zero_transform_bypass_flag
            if (r.bit()) {
                const int lists = s.chroma_format_idc != 3 ? 8 : 12;
                for (int i = 0; i < lists; ++i) {
                    if (r.bit()) skip_scaling_list(r, i < 6 ? 16 : 64);
                }
            }
        }
        s.log2_max_frame_num = static_cast<int>(r.ue()) + 4;
        s.poc_type = static_cast<int>(r.ue());
        if (s.poc_type == 0) {
            s.log2_max_poc_lsb = static_cast<int>(r.ue()) + 4;
        } else if (s.poc_type == 1) {
            s.delta_pic_order_always_zero = r.bit() != 0;
            r.se();
            r.se();
            const uint32_t cycle = r.ue();
            for (uint32_t i = 0; i < cycle; ++i) r.se();
        }
        r.ue();   // max_num_ref_frames
        r.bit();  // gaps_in_frame_num_value_allowed_flag
        r.ue();   // pic_width_in_mbs_minus1
        r.ue();   // pic_height_in_map_units_minus1
        s.frame_mbs_only = r.bit() != 0;
        return s;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<Pps> parse_pps(const Bytes& nal) {
    if (nal_type(nal) != 8) return std::nullopt;
    try {
        Bytes rbsp = to_rbsp(nal.data() + 1, nal.size() - 1);
        BitReader r(rbsp);
        Pps p;
        p.id = static_cast<int>(r.ue());
        p.sps_id = static_cast<int>(r.ue());
        p.cabac = r.bit() != 0;
        p.bottom_field_pic_order_present = r.bit() != 0;
        p.num_slice_groups = static_cast<int>(r.ue()) + 1;
        if (p.num_slice_groups > 1) return p;  // FMO: caller rejects
        p.num_ref_idx_l0_default = static_cast<int>(r.ue()) + 1;
        p.num_ref_idx_l1_default = static_cast<int>(r.ue()) + 1;
        p.weighted_pred = r.bit() != 0;
        p.weighted_bipred_idc = static_cast<int>(r.bits(2));
        r.se();  // pic_init_qp_minus26
        r.se();  // pic_init_qs_minus26
        r.se();  // chroma_qp_index_offset
        p.deblocking_filter_control_present = r.bit() != 0;
        r.bit();  // constrained_intra_pred_flag
        p.redundant_pic_cnt_present = r.bit() != 0;
        return p;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<Bytes> rewrite_sps_id(const Bytes& nal, int new_id) {
    if (nal_type(nal) != 7) return std::nullopt;
    return rewrite_leading(nal, 24, 1, [&](std::vector<uint32_t>& v) { v[0] = static_cast<uint32_t>(new_id); });
}

std::optional<Bytes> rewrite_pps_ids(const Bytes& nal, int new_pps_id, int new_sps_id) {
    if (nal_type(nal) != 8) return std::nullopt;
    return rewrite_leading(nal, 0, 2, [&](std::vector<uint32_t>& v) {
        v[0] = static_cast<uint32_t>(new_pps_id);
        v[1] = static_cast<uint32_t>(new_sps_id);
    });
}

std::optional<Bytes> rewrite_slice_pps_id(const Bytes& nal, const Sps& sps, const Pps& pps, int new_pps_id, std::string* error) {
    const int type = nal_type(nal);
    if (type != 1 && type != 5) {
        if (error) *error = "not a coded slice";
        return std::nullopt;
    }
    if (pps.num_slice_groups > 1) {
        if (error) *error = "slice groups (FMO) are not supported";
        return std::nullopt;
    }
    const int nal_ref_idc = (nal[0] >> 5) & 3;
    const bool idr = type == 5;
    try {
        Bytes rbsp = to_rbsp(nal.data() + 1, nal.size() - 1);
        BitReader r(rbsp);
        BitWriter w;
        w.ue(r.ue());                        // first_mb_in_slice
        const uint32_t slice_type_raw = r.ue();
        w.ue(slice_type_raw);
        r.ue();                              // pic_parameter_set_id (old)
        w.ue(static_cast<uint32_t>(new_pps_id));
        const size_t header_rest = r.pos();

        const int st = static_cast<int>(slice_type_raw % 5);
        const bool P = st == 0, B = st == 1, I = st == 2, SP = st == 3, SI = st == 4;
        const int chroma_array_type = sps.separate_colour_plane ? 0 : sps.chroma_format_idc;
        if (sps.separate_colour_plane) r.bits(2);
        r.bits(sps.log2_max_frame_num);
        bool field_pic = false;
        if (!sps.frame_mbs_only) {
            field_pic = r.bit() != 0;
            if (field_pic) r.bit();
        }
        if (idr) r.ue();
        if (sps.poc_type == 0) {
            r.bits(sps.log2_max_poc_lsb);
            if (pps.bottom_field_pic_order_present && !field_pic) r.se();
        }
        if (sps.poc_type == 1 && !sps.delta_pic_order_always_zero) {
            r.se();
            if (pps.bottom_field_pic_order_present && !field_pic) r.se();
        }
        if (pps.redundant_pic_cnt_present) r.ue();
        if (B) r.bit();
        int l0 = pps.num_ref_idx_l0_default, l1 = pps.num_ref_idx_l1_default;
        if (P || SP || B) {
            if (r.bit()) {
                l0 = static_cast<int>(r.ue()) + 1;
                if (B) l1 = static_cast<int>(r.ue()) + 1;
            }
        }
        auto list_modification = [&] {
            if (r.bit()) {
                for (int guard = 0; guard < 64; ++guard) {
                    const uint32_t idc = r.ue();
                    if (idc == 3) break;
                    r.ue();
                }
            }
        };
        if (!I && !SI) list_modification();
        if (B) list_modification();
        if ((pps.weighted_pred && (P || SP)) || (pps.weighted_bipred_idc == 1 && B)) {
            r.ue();
            if (chroma_array_type != 0) r.ue();
            auto weights = [&](int count) {
                for (int i = 0; i < count; ++i) {
                    if (r.bit()) {
                        r.se();
                        r.se();
                    }
                    if (chroma_array_type != 0 && r.bit()) {
                        for (int j = 0; j < 4; ++j) r.se();
                    }
                }
            };
            weights(l0);
            if (B) weights(l1);
        }
        if (nal_ref_idc != 0) {
            if (idr) {
                r.bit();
                r.bit();
            } else if (r.bit()) {
                for (int guard = 0; guard < 64; ++guard) {
                    const uint32_t mmco = r.ue();
                    if (mmco == 0) break;
                    if (mmco == 1 || mmco == 3) r.ue();
                    if (mmco == 2) r.ue();
                    if (mmco == 3 || mmco == 6) r.ue();
                    if (mmco == 4) r.ue();
                }
            }
        }
        if (pps.cabac && !I && !SI) r.ue();
        r.se();  // slice_qp_delta
        if (SP || SI) {
            if (SP) r.bit();
            r.se();
        }
        if (pps.deblocking_filter_control_present) {
            if (r.ue() != 1) {
                r.se();
                r.se();
            }
        }
        const size_t header_end = r.pos();
        copy_bits(rbsp, header_rest, header_end, w);
        if (pps.cabac) {
            // cabac_alignment_one_bit: slice data starts at the next byte boundary.
            const size_t data_start = (header_end + 7) / 8;
            while (!w.aligned()) w.bit(1);
            w.bytes(rbsp.data() + data_start, rbsp.size() - data_start);
        } else {
            const size_t stop = stop_bit_pos(rbsp);
            if (stop == std::string::npos || stop < header_end) throw Truncated();
            copy_bits(rbsp, header_end, stop, w);
            w.trailing();
        }
        Bytes out{nal[0]};
        Bytes payload = to_nal(w.take());
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

}  // namespace syna::h264
