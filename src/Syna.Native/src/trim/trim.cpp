#include "trim/trim.h"

#include "common/ffmpeg.h"
#include "common/json.h"
#include "common/log.h"
#include "common/util.h"
#include "media/media_info.h"
#include "trim/audio_mixdown.h"
#include "trim/encoders.h"
#include "trim/h264_bitstream.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>

namespace syna {
namespace {

using h264::Bytes;

// Mixed audio is decoded from this far before S, so the first output samples are complete.
constexpr int64_t kMixPrerollUs = 200000;

const char* mode_name(int m) {
    switch (m) {
        case SYNA_TRIM_QUICK: return "quick";
        case SYNA_TRIM_SMART: return "smart";
        default: return "exact";
    }
}

const char* video_handling_name(int v) {
    switch (v) {
        case SYNA_TRIM_VIDEO_ENCODE: return "encode";
        case SYNA_TRIM_VIDEO_LOSSLESS: return "lossless";
        default: return "original";
    }
}

struct AudioOutputReport {
    std::string method;       // "copy", "encode" (one track re-encoded) or "mix" (several tracks into one)
    int tracks = 0;           // source tracks in it
    std::string description;  // "AAC · 48 kHz · stereo · 256 kb/s"
    bool adjusted = false;    // a mixer gain or offset was applied to it
};

struct Report {
    std::string requested_mode, mode_used, output, error, fallback_reason, video_method, encoder;
    std::string video_handling = "original";
    std::string audio_method = "none", audio_encoder, audio_note;
    std::vector<AudioOutputReport> audio_outputs;
    int audio_tracks = 0;
    bool success = false;
    bool hardware_encoder = false;
    bool lossless_video = false, lossless_verified = false;
    int64_t requested_in = 0, requested_out = 0, actual_start = 0, actual_end = 0;
    double reencoded_s = 0, copied_s = 0, elapsed_s = 0;
    bool v_passed = false;
    double v_duration = 0, v_fps = 0;
    int v_width = 0, v_height = 0;
    std::string v_codec;
    int64_t v_frames = 0, v_first_pts = 0, v_bit_rate = 0;
    std::vector<std::string> v_audio_codecs, v_audio_layouts, issues;
    std::vector<int> v_audio_rates;

    void reset_method() {
        mode_used.clear();
        video_method.clear();
        encoder.clear();
        hardware_encoder = false;
        lossless_verified = false;
        reencoded_s = copied_s = 0;
        issues.clear();
    }

    std::string json() const {
        JsonWriter w;
        w.begin_object();
        w.field("requestedMode", requested_mode);
        w.field("modeUsed", mode_used);
        w.field("output", output);
        w.field("success", success);
        w.field("error", error);
        w.field("fallbackReason", fallback_reason);
        w.field("requestedInUs", requested_in);
        w.field("requestedOutUs", requested_out);
        w.field("actualStartUs", actual_start);
        w.field("actualEndUs", actual_end);
        w.field("videoHandling", video_handling);
        w.field("videoMethod", video_method);
        w.field("encoder", encoder);
        w.field("hardwareEncoder", hardware_encoder);
        w.field("losslessVideo", lossless_video);
        w.field("losslessVerified", lossless_verified);
        w.field("reencodedSeconds", reencoded_s);
        w.field("copiedSeconds", copied_s);
        w.field("audioMethod", audio_method);
        w.field("audioTracks", audio_tracks);
        w.field("audioEncoder", audio_encoder);
        w.field("audioNote", audio_note);
        w.key("audioOutputs").begin_array();
        for (const AudioOutputReport& a : audio_outputs) {
            w.begin_object();
            w.field("method", a.method);
            w.field("tracks", a.tracks);
            w.field("description", a.description);
            w.field("adjusted", a.adjusted);
            w.end_object();
        }
        w.end_array();
        w.field("elapsedSeconds", elapsed_s);
        w.key("validation").begin_object();
        w.field("passed", v_passed);
        w.field("durationS", v_duration);
        w.field("width", v_width);
        w.field("height", v_height);
        w.field("videoCodec", v_codec);
        w.field("fpsAvg", v_fps);
        w.field("videoBitRate", v_bit_rate);
        w.field("frameCount", v_frames);
        w.field("firstFramePtsUs", v_first_pts);
        w.field("audioStreams", static_cast<int>(v_audio_codecs.size()));
        w.key("audioCodecs").begin_array();
        for (const auto& c : v_audio_codecs) w.value(c);
        w.end_array();
        w.key("audioSampleRates").begin_array();
        for (int r : v_audio_rates) w.value(r);
        w.end_array();
        w.key("audioLayouts").begin_array();
        for (const auto& l : v_audio_layouts) w.value(l);
        w.end_array();
        w.key("issues").begin_array();
        for (const auto& i : issues) w.value(i);
        w.end_array();
        w.end_object();
        w.end_object();
        return w.str();
    }
};

syna_result write_error(int av_err) {
    if (av_err == AVERROR(ENOSPC)) return SYNA_E_DISK_FULL;
    if (av_err == AVERROR(EACCES) || av_err == AVERROR(EPERM)) return SYNA_E_ACCESS_DENIED;
    return SYNA_E_IO;
}

bool is_annexb(const uint8_t* d, size_t n) { return n >= 4 && d[0] == 0 && d[1] == 0 && (d[2] == 1 || (d[2] == 0 && d[3] == 1)); }

// HEVC parameter-set arrays of an hvcC record.
std::optional<std::vector<Bytes>> hvcc_nals(const uint8_t* d, size_t n) {
    if (!d || n < 23 || d[0] != 1) return std::nullopt;
    std::vector<Bytes> out;
    size_t p = 22;
    const int arrays = d[p++];
    for (int a = 0; a < arrays; ++a) {
        if (p + 3 > n) return std::nullopt;
        p += 1;
        const int count = (d[p] << 8) | d[p + 1];
        p += 2;
        for (int i = 0; i < count; ++i) {
            if (p + 2 > n) return std::nullopt;
            const size_t len = (static_cast<size_t>(d[p]) << 8) | d[p + 1];
            p += 2;
            if (p + len > n) return std::nullopt;
            out.emplace_back(d + p, d + p + len);
            p += len;
        }
    }
    return out;
}

int hevc_type(const Bytes& nal) { return nal.empty() ? -1 : (nal[0] >> 1) & 0x3F; }

// True if the packet starts a clean random access point: H.264 IDR, HEVC IDR (no leading pictures).
bool is_idr(const AVPacket* pkt, AVCodecID codec, int length_size) {
    for (const Bytes& nal : h264::split_length_prefixed(pkt->data, static_cast<size_t>(pkt->size), length_size)) {
        if (codec == AV_CODEC_ID_H264 && h264::nal_type(nal) == 5) return true;
        if (codec == AV_CODEC_ID_HEVC) {
            const int t = hevc_type(nal);
            if (t == 19 || t == 20) return true;
        }
    }
    return false;
}

void replace_packet_data(AVPacket* pkt, const Bytes& data) {
    PacketPtr fresh = make_packet();
    av_new_packet(fresh.get(), static_cast<int>(data.size()));
    std::memcpy(fresh->data, data.data(), data.size());
    av_packet_copy_props(fresh.get(), pkt);
    av_packet_unref(pkt);
    av_packet_move_ref(pkt, fresh.get());
}

// Constant quality on the H.264 CQ scale (lower is better): near-transparent re-encodes that keep
// the source's settings, the splice of a Smart Cut (as close to the copied part as possible), and
// the default of a requested re-encode.
constexpr int kOriginalQuality = 16;
constexpr int kSpliceQuality = 15;
constexpr int kDefaultQuality = 23;

// What a re-encode produces.
struct VideoTarget {
    AVCodecID codec = AV_CODEC_ID_NONE;  // NONE: the source's codec, then the most compatible ones
    int width = 0, height = 0;           // 0: the source's size
    AVRational fps{0, 1};                // 0: the source's frame timing, frame for frame
    int quality = kOriginalQuality;
    int64_t bitrate = 0, max_bitrate = 0;
    int speed = SYNA_ENCODER_SPEED_BALANCED;
    bool lossless = false;
};

bool is(const EncoderInfo& e, const char* part) { return e.name.find(part) != std::string::npos; }

// The H.264-scale quality on each encoder's own scale; equal visual quality, roughly.
int encoder_quality(const EncoderInfo& e, AVCodecID target, int q) {
    if (is(e, "nvenc")) return target == AV_CODEC_ID_AV1 ? std::clamp((q * 8 + 2) / 5, 1, 63) : target == AV_CODEC_ID_HEVC ? std::min(51, q + 2) : q;
    if (is(e, "qsv")) return std::min(51, q + 2);
    if (e.name == "libsvtav1") return std::clamp(q + 8, 1, 63);
    if (e.name == "libvpx-vp9") return std::clamp(q + 5, 0, 63);
    if (e.name == "libkvazaar") return std::clamp(q + 5, 0, 51);
    if (is(e, "_mf")) return std::clamp(85 + (19 - q) * 10 / 3, 1, 100);
    return q;  // AMF QP
}

class Trimmer {
public:
    Trimmer(const TrimParams& p, Job& job) : p_(p), job_(job) {}
    syna_result run();
    std::string report_json() const { return report_.json(); }

private:
    syna_result open_source();
    bool keep_stream(unsigned i) const;
    int64_t to_video_ts(int64_t timeline_us) const { return av_rescale_q_rnd(timeline_us + origin_, kMicros, vst_->time_base, AV_ROUND_NEAR_INF); }
    int64_t video_ts_to_us(int64_t ts) const { return ts_to_us(ts, vst_->time_base) - origin_; }
    int64_t rescale_from_video(int64_t ts, const AVStream* st) const { return av_rescale_q(ts, vst_->time_base, st->time_base); }
    void seek_video(int64_t ts);
    void reset_output_state();

    syna_result create_output(const AVCodecContext* video_encoder, const Bytes* video_extradata);
    syna_result write(AVPacket* pkt, int in_index, AVRational src_tb, int64_t shift);
    syna_result write_stream(AVPacket* pkt, int out_index, AVRational src_tb);
    syna_result finish_output();

    // Audio: every output track either copies one source track or encodes one or more of them
    // (several: the mix; one: a track that had to be re-encoded).
    struct AudioOut {
        std::vector<int> sources;               // source streams, file order
        std::unique_ptr<AudioMixdown> encoder;  // null: stream copy of sources[0]
        int out_index = -1;
    };
    syna_result prepare_audio();
    TrimTrack mixer(int si) const;  // the player's mixer state for a source stream
    bool adjusted(int si) const;    // the mixer changes its level or timing
    bool can_copy_audio(int si) const;
    void add_encoded_audio_stream(AVFormatContext* oc, AudioOut& a);
    void begin_audio();
    syna_result finish_audio();
    bool encoded(int si) const { return audio_route_[static_cast<size_t>(si)] >= 0 && audio_out_[static_cast<size_t>(audio_route_[static_cast<size_t>(si)])].encoder; }
    int64_t range_start(const AVStream* st) const;
    int64_t range_end(const AVStream* st) const;
    // Encoded audio is decoded from before S: a little for the decoder, plus the track's mixer delay.
    int64_t preroll(const AVStream* st) const {
        return encoded(st->index) ? av_rescale_q(kMixPrerollUs + std::max<int64_t>(0, mixer(st->index).offset_us), kMicros, st->time_base) : 0;
    }
    syna_result audio_packet(AVPacket* pkt, int si, std::vector<bool>& done);
    syna_result write_audio_range(AVPacket* pkt, int si, std::vector<bool>& done);
    bool audio_done(const std::vector<bool>& done) const;

    CodecPtr open_video_decoder();
    VideoTarget video_target(const AVCodecContext* dec) const;
    std::optional<CodecPtr> open_encoder(const AVCodecContext* dec, const VideoTarget& t, int gop, EncoderInfo& chosen);
    std::optional<CodecPtr> open_encoder_for(AVCodecID target, const AVCodecContext* dec, int depth, const VideoTarget& t, int gop, EncoderInfo& chosen);
    syna_result encode_frame(AVCodecContext* enc, AVFrame* f, int64_t rel_pts, int64_t dur, SwsContext*& sws, FramePtr& converted,
                            std::vector<PacketPtr>* collect);

    syna_result do_quick();
    syna_result do_exact();
    syna_result do_smart();
    syna_result do_audio_only();
    std::optional<std::string> smart_blocker(int64_t& k2, int64_t& k2_delay);
    syna_result copy_until_out(PacketPtr v, std::deque<PacketPtr>& held, bool& done);

    syna_result validate();
    void validate_encoded_audio(const AudioTrackInfo& o, const AudioOut& a, size_t ordinal);
    void verify_lossless();
    void progress(int64_t video_ts);

    const TrimParams& p_;
    Job& job_;
    Report report_;
    std::wstring temp_;
    bool global_header_ = false;
    const AVOutputFormat* ofmt_ = nullptr;

    FormatPtr in_;
    MediaInfo info_;
    int vi_ = -1;
    AVStream* vst_ = nullptr;
    int64_t origin_ = 0, in_ts_ = 0, out_ts_ = 0, frame_ts_ = 1;
    std::vector<int> audio_streams_;  // kept source audio streams, in file order
    std::vector<AudioOut> audio_out_; // output audio tracks, in output order
    std::vector<int> audio_route_;    // per source stream: its entry in audio_out_, -1 if none
    int64_t audio_delay_us_ = 0;      // the longest mixer delay of an encoded track: audio read before S

    OutputFormatPtr out_;
    std::vector<int> map_;
    int64_t start_ts_ = AV_NOPTS_VALUE;   // S, absolute video time base
    int64_t video_frames_ = 0;
    int64_t video_end_rel_ = 0;           // end of the last written frame, relative to S
    int64_t splice_rel_us_ = 0;           // smart cut: first copied frame, relative to S
    VideoTarget target_;                  // what the last re-encode produced
};

void Trimmer::progress(int64_t video_ts) {
    if (start_ts_ == AV_NOPTS_VALUE || out_ts_ <= start_ts_) return;
    job_.set_progress(0.9 * static_cast<double>(video_ts - start_ts_) / static_cast<double>(out_ts_ - start_ts_));
}

syna_result Trimmer::open_source() {
    std::string err;
    int av_err = 0;
    in_ = open_input(p_.source, true, &err, &av_err);
    if (!in_) {
        report_.error = err;
        return av_err ? map_av_error(av_err) : SYNA_E_OPEN;
    }
    describe_media(in_.get(), p_.source, true, info_);
    origin_ = in_->start_time != AV_NOPTS_VALUE ? in_->start_time : 0;
    vi_ = info_.has_video ? info_.video.stream_index : -1;
    if (vi_ >= 0) {
        vst_ = in_->streams[vi_];
        const AVRational fr = vst_->avg_frame_rate.num > 0 ? vst_->avg_frame_rate : vst_->r_frame_rate;
        frame_ts_ = fr.num > 0 ? std::max<int64_t>(1, av_rescale_q(1, av_inv_q(fr), vst_->time_base)) : 1;
        in_ts_ = to_video_ts(p_.in_us);
        out_ts_ = to_video_ts(std::min(p_.out_us, info_.duration_us));
    }
    for (unsigned i = 0; i < in_->nb_streams; ++i) {
        in_->streams[i]->discard = keep_stream(i) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        if (keep_stream(i) && static_cast<int>(i) != vi_) audio_streams_.push_back(static_cast<int>(i));
    }
    return SYNA_OK;
}

bool Trimmer::keep_stream(unsigned i) const {
    const AVStream* st = in_->streams[i];
    if (static_cast<int>(i) == vi_) return true;
    return st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && (st->codecpar->ch_layout.nb_channels > 0 || st->codecpar->sample_rate > 0);
}

void Trimmer::seek_video(int64_t ts) {
    // Keyframes are indexed by DTS; step back far enough that reordering cannot hide the target,
    // and that a delayed audio track finds the audio it plays from S.
    const int64_t margin = std::max<int64_t>(av_rescale_q(1000000, kMicros, vst_->time_base), (info_.video.has_b_frames + 2) * frame_ts_) +
                           av_rescale_q(audio_delay_us_, kMicros, vst_->time_base);
    const int64_t first = vst_->start_time != AV_NOPTS_VALUE ? vst_->start_time : 0;
    const int64_t target = std::max<int64_t>(ts - margin, first);
    if (avformat_seek_file(in_.get(), vi_, INT64_MIN, target, target, 0) < 0) av_seek_frame(in_.get(), vi_, target, AVSEEK_FLAG_BACKWARD);
}

void Trimmer::reset_output_state() {
    out_.reset();
    map_.clear();
    start_ts_ = AV_NOPTS_VALUE;
    video_frames_ = 0;
    video_end_rel_ = 0;
    splice_rel_us_ = 0;
    DeleteFileW(temp_.c_str());
}

syna_result Trimmer::create_output(const AVCodecContext* video_encoder, const Bytes* video_extradata) {
    AVFormatContext* oc = nullptr;
    const std::string dest = to_utf8(p_.destination);
    int r = avformat_alloc_output_context2(&oc, nullptr, nullptr, dest.c_str());
    if (r < 0 || !oc) {
        report_.error = "unsupported output container";
        return SYNA_E_UNSUPPORTED;
    }
    out_.reset(oc);
    map_.assign(in_->nb_streams, -1);
    for (unsigned i = 0; i < in_->nb_streams; ++i) {
        if (!keep_stream(i)) continue;
        AudioOut* audio = static_cast<int>(i) != vi_ ? &audio_out_[static_cast<size_t>(audio_route_[i])] : nullptr;
        if (audio && audio->out_index >= 0) continue;  // a mix sits where its first track was
        if (audio && audio->encoder) {
            add_encoded_audio_stream(oc, *audio);
            if (audio->out_index < 0) {
                report_.error = "cannot add an encoded audio track to the output";
                return SYNA_E_FAIL;
            }
            continue;
        }
        AVStream* is = in_->streams[i];
        AVStream* os = avformat_new_stream(oc, nullptr);
        if (!os) return SYNA_E_FAIL;
        const bool video = static_cast<int>(i) == vi_;
        if (video && video_encoder) {
            avcodec_parameters_from_context(os->codecpar, video_encoder);
            os->time_base = video_encoder->time_base;
        } else {
            avcodec_parameters_copy(os->codecpar, is->codecpar);
            os->time_base = is->time_base;
            // Keep the source tag (e.g. hvc1) when this container accepts it for the codec.
            unsigned tmp = 0;
            if (oc->oformat->codec_tag && av_codec_get_id(oc->oformat->codec_tag, os->codecpar->codec_tag) != os->codecpar->codec_id &&
                av_codec_get_tag2(oc->oformat->codec_tag, os->codecpar->codec_id, &tmp))
                os->codecpar->codec_tag = 0;
            if (video && video_extradata) {
                av_freep(&os->codecpar->extradata);
                os->codecpar->extradata = static_cast<uint8_t*>(av_mallocz(video_extradata->size() + AV_INPUT_BUFFER_PADDING_SIZE));
                std::memcpy(os->codecpar->extradata, video_extradata->data(), video_extradata->size());
                os->codecpar->extradata_size = static_cast<int>(video_extradata->size());
            }
        }
        if (video) {
            os->sample_aspect_ratio = is->sample_aspect_ratio;
            os->avg_frame_rate = is->avg_frame_rate;
            os->r_frame_rate = is->r_frame_rate;
            // A converted rate is what the muxer must know (it also times the last frame with it).
            if (video_encoder && video_encoder->framerate.num > 0 && av_cmp_q(video_encoder->time_base, is->time_base) != 0)
                os->avg_frame_rate = os->r_frame_rate = video_encoder->framerate;
            if (video_encoder) {
                // Rotation and HDR metadata are carried as stream side data.
                for (int k = 0; k < is->codecpar->nb_coded_side_data; ++k) {
                    const AVPacketSideData& sd = is->codecpar->coded_side_data[k];
                    if (AVPacketSideData* dst = av_packet_side_data_new(&os->codecpar->coded_side_data, &os->codecpar->nb_coded_side_data, sd.type, sd.size, 0))
                        std::memcpy(dst->data, sd.data, sd.size);
                }
            }
        }
        av_dict_copy(&os->metadata, is->metadata, 0);
        // MP4 track names live in udta/name, which the muxer writes from "title".
        if (!av_dict_get(os->metadata, "title", nullptr, 0)) {
            if (AVDictionaryEntry* name = av_dict_get(is->metadata, "name", nullptr, 0)) av_dict_set(&os->metadata, "title", name->value, 0);
        }
        os->disposition = is->disposition;
        map_[i] = os->index;
        if (audio) audio->out_index = os->index;
    }
    av_dict_copy(&oc->metadata, in_->metadata, 0);
    const std::string temp = to_utf8(temp_);
    r = avio_open(&oc->pb, temp.c_str(), AVIO_FLAG_WRITE);
    if (r < 0) {
        report_.error = "cannot create the output file: " + av_error_string(r);
        return write_error(r);
    }
    // MP4/MOV place a track that starts after zero (a copied audio track begins at its first packet
    // after S) with an edit list counted in the movie timescale, milliseconds by default: that alone
    // shifted copied tracks by up to 1 ms. 1/90000 s keeps them within 11 us. Other muxers ignore it.
    AVDictionary* options = nullptr;
    av_dict_set_int(&options, "movie_timescale", 90000, 0);
    r = avformat_write_header(oc, &options);
    av_dict_free(&options);
    if (r < 0) {
        report_.error = "cannot write the output header: " + av_error_string(r);
        return write_error(r);
    }
    return SYNA_OK;
}

syna_result Trimmer::write(AVPacket* pkt, int in_index, AVRational src_tb, int64_t shift) {
    if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= shift;
    if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= shift;
    if (in_index == vi_) {
        ++video_frames_;
        const int64_t dur = pkt->duration > 0 ? pkt->duration : av_rescale_q(frame_ts_, vst_->time_base, src_tb);
        video_end_rel_ = std::max(video_end_rel_, av_rescale_q(pkt->pts + dur, src_tb, vst_->time_base));
    }
    return write_stream(pkt, map_[static_cast<size_t>(in_index)], src_tb);
}

syna_result Trimmer::write_stream(AVPacket* pkt, int out_index, AVRational src_tb) {
    AVStream* os = out_->streams[out_index];
    av_packet_rescale_ts(pkt, src_tb, os->time_base);
    pkt->stream_index = os->index;
    pkt->pos = -1;
    const int r = av_interleaved_write_frame(out_.get(), pkt);
    if (r < 0) {
        report_.error = "write failed: " + av_error_string(r);
        return write_error(r);
    }
    return SYNA_OK;
}

// A single audio track is stream-copied: packets starting inside [S, OUT) are kept, so the track
// stays bit-identical and in sync (it may start up to one audio frame after S).
syna_result Trimmer::write_audio_range(AVPacket* pkt, int si, std::vector<bool>& done) {
    AVStream* st = in_->streams[si];
    if (pkt->pts == AV_NOPTS_VALUE) return SYNA_OK;
    const int64_t s = range_start(st), e = range_end(st);
    if (pkt->pts >= e) {
        done[static_cast<size_t>(si)] = true;
        return SYNA_OK;
    }
    if (pkt->pts < s) return SYNA_OK;
    return write(pkt, si, st->time_base, s);
}

// Selection bounds in a stream's time base: S (the output's zero) and OUT.
int64_t Trimmer::range_start(const AVStream* st) const {
    return vi_ >= 0 ? rescale_from_video(start_ts_, st) : av_rescale_q(p_.in_us + origin_, kMicros, st->time_base);
}

int64_t Trimmer::range_end(const AVStream* st) const {
    return vi_ >= 0 ? rescale_from_video(out_ts_, st) : av_rescale_q(p_.out_us + origin_, kMicros, st->time_base);
}

// Mix: several audio tracks become one track that mixes them all (players, editors and upload sites
// generally play only the first track). Separate: every track stays its own track. Either way each
// track enters at the level and timing the player's mixer gives it (muted or solo-silenced: gain 0,
// still present). Tracks are stream-copied unless the mixer changes them or the requested audio
// settings or the container need them encoded. When a mix cannot be made the tracks are kept
// separate and the report says why; a track is never dropped.
syna_result Trimmer::prepare_audio() {
    audio_out_.clear();
    audio_route_.assign(in_->nb_streams, -1);
    audio_delay_us_ = 0;
    report_.audio_tracks = static_cast<int>(audio_streams_.size());
    report_.audio_method = audio_streams_.empty() ? "none" : "copy";
    report_.audio_encoder.clear();
    report_.audio_note.clear();
    report_.audio_outputs.clear();
    if (audio_streams_.empty()) return SYNA_OK;
    const AudioEncodeSettings settings{p_.audio_codec, p_.audio_bitrate, p_.audio_sample_rate, p_.audio_channels};
    auto add = [&](AudioOut out) {
        for (int si : out.sources) {
            audio_route_[static_cast<size_t>(si)] = static_cast<int>(audio_out_.size());
            if (out.encoder) audio_delay_us_ = std::max(audio_delay_us_, mixer(si).offset_us);
        }
        audio_out_.push_back(std::move(out));
    };

    if (p_.audio == SYNA_TRIM_AUDIO_MIX && audio_streams_.size() > 1) {
        auto mix = std::make_unique<AudioMixdown>();
        std::vector<TrimTrack> tracks;
        for (int si : audio_streams_) tracks.push_back(mixer(si));
        std::string why;
        if (mix->open(in_.get(), tracks, ofmt_, global_header_, settings, why)) {
            report_.audio_method = "mix";
            report_.audio_encoder = mix->describe();
            report_.audio_outputs.push_back({"mix", static_cast<int>(audio_streams_.size()), mix->describe(),
                                             std::ranges::any_of(audio_streams_, [&](int si) { return adjusted(si); })});
            add({audio_streams_, std::move(mix)});
            return SYNA_OK;
        }
        report_.audio_note = "the audio tracks were kept separate: " + why;
        log::warn("trim", "audio mixdown unavailable: {}", why);
    }

    bool copied = false, encoded_any = false;
    for (int si : audio_streams_) {
        const AudioTrackInfo* src = nullptr;
        for (const AudioTrackInfo& a : info_.audio)
            if (a.stream_index == si) src = &a;
        if (can_copy_audio(si)) {
            const std::string what = src ? std::format("{} · {:g} kHz · {}{}", src->codec, src->sample_rate / 1000.0, src->channel_layout,
                                                       src->bit_rate > 0 ? std::format(" · {} kb/s", src->bit_rate / 1000) : "")
                                         : std::string();
            report_.audio_outputs.push_back({"copy", 1, what});
            add({{si}, nullptr});
            copied = true;
            continue;
        }
        auto enc = std::make_unique<AudioMixdown>();
        std::string why;
        if (!enc->open(in_.get(), {mixer(si)}, ofmt_, global_header_, settings, why)) {
            report_.error = std::format("audio track {} cannot be encoded: {}", report_.audio_outputs.size() + 1, why);
            return SYNA_E_UNSUPPORTED;
        }
        report_.audio_outputs.push_back({"encode", 1, enc->describe(), adjusted(si)});
        if (report_.audio_encoder.empty()) report_.audio_encoder = enc->describe();
        add({{si}, std::move(enc)});
        encoded_any = true;
    }
    report_.audio_method = encoded_any ? (copied ? "copy+encode" : "encode") : "copy";
    return SYNA_OK;
}

TrimTrack Trimmer::mixer(int si) const {
    for (const TrimTrack& t : p_.tracks)
        if (t.stream == si) return t;
    return {si, 1.0f, 0};
}

bool Trimmer::adjusted(int si) const {
    const TrimTrack t = mixer(si);
    return std::fabs(t.gain - 1.0f) > 1e-4f || t.offset_us != 0;
}

// A track is copied when the mixer leaves it as recorded, the request leaves its codec, rate and
// channels as they are, and the output container can store its codec.
bool Trimmer::can_copy_audio(int si) const {
    const AVCodecParameters* par = in_->streams[si]->codecpar;
    if (adjusted(si) || p_.audio_codec != SYNA_AUDIO_CODEC_AUTO) return false;
    if (p_.audio_sample_rate > 0 && p_.audio_sample_rate != par->sample_rate) return false;
    if (p_.audio_channels > 0 && p_.audio_channels != par->ch_layout.nb_channels) return false;
    const int q = avformat_query_codec(ofmt_, par->codec_id, FF_COMPLIANCE_NORMAL);
    return q == 1 || (q < 0 && ofmt_->audio_codec == par->codec_id);
}

void Trimmer::add_encoded_audio_stream(AVFormatContext* oc, AudioOut& a) {
    AVStream* os = avformat_new_stream(oc, nullptr);
    if (!os) return;
    avcodec_parameters_from_context(os->codecpar, a.encoder->encoder());
    os->time_base = a.encoder->encoder()->time_base;
    // Named after the tracks it contains when they carry names; the language is kept when they agree.
    std::string title, language;
    bool same_language = true;
    int disposition = 0;
    for (const AudioTrackInfo& t : info_.audio) {
        if (std::ranges::find(a.sources, t.stream_index) == a.sources.end()) continue;
        if (!t.title.empty()) title += (title.empty() ? "" : " + ") + t.title;
        if (language.empty()) language = t.language;
        else if (t.language != language) same_language = false;
        disposition = in_->streams[t.stream_index]->disposition;
    }
    os->disposition = a.sources.size() > 1 ? AV_DISPOSITION_DEFAULT : disposition;
    if (!title.empty()) av_dict_set(&os->metadata, "title", title.c_str(), 0);
    if (same_language && !language.empty() && language != "und") av_dict_set(&os->metadata, "language", language.c_str(), 0);
    a.out_index = os->index;
}

// Starts the encoded tracks once S is known and the output exists.
void Trimmer::begin_audio() {
    const int64_t zero = vi_ >= 0 ? ts_to_us(start_ts_, vst_->time_base) : p_.in_us + origin_;
    const int64_t end = vi_ >= 0 ? ts_to_us(out_ts_, vst_->time_base) : p_.out_us + origin_;
    for (AudioOut& a : audio_out_) {
        if (!a.encoder) continue;
        AudioMixdown* enc = a.encoder.get();
        const int index = a.out_index;
        enc->begin(zero, end - zero, [this, enc, index](AVPacket* pkt) { return write_stream(pkt, index, enc->encoder()->time_base); });
    }
}

// Audio packets in file order: copied tracks are written as they are; encoded ones are decoded and
// cut at the sample. done[si] marks streams that reached OUT (a track the mixer advances is read
// that much past OUT).
syna_result Trimmer::audio_packet(AVPacket* pkt, int si, std::vector<bool>& done) {
    AudioOut& a = audio_out_[static_cast<size_t>(audio_route_[static_cast<size_t>(si)])];
    if (!a.encoder) return write_audio_range(pkt, si, done);
    const AVStream* st = in_->streams[si];
    if (pkt->pts != AV_NOPTS_VALUE && pkt->pts >= range_end(st) + av_rescale_q(std::max<int64_t>(0, -mixer(si).offset_us), kMicros, st->time_base)) {
        done[static_cast<size_t>(si)] = true;
        return SYNA_OK;
    }
    const syna_result r = a.encoder->feed(pkt);
    if (r != SYNA_OK) report_.error = a.encoder->error();
    return r;
}

bool Trimmer::audio_done(const std::vector<bool>& done) const {
    return std::ranges::all_of(audio_streams_, [&](int si) { return done[static_cast<size_t>(si)]; });
}

syna_result Trimmer::finish_audio() {
    for (AudioOut& a : audio_out_) {
        if (!a.encoder) continue;
        if (syna_result m = a.encoder->finish(); m != SYNA_OK) {
            report_.error = a.encoder->error();
            return m;
        }
    }
    return SYNA_OK;
}

syna_result Trimmer::finish_output() {
    if (syna_result m = finish_audio(); m != SYNA_OK) return m;
    const int r = av_write_trailer(out_.get());
    out_.reset();
    if (r < 0) {
        report_.error = "cannot finalize the output: " + av_error_string(r);
        return write_error(r);
    }
    report_.actual_start = video_ts_to_us(start_ts_);
    report_.actual_end = video_ts_to_us(start_ts_ + video_end_rel_);
    return SYNA_OK;
}

CodecPtr Trimmer::open_video_decoder() {
    const AVCodec* codec = avcodec_find_decoder(vst_->codecpar->codec_id);
    if (!codec) return nullptr;
    CodecPtr ctx(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(ctx.get(), vst_->codecpar);
    ctx->pkt_timebase = vst_->time_base;
    ctx->thread_count = 0;
    ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0) return nullptr;
    return ctx;
}

// The re-encode the request asks for, resolved against the source. ORIGINAL keeps the source's
// codec, size and frame timing; ENCODE may change them; LOSSLESS keeps size and timing.
VideoTarget Trimmer::video_target(const AVCodecContext* dec) const {
    VideoTarget t;
    t.speed = p_.video_speed;
    if (p_.video == SYNA_TRIM_VIDEO_ORIGINAL) {
        t.quality = p_.video_quality > 0 ? p_.video_quality : kOriginalQuality;
        return t;
    }
    switch (p_.video_codec) {
        case SYNA_VIDEO_CODEC_H264: t.codec = AV_CODEC_ID_H264; break;
        case SYNA_VIDEO_CODEC_HEVC: t.codec = AV_CODEC_ID_HEVC; break;
        case SYNA_VIDEO_CODEC_AV1: t.codec = AV_CODEC_ID_AV1; break;
        case SYNA_VIDEO_CODEC_VP9: t.codec = AV_CODEC_ID_VP9; break;
        default: break;
    }
    if (p_.video == SYNA_TRIM_VIDEO_LOSSLESS) {
        t.lossless = true;
        return t;
    }
    t.quality = p_.video_quality > 0 ? p_.video_quality : kDefaultQuality;
    t.bitrate = p_.video_bitrate;
    t.max_bitrate = p_.video_max_bitrate;
    // Only ever smaller than the source: a larger size or a higher rate adds nothing but bytes.
    if (p_.video_height > 0 && p_.video_height < dec->height) {
        t.height = p_.video_height & ~1;
        t.width = std::max(2, static_cast<int>(std::lround(static_cast<double>(dec->width) * t.height / dec->height / 2.0)) * 2);
    }
    if (p_.video_fps.num > 0) {
        const AVRational src = vst_->avg_frame_rate.num > 0 ? vst_->avg_frame_rate : vst_->r_frame_rate;
        if (src.num <= 0 || av_cmp_q(p_.video_fps, src) < 0) t.fps = p_.video_fps;
    }
    return t;
}

// Same codec as the source when an encoder exists; otherwise the most compatible codec that the
// output container accepts (WebM only takes VP8/VP9/AV1, for example). A codec the request names
// is used or the trim fails: it is never swapped for another one silently.
std::optional<CodecPtr> Trimmer::open_encoder(const AVCodecContext* dec, const VideoTarget& t, int gop, EncoderInfo& chosen) {
    const AVCodecID source = vst_->codecpar->codec_id;
    const AVPixFmtDescriptor* sd = av_pix_fmt_desc_get(dec->pix_fmt);
    const int depth = sd ? sd->comp[0].depth : 8;
    const AVOutputFormat* ofmt = av_guess_format(nullptr, to_utf8(p_.destination).c_str(), nullptr);
    std::vector<AVCodecID> order;
    if (t.codec != AV_CODEC_ID_NONE) {
        order.push_back(t.codec);
    } else {
        order.push_back(source);
        for (AVCodecID c : {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_AV1, AV_CODEC_ID_VP9})
            if (c != source) order.push_back(c);
    }
    for (AVCodecID target : order) {
        if (target == AV_CODEC_ID_H264 && depth > 8) continue;  // no 10-bit H.264 encoder among the candidates
        if (ofmt && avformat_query_codec(ofmt, target, FF_COMPLIANCE_NORMAL) == 0) continue;
        if (auto ctx = open_encoder_for(target, dec, depth, t, gop, chosen)) {
            if (t.codec == AV_CODEC_ID_NONE && target != source) {
                const std::string why = source == AV_CODEC_ID_H264 && depth > 8
                                            ? std::string("no 10-bit H.264 encoder is available; the video was re-encoded as ") + avcodec_get_name(target)
                                            : std::string("no usable ") + avcodec_get_name(source) + (t.lossless ? " lossless" : "") +
                                                  " encoder; the video was re-encoded as " + avcodec_get_name(target);
                report_.fallback_reason += (report_.fallback_reason.empty() ? "" : "; ") + why;
            }
            return ctx;
        }
    }
    return std::nullopt;
}

std::optional<CodecPtr> Trimmer::open_encoder_for(AVCodecID target, const AVCodecContext* dec, int depth, const VideoTarget& t, int gop,
                                                  EncoderInfo& chosen) {
    for (const EncoderInfo& e : encoder_candidates(target, p_.allow_hw_encode)) {
        if (!encoder_available(e)) continue;
        if (t.lossless && !encoder_supports_lossless(e)) continue;
        const AVCodec* codec = avcodec_find_encoder_by_name(e.name.c_str());
        if (!codec) continue;
        CodecPtr ctx(avcodec_alloc_context3(codec));
        ctx->width = t.width > 0 ? t.width : dec->width;
        ctx->height = t.height > 0 ? t.height : dec->height;
        ctx->sample_aspect_ratio = vst_->sample_aspect_ratio.num ? vst_->sample_aspect_ratio : dec->sample_aspect_ratio;
        if (t.fps.num > 0) {
            ctx->time_base = av_inv_q(t.fps);  // a constant rate: one tick per frame
            ctx->framerate = t.fps;
        } else {
            ctx->time_base = vst_->time_base;  // source timestamps pass through unchanged (no CFR conversion)
            ctx->framerate = vst_->avg_frame_rate.num > 0 ? vst_->avg_frame_rate : vst_->r_frame_rate;
        }
        ctx->color_range = dec->color_range;
        ctx->colorspace = dec->colorspace;
        ctx->color_primaries = dec->color_primaries;
        ctx->color_trc = dec->color_trc;
        ctx->chroma_sample_location = dec->chroma_sample_location;
        ctx->gop_size = gop;
        ctx->max_b_frames = 0;
        if (global_header_) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        const void* fmts = nullptr;
        int nfmt = 0;
        avcodec_get_supported_config(ctx.get(), codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &fmts, &nfmt);
        const auto* list = static_cast<const AVPixelFormat*>(fmts);
        auto supported = [&](AVPixelFormat f) {
            for (int i = 0; i < nfmt; ++i)
                if (list[i] == f) return true;
            return false;
        };
        // GPU encoders work natively in NV12/P010; CPU encoders take the decoder's format when they can.
        const std::vector<AVPixelFormat> prefs = depth > 8 ? std::vector<AVPixelFormat>{AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV420P10LE}
                                                           : std::vector<AVPixelFormat>{AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P};
        AVPixelFormat pick = !e.hardware && supported(dec->pix_fmt) ? dec->pix_fmt : AV_PIX_FMT_NONE;
        for (AVPixelFormat f : prefs)
            if (pick == AV_PIX_FMT_NONE && supported(f)) pick = f;
        if (pick == AV_PIX_FMT_NONE) {
            log::info("trim", "encoder {} has no {}-bit 4:2:0 input", e.name, depth);
            continue;
        }
        ctx->pix_fmt = pick;
        apply_encoder_options(ctx.get(), e);

        // Rate control: constant quality (the request's H.264-scale value on the encoder's own
        // scale) or an average bitrate, optionally capped; the speed picks the encoder preset.
        void* priv = ctx->priv_data;
        const int q = encoder_quality(e, target, t.quality);
        const bool by_rate = t.bitrate > 0;
        const bool faster = t.speed == SYNA_ENCODER_SPEED_FASTER, smaller = t.speed == SYNA_ENCODER_SPEED_SMALLER;
        if (t.max_bitrate > 0) {
            ctx->rc_max_rate = t.max_bitrate;
            ctx->rc_buffer_size = static_cast<int>(std::min<int64_t>(t.max_bitrate * 2, INT_MAX));
        }
        if (t.lossless) {
            configure_lossless(ctx.get(), e);
            if (is(e, "nvenc")) av_opt_set_int(priv, "forced-idr", 1, 0);
            if (e.name == "libvpx-vp9") {
                av_opt_set(priv, "deadline", "good", 0);
                av_opt_set_int(priv, "cpu-used", faster ? 6 : 4, 0);
                av_opt_set_int(priv, "row-mt", 1, 0);
                ctx->thread_count = static_cast<int>(std::min(16u, std::max(1u, std::thread::hardware_concurrency())));
            }
        } else if (is(e, "nvenc")) {
            av_opt_set(priv, "preset", faster ? "p3" : smaller ? "p7" : "p5", 0);
            av_opt_set(priv, "tune", "hq", 0);
            av_opt_set(priv, "rc", "vbr", 0);
            av_opt_set_int(priv, "spatial-aq", 1, 0);
            av_opt_set_int(priv, "forced-idr", 1, 0);
            if (by_rate) {
                ctx->bit_rate = t.bitrate;
            } else {
                av_opt_set_int(priv, "cq", q, 0);
                ctx->bit_rate = 0;
            }
            if (target == AV_CODEC_ID_HEVC) av_opt_set(priv, "profile", depth > 8 ? "main10" : "main", 0);
            if (target == AV_CODEC_ID_H264) av_opt_set(priv, "profile", "high", 0);
        } else if (is(e, "amf")) {
            av_opt_set(priv, "quality", faster ? "speed" : "quality", 0);
            if (by_rate) {
                av_opt_set(priv, "rc", "vbr_peak", 0);
                ctx->bit_rate = t.bitrate;
                if (t.max_bitrate <= 0) ctx->rc_max_rate = t.bitrate * 3 / 2;
            } else {
                av_opt_set(priv, "rc", "cqp", 0);
                av_opt_set_int(priv, "qp_i", q, 0);
                av_opt_set_int(priv, "qp_p", q, 0);
            }
        } else if (is(e, "qsv")) {
            av_opt_set(priv, "preset", faster ? "fast" : smaller ? "veryslow" : "slower", 0);
            if (by_rate) ctx->bit_rate = t.bitrate;
            else ctx->global_quality = q;
        } else if (e.name == "libsvtav1") {
            av_opt_set_int(priv, "preset", faster ? 10 : smaller ? 5 : 8, 0);
            if (by_rate) ctx->bit_rate = t.bitrate;
            else av_opt_set_int(priv, "crf", q, 0);
        } else if (e.name == "libvpx-vp9") {
            av_opt_set(priv, "deadline", "good", 0);
            av_opt_set_int(priv, "cpu-used", faster ? 6 : smaller ? 2 : 4, 0);
            av_opt_set_int(priv, "row-mt", 1, 0);
            ctx->thread_count = static_cast<int>(std::min(16u, std::max(1u, std::thread::hardware_concurrency())));
            if (by_rate) {
                ctx->bit_rate = t.bitrate;
            } else {
                ctx->bit_rate = 0;
                av_opt_set_int(priv, "crf", q, 0);
            }
        } else if (e.name == "libkvazaar") {
            const char* preset = faster ? "superfast" : smaller ? "medium" : "fast";
            const std::string params = by_rate ? std::format("preset={},bitrate={}", preset, t.bitrate) : std::format("preset={},qp={}", preset, q);
            av_opt_set(priv, "kvazaar-params", params.c_str(), 0);
        } else if (is(e, "_mf")) {
            if (by_rate) {
                av_opt_set(priv, "rate_control", "u_vbr", 0);
                ctx->bit_rate = t.bitrate;
            } else {
                av_opt_set(priv, "rate_control", "quality", 0);
                av_opt_set_int(priv, "quality", q, 0);
            }
        } else {
            // OpenH264 is bitrate driven: without a requested rate, the source's rate scaled by the
            // quality (6 steps on the CQ scale halve or double it).
            const int64_t src = vst_->codecpar->bit_rate > 0 ? vst_->codecpar->bit_rate : 20000000;
            ctx->bit_rate = by_rate ? t.bitrate : static_cast<int64_t>(static_cast<double>(src) * 1.25 * std::pow(2.0, (19 - t.quality) / 6.0));
        }
        const int r = avcodec_open2(ctx.get(), codec, nullptr);
        if (r >= 0) {
            chosen = e;
            log::info("trim", "encoder {} {}x{} pix_fmt={} gop={} {} speed={}", e.name, ctx->width, ctx->height, av_get_pix_fmt_name(pick), gop,
                      t.lossless ? std::string("lossless") : by_rate ? std::format("bitrate={} max={}", t.bitrate, t.max_bitrate) : std::format("quality={}", q),
                      t.speed);
            return ctx;
        }
        log::warn("trim", "encoder {} failed to open: {}", e.name, av_error_string(r));
    }
    return std::nullopt;
}

// rel_pts and dur are in the encoder's time base, relative to S. Frames are converted to the
// encoder's pixel format and, when it is smaller, scaled to its size.
syna_result Trimmer::encode_frame(AVCodecContext* enc, AVFrame* f, int64_t rel_pts, int64_t dur, SwsContext*& sws, FramePtr& converted,
                                 std::vector<PacketPtr>* collect) {
    AVFrame* in = f;
    if (f && (f->format != enc->pix_fmt || f->width != enc->width || f->height != enc->height)) {
        const bool scale = f->width != enc->width || f->height != enc->height;
        sws = sws_getCachedContext(sws, f->width, f->height, static_cast<AVPixelFormat>(f->format), enc->width, enc->height, enc->pix_fmt,
                                   scale ? SWS_LANCZOS | SWS_ACCURATE_RND : SWS_POINT, nullptr, nullptr, nullptr);
        if (!converted) {
            converted = make_frame();
            converted->format = enc->pix_fmt;
            converted->width = enc->width;
            converted->height = enc->height;
            if (av_frame_get_buffer(converted.get(), 0) < 0) return SYNA_E_FAIL;
        }
        if (av_frame_make_writable(converted.get()) < 0 || !sws) return SYNA_E_FAIL;
        sws_scale(sws, f->data, f->linesize, 0, f->height, converted->data, converted->linesize);
        av_frame_copy_props(converted.get(), f);
        in = converted.get();
    }
    if (in) {
        in->pts = rel_pts;
        in->duration = dur;
        in->pict_type = AV_PICTURE_TYPE_NONE;
    }
    int r = avcodec_send_frame(enc, in);
    if (r < 0 && r != AVERROR_EOF) {
        report_.error = "the encoder rejected a frame: " + av_error_string(r);
        return SYNA_E_FAIL;
    }
    PacketPtr pkt = make_packet();
    while (avcodec_receive_packet(enc, pkt.get()) >= 0) {
        if (collect) {
            PacketPtr keep = make_packet();
            av_packet_move_ref(keep.get(), pkt.get());
            collect->push_back(std::move(keep));
        } else if (syna_result w = write(pkt.get(), vi_, enc->time_base, 0); w != SYNA_OK) {
            return w;
        }
    }
    return SYNA_OK;
}

syna_result Trimmer::do_audio_only() {
    if (syna_result r = create_output(nullptr, nullptr); r != SYNA_OK) return r;
    begin_audio();
    const bool any_encoded = std::ranges::any_of(audio_out_, [](const AudioOut& a) { return a.encoder != nullptr; });
    const int64_t seek_us = p_.in_us + origin_ - (any_encoded ? kMixPrerollUs + audio_delay_us_ : 0);
    avformat_seek_file(in_.get(), -1, INT64_MIN, seek_us, seek_us, 0);
    PacketPtr pkt = make_packet();
    std::vector<bool> done(in_->nb_streams, false);
    for (;;) {
        if (job_.cancelled()) return SYNA_E_CANCELLED;
        if (av_read_frame(in_.get(), pkt.get()) < 0) break;
        const int si = pkt->stream_index;
        const syna_result r = keep_stream(static_cast<unsigned>(si)) ? audio_packet(pkt.get(), si, done) : SYNA_OK;
        av_packet_unref(pkt.get());
        if (r != SYNA_OK) return r;
        if (audio_done(done)) break;
    }
    if (syna_result m = finish_audio(); m != SYNA_OK) return m;
    const int r = av_write_trailer(out_.get());
    out_.reset();
    report_.mode_used = mode_name(p_.mode);
    report_.video_method = "none";
    report_.actual_start = p_.in_us;
    report_.actual_end = p_.out_us;
    return r < 0 ? write_error(r) : SYNA_OK;
}

// Stream-copy end rule (Quick and Smart): write the decode-order prefix that contains every frame
// displayed before OUT. A packet displayed after OUT is held until a later packet shows it is
// needed as a reference (B-frames); copying stops once decoding has passed OUT. Exact for streams
// without reordering; with B-frames the prefix may add a frame displayed after OUT.
syna_result Trimmer::copy_until_out(PacketPtr v, std::deque<PacketPtr>& held, bool& done) {
    if (done) return SYNA_OK;
    if (v->pts != AV_NOPTS_VALUE && v->pts < out_ts_) {
        while (!held.empty()) {
            PacketPtr t = std::move(held.front());
            held.pop_front();
            if (syna_result r = write(t.get(), vi_, vst_->time_base, start_ts_); r != SYNA_OK) return r;
        }
        progress(v->pts);
        return write(v.get(), vi_, vst_->time_base, start_ts_);
    }
    if (v->dts != AV_NOPTS_VALUE && v->dts >= out_ts_) {
        done = true;
        held.clear();
        return SYNA_OK;
    }
    held.push_back(std::move(v));
    return SYNA_OK;
}

// Stream copy from the last keyframe at or before IN; the end follows copy_until_out.
syna_result Trimmer::do_quick() {
    if (syna_result r = create_output(nullptr, nullptr); r != SYNA_OK) return r;
    seek_video(in_ts_);
    PacketPtr pkt = make_packet();
    std::deque<PacketPtr> vbuf, tail;
    std::vector<std::deque<PacketPtr>> abuf(in_->nb_streams);
    std::vector<bool> done(in_->nb_streams, false);
    int64_t candidate = AV_NOPTS_VALUE;
    bool scanning = true, video_done = false;

    auto video_out = [&](PacketPtr v) -> syna_result { return copy_until_out(std::move(v), tail, video_done); };
    auto end_scan = [&]() -> syna_result {
        scanning = false;
        start_ts_ = candidate;
        begin_audio();
        while (!vbuf.empty()) {
            PacketPtr v = std::move(vbuf.front());
            vbuf.pop_front();
            if (syna_result r = video_out(std::move(v)); r != SYNA_OK) return r;
        }
        for (unsigned i = 0; i < in_->nb_streams; ++i) {
            while (!abuf[i].empty()) {
                PacketPtr a = std::move(abuf[i].front());
                abuf[i].pop_front();
                if (syna_result r = audio_packet(a.get(), static_cast<int>(i), done); r != SYNA_OK) return r;
            }
        }
        return SYNA_OK;
    };

    for (;;) {
        if (job_.cancelled()) return SYNA_E_CANCELLED;
        const int rr = av_read_frame(in_.get(), pkt.get());
        if (rr < 0) {
            if (rr != AVERROR_EOF) {
                report_.error = "read error: " + av_error_string(rr);
                return SYNA_E_IO;
            }
            break;
        }
        const int si = pkt->stream_index;
        if (!keep_stream(static_cast<unsigned>(si))) {
            av_packet_unref(pkt.get());
            continue;
        }
        PacketPtr p = make_packet();
        av_packet_move_ref(p.get(), pkt.get());
        if (si == vi_) {
            if (scanning) {
                const bool key = (p->flags & AV_PKT_FLAG_KEY) != 0;
                if (key && p->pts != AV_NOPTS_VALUE && p->pts <= in_ts_) {
                    candidate = p->pts;
                    vbuf.clear();
                    vbuf.push_back(std::move(p));
                    for (unsigned i = 0; i < in_->nb_streams; ++i) {
                        const int64_t s = rescale_from_video(candidate, in_->streams[i]) - preroll(in_->streams[i]);
                        while (!abuf[i].empty() && abuf[i].front()->pts < s) abuf[i].pop_front();
                    }
                    continue;
                }
                if (candidate == AV_NOPTS_VALUE) {
                    if (!key) continue;     // undecodable without a preceding keyframe
                    candidate = p->pts;     // IN lies before the first keyframe
                    vbuf.push_back(std::move(p));
                    continue;
                }
                if ((key && p->pts > in_ts_) || (p->dts != AV_NOPTS_VALUE && p->dts > in_ts_)) {
                    if (syna_result r = end_scan(); r != SYNA_OK) return r;
                } else {
                    vbuf.push_back(std::move(p));
                    continue;
                }
            }
            if (syna_result r = video_out(std::move(p)); r != SYNA_OK) return r;
        } else if (scanning) {
            if (p->pts != AV_NOPTS_VALUE && (candidate == AV_NOPTS_VALUE || p->pts >= rescale_from_video(candidate, in_->streams[si]) - preroll(in_->streams[si])))
                abuf[static_cast<size_t>(si)].push_back(std::move(p));
            continue;
        } else if (syna_result r = audio_packet(p.get(), si, done); r != SYNA_OK) {
            return r;
        }
        if (!scanning && video_done && audio_done(done)) break;
    }
    if (scanning) {
        if (candidate == AV_NOPTS_VALUE) {
            report_.error = "no keyframe found in the selection";
            return SYNA_E_FAIL;
        }
        if (syna_result r = end_scan(); r != SYNA_OK) return r;
    }
    if (syna_result r = finish_output(); r != SYNA_OK) return r;
    report_.mode_used = "quick";
    report_.video_method = "copy";
    report_.copied_s = (report_.actual_end - report_.actual_start) / 1e6;
    if (start_ts_ < in_ts_)
        report_.fallback_reason = std::format("starts at the keyframe {:.3f} s before IN", (p_.in_us - report_.actual_start) / 1e6);
    return SYNA_OK;
}

// Re-encodes [frame containing IN, OUT) keeping source timestamps.
syna_result Trimmer::do_exact() {
    CodecPtr dec = open_video_decoder();
    if (!dec) {
        report_.error = "cannot decode the source video";
        return SYNA_E_UNSUPPORTED;
    }
    target_ = video_target(dec.get());
    const double out_fps = target_.fps.num > 0 ? av_q2d(target_.fps) : info_.video.fps_avg;
    const int gop = info_.video.keyframe_interval_s > 0 && out_fps > 0 ? std::max(1, static_cast<int>(info_.video.keyframe_interval_s * out_fps + 0.5)) : 120;
    EncoderInfo chosen;
    std::optional<CodecPtr> enc = open_encoder(dec.get(), target_, gop, chosen);
    if (!enc) {
        if (target_.codec != AV_CODEC_ID_NONE)
            report_.error = std::format("no usable {}{} encoder for this output", avcodec_get_name(target_.codec), target_.lossless ? " lossless" : "");
        else if (target_.lossless)
            report_.error = "no lossless video encoder is available for this output";
        else
            report_.error = std::string("no usable encoder for ") + avcodec_get_name(vst_->codecpar->codec_id);
        return SYNA_E_UNSUPPORTED;
    }
    const AVRational enc_tb = (*enc)->time_base;
    int64_t next_slot = 0;  // constant output rate: the next frame slot to fill
    seek_video(in_ts_);
    PacketPtr pkt = make_packet();
    FramePtr frame = make_frame();
    FramePtr converted;
    SwsContext* sws = nullptr;
    ScopeExit free_sws([&] { if (sws) sws_freeContext(sws); });
    std::vector<std::deque<PacketPtr>> abuf(in_->nb_streams);
    std::vector<bool> done(in_->nb_streams, false);
    bool video_done = false;

    auto handle = [&](AVFrame* f) -> syna_result {
        const int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        if (pts == AV_NOPTS_VALUE) return SYNA_OK;
        const int64_t dur = f->duration > 0 ? f->duration : frame_ts_;
        if (pts + dur <= in_ts_) return SYNA_OK;
        if (pts >= out_ts_) {
            video_done = true;
            return SYNA_OK;
        }
        if (!out_) {
            start_ts_ = pts;
            if (syna_result r = create_output(enc->get(), nullptr); r != SYNA_OK) return r;
            begin_audio();
            for (unsigned i = 0; i < in_->nb_streams; ++i) {
                while (!abuf[i].empty()) {
                    PacketPtr a = std::move(abuf[i].front());
                    abuf[i].pop_front();
                    if (syna_result r = audio_packet(a.get(), static_cast<int>(i), done); r != SYNA_OK) return r;
                }
            }
        }
        progress(pts);
        if (target_.fps.num == 0) return encode_frame(enc->get(), f, pts - start_ts_, dur, sws, converted, nullptr);
        // Constant output rate: every slot until this frame ends (or OUT) shows this frame, so
        // frames between slots are dropped and the cadence is even.
        const int64_t end_rel = std::min(pts + dur, out_ts_) - start_ts_;
        while (av_rescale_q(next_slot, enc_tb, vst_->time_base) < end_rel) {
            if (syna_result r = encode_frame(enc->get(), f, next_slot, 1, sws, converted, nullptr); r != SYNA_OK) return r;
            ++next_slot;
        }
        return SYNA_OK;
    };

    for (;;) {
        if (job_.cancelled()) return SYNA_E_CANCELLED;
        const int rr = av_read_frame(in_.get(), pkt.get());
        if (rr < 0) {
            if (rr != AVERROR_EOF) {
                report_.error = "read error: " + av_error_string(rr);
                return SYNA_E_IO;
            }
            avcodec_send_packet(dec.get(), nullptr);
            while (!video_done && avcodec_receive_frame(dec.get(), frame.get()) >= 0) {
                const syna_result r = handle(frame.get());
                av_frame_unref(frame.get());
                if (r != SYNA_OK) return r;
            }
            break;
        }
        const int si = pkt->stream_index;
        if (!keep_stream(static_cast<unsigned>(si))) {
            av_packet_unref(pkt.get());
            continue;
        }
        if (si == vi_) {
            if (!video_done && avcodec_send_packet(dec.get(), pkt.get()) >= 0) {
                while (!video_done && avcodec_receive_frame(dec.get(), frame.get()) >= 0) {
                    const syna_result r = handle(frame.get());
                    av_frame_unref(frame.get());
                    if (r != SYNA_OK) return r;
                }
            }
            av_packet_unref(pkt.get());
        } else {
            PacketPtr p = make_packet();
            av_packet_move_ref(p.get(), pkt.get());
            if (!out_) {
                abuf[static_cast<size_t>(si)].push_back(std::move(p));
                continue;
            }
            if (syna_result r = audio_packet(p.get(), si, done); r != SYNA_OK) return r;
        }
        if (video_done && out_ && audio_done(done)) break;
    }
    if (!out_) {
        report_.error = "no frames in the selected range";
        return SYNA_E_FAIL;
    }
    if (syna_result r = encode_frame(enc->get(), nullptr, 0, 0, sws, converted, nullptr); r != SYNA_OK) return r;
    if (syna_result r = finish_output(); r != SYNA_OK) return r;
    report_.mode_used = "exact";
    report_.video_method = "reencode";
    report_.encoder = chosen.label;
    report_.hardware_encoder = chosen.hardware;
    report_.reencoded_s = (report_.actual_end - report_.actual_start) / 1e6;
    return SYNA_OK;
}

// Reasons Smart Cut cannot produce a standards-compliant splice for this source/output.
// k2: first IDR inside the selection; k2_delay: its pts - dts (the source's reorder delay there).
std::optional<std::string> Trimmer::smart_blocker(int64_t& k2, int64_t& k2_delay) {
    const AVCodecID codec = vst_->codecpar->codec_id;
    if (codec != AV_CODEC_ID_H264 && codec != AV_CODEC_ID_HEVC) return std::string("Smart Cut supports H.264 and HEVC; this video is ") + avcodec_get_name(codec);
    const uint8_t* ex = vst_->codecpar->extradata;
    if (!ex || vst_->codecpar->extradata_size < 7 || ex[0] != 1) return std::string("the source does not store parameter sets in its container header");
    const AVOutputFormat* ofmt = av_guess_format(nullptr, to_utf8(p_.destination).c_str(), nullptr);
    if (!ofmt || (!std::strstr(ofmt->name, "mp4") && !std::strstr(ofmt->name, "mov") && !std::strstr(ofmt->name, "matroska")))
        return std::string("the output container is not MP4, MOV or Matroska");
    if (std::ranges::none_of(encoder_candidates(codec, p_.allow_hw_encode), encoder_available))
        return std::string("no ") + avcodec_get_name(codec) + " encoder is available";
    const int length_size = codec == AV_CODEC_ID_H264 ? (ex[4] & 3) + 1 : (vst_->codecpar->extradata_size > 21 ? (ex[21] & 3) + 1 : 4);

    // Pre-scan packets (no decoding): find the first IDR inside the selection. With frame
    // reordering the GOP that starts there must be closed: no later packet may be displayed before
    // it, or the copied part would depend on frames that are replaced by the re-encoded head.
    seek_video(in_ts_);
    PacketPtr pkt = make_packet();
    k2 = AV_NOPTS_VALUE;
    k2_delay = 0;
    int after = 0;
    while (av_read_frame(in_.get(), pkt.get()) >= 0) {
        if (pkt->stream_index != vi_) {
            av_packet_unref(pkt.get());
            continue;
        }
        if (k2 == AV_NOPTS_VALUE) {
            const bool past = pkt->dts != AV_NOPTS_VALUE && pkt->dts >= out_ts_;
            if (!past && (pkt->flags & AV_PKT_FLAG_KEY) && pkt->pts >= in_ts_ && pkt->pts < out_ts_ && is_idr(pkt.get(), codec, length_size)) {
                k2 = pkt->pts;
                k2_delay = pkt->dts != AV_NOPTS_VALUE ? std::max<int64_t>(0, pkt->pts - pkt->dts) : 0;
            }
            av_packet_unref(pkt.get());
            if (past) break;
            continue;
        }
        const bool leading = pkt->pts != AV_NOPTS_VALUE && pkt->pts < k2;
        const bool next_gop = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        av_packet_unref(pkt.get());
        if (leading) return std::string("the keyframe inside the selection opens an open GOP (frames displayed before it depend on it)");
        if (next_gop || ++after >= 32) break;
    }
    if (k2 == AV_NOPTS_VALUE) return std::string("there is no IDR keyframe inside the selection");
    return std::nullopt;
}

// Re-encodes only [IN, K2) where K2 is the first IDR inside the selection, then copies the rest.
// The encoder's parameter sets are renumbered and stored next to the source's in avcC, so the
// result is a compliant avc1 stream rather than relying on in-band parameter set changes.
syna_result Trimmer::do_smart() {
    int64_t k2 = AV_NOPTS_VALUE, k2_delay = 0;
    if (auto why = smart_blocker(k2, k2_delay)) {
        report_.fallback_reason = *why + "; Exact Cut was used instead";
        return do_exact();
    }
    const AVCodecID codec = vst_->codecpar->codec_id;
    const AVCodecParameters* par = vst_->codecpar;
    std::vector<PacketPtr> head;
    Bytes merged;
    EncoderInfo chosen;
    bool remapped = false;

    if (k2 > in_ts_) {
        CodecPtr dec = open_video_decoder();
        if (!dec) {
            report_.fallback_reason = "cannot decode the source; Exact Cut was used instead";
            return do_exact();
        }
        VideoTarget splice;
        splice.quality = kSpliceQuality;
        std::optional<CodecPtr> enc = open_encoder(dec.get(), splice, 1 << 20, chosen);
        if (!enc || (*enc)->codec_id != codec) {
            report_.fallback_reason = "no matching encoder for the splice; Exact Cut was used instead";
            return do_exact();
        }
        const uint8_t* ee = (*enc)->extradata;
        const size_t en = static_cast<size_t>((*enc)->extradata_size);
        std::vector<Bytes> enc_ps;
        if (ee && en && is_annexb(ee, en)) enc_ps = h264::split_annexb(ee, en);
        else if (codec == AV_CODEC_ID_H264 && ee && en) {
            if (auto c = h264::parse_avcc(ee, en)) {
                enc_ps = c->sps;
                enc_ps.insert(enc_ps.end(), c->pps.begin(), c->pps.end());
            }
        } else if (ee && en) {
            if (auto v = hvcc_nals(ee, en)) enc_ps = *v;
        }

        std::optional<h264::Sps> e_sps;
        std::optional<h264::Pps> e_pps;
        int new_pps_id = 0;
        int length_size = 4;
        if (codec == AV_CODEC_ID_H264) {
            auto src = h264::parse_avcc(par->extradata, static_cast<size_t>(par->extradata_size));
            Bytes enc_sps, enc_pps;
            for (const Bytes& n : enc_ps) {
                if (h264::nal_type(n) == 7 && enc_sps.empty()) enc_sps = n;
                if (h264::nal_type(n) == 8 && enc_pps.empty()) enc_pps = n;
            }
            if (!src || src->sps.empty() || src->pps.empty() || enc_sps.empty() || enc_pps.empty()) {
                report_.fallback_reason = "cannot read the parameter sets; Exact Cut was used instead";
                return do_exact();
            }
            length_size = src->nal_length_size;
            if (enc_sps == src->sps[0] && enc_pps == src->pps[0]) {
                merged.assign(par->extradata, par->extradata + par->extradata_size);
            } else {
                int max_sps = -1, max_pps = -1;
                for (const Bytes& s : src->sps)
                    if (auto p = h264::parse_sps(s)) max_sps = std::max(max_sps, p->id);
                for (const Bytes& p : src->pps)
                    if (auto q = h264::parse_pps(p)) max_pps = std::max(max_pps, q->id);
                e_sps = h264::parse_sps(enc_sps);
                e_pps = h264::parse_pps(enc_pps);
                const int new_sps_id = max_sps + 1;
                new_pps_id = max_pps + 1;
                auto sps2 = h264::rewrite_sps_id(enc_sps, new_sps_id);
                auto pps2 = h264::rewrite_pps_ids(enc_pps, new_pps_id, new_sps_id);
                if (!e_sps || !e_pps || e_pps->num_slice_groups > 1 || !sps2 || !pps2 || new_sps_id > 31 || new_pps_id > 255) {
                    report_.fallback_reason = "the encoder's parameter sets cannot be renumbered; Exact Cut was used instead";
                    return do_exact();
                }
                h264::AvcConfig cfg = *src;
                cfg.sps.push_back(*sps2);
                cfg.pps.push_back(*pps2);
                cfg.level = std::max(cfg.level, e_sps->level_idc);
                cfg.profile = std::max(cfg.profile, e_sps->profile_idc);
                cfg.compatibility &= e_sps->constraint_flags;
                merged = h264::build_avcc(cfg);
                remapped = true;
            }
        } else {
            // HEVC: splice only when the encoder emitted the very same VPS/SPS/PPS as the source.
            auto src = hvcc_nals(par->extradata, static_cast<size_t>(par->extradata_size));
            std::vector<Bytes> a = src ? *src : std::vector<Bytes>{}, b = enc_ps;
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            if (a.empty() || a != b) {
                report_.fallback_reason = "the HEVC encoder's parameter sets differ from the source's; Exact Cut was used instead";
                return do_exact();
            }
            merged.assign(par->extradata, par->extradata + par->extradata_size);
            length_size = (par->extradata[21] & 3) + 1;
        }

        // Encode the head.
        seek_video(in_ts_);
        PacketPtr pkt = make_packet();
        FramePtr frame = make_frame();
        FramePtr converted;
        SwsContext* sws = nullptr;
        ScopeExit free_sws([&] { if (sws) sws_freeContext(sws); });
        bool head_done = false;
        auto handle = [&](AVFrame* f) -> syna_result {
            const int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            if (pts == AV_NOPTS_VALUE) return SYNA_OK;
            const int64_t dur = f->duration > 0 ? f->duration : frame_ts_;
            if (pts + dur <= in_ts_) return SYNA_OK;
            if (pts >= k2) {
                head_done = true;
                return SYNA_OK;
            }
            if (start_ts_ == AV_NOPTS_VALUE) start_ts_ = pts;
            progress(pts);
            return encode_frame(enc->get(), f, pts - start_ts_, dur, sws, converted, &head);
        };
        while (!head_done) {
            if (job_.cancelled()) return SYNA_E_CANCELLED;
            const int rr = av_read_frame(in_.get(), pkt.get());
            if (rr < 0) {
                avcodec_send_packet(dec.get(), nullptr);
                while (!head_done && avcodec_receive_frame(dec.get(), frame.get()) >= 0) {
                    const syna_result r = handle(frame.get());
                    av_frame_unref(frame.get());
                    if (r != SYNA_OK) return r;
                }
                break;
            }
            if (pkt->stream_index == vi_ && avcodec_send_packet(dec.get(), pkt.get()) >= 0) {
                while (!head_done && avcodec_receive_frame(dec.get(), frame.get()) >= 0) {
                    const syna_result r = handle(frame.get());
                    av_frame_unref(frame.get());
                    if (r != SYNA_OK) return r;
                }
            }
            av_packet_unref(pkt.get());
        }
        if (syna_result r = encode_frame(enc->get(), nullptr, 0, 0, sws, converted, &head); r != SYNA_OK) return r;
        if (start_ts_ == AV_NOPTS_VALUE || head.empty()) {
            report_.fallback_reason = "no frames before the first keyframe; Exact Cut was used instead";
            return do_exact();
        }

        // Re-packetize the head as length-prefixed NAL units; renumber slice PPS references.
        for (PacketPtr& hp : head) {
            std::vector<Bytes> nals = is_annexb(hp->data, static_cast<size_t>(hp->size))
                                          ? h264::split_annexb(hp->data, static_cast<size_t>(hp->size))
                                          : h264::split_length_prefixed(hp->data, static_cast<size_t>(hp->size), 4);
            std::vector<Bytes> outn;
            for (Bytes& n : nals) {
                if (codec == AV_CODEC_ID_H264) {
                    const int t = h264::nal_type(n);
                    if (t == 7 || t == 8 || t == 9) continue;  // parameter sets live in avcC; drop AUDs
                    if (remapped && (t == 1 || t == 5)) {
                        std::string why;
                        auto rewritten = h264::rewrite_slice_pps_id(n, *e_sps, *e_pps, new_pps_id, &why);
                        if (!rewritten) {
                            report_.fallback_reason = "slice header rewrite failed (" + why + "); Exact Cut was used instead";
                            head.clear();
                            start_ts_ = AV_NOPTS_VALUE;
                            return do_exact();
                        }
                        n = std::move(*rewritten);
                    }
                } else {
                    const int t = hevc_type(n);
                    if (t >= 32 && t <= 35) continue;  // VPS/SPS/PPS/AUD
                }
                outn.push_back(std::move(n));
            }
            replace_packet_data(hp.get(), h264::join_length_prefixed(outn, length_size));
            // The copied part keeps the source's decode delay; place the head's decode times the
            // same distance before presentation so decode order stays strictly increasing across
            // the splice (the head itself has no reordering, so any dts <= pts is valid).
            if (k2_delay > 0 && hp->pts != AV_NOPTS_VALUE) hp->dts = hp->pts - k2_delay;
        }
    } else {
        start_ts_ = k2;  // IN is exactly on a keyframe: nothing to re-encode
        merged.assign(par->extradata, par->extradata + par->extradata_size);
    }

    if (syna_result r = create_output(nullptr, &merged); r != SYNA_OK) return r;
    begin_audio();
    for (PacketPtr& hp : head) {
        if (syna_result r = write(hp.get(), vi_, vst_->time_base, 0); r != SYNA_OK) return r;
    }
    const int64_t head_frames = video_frames_;
    splice_rel_us_ = ts_to_us(k2 - start_ts_, vst_->time_base);

    // Copy the rest, from K2, with the audio from S.
    seek_video(start_ts_);
    PacketPtr pkt = make_packet();
    std::vector<bool> done(in_->nb_streams, false);
    bool video_done = false;
    std::deque<PacketPtr> held;
    for (;;) {
        if (job_.cancelled()) return SYNA_E_CANCELLED;
        const int rr = av_read_frame(in_.get(), pkt.get());
        if (rr < 0) {
            if (rr != AVERROR_EOF) {
                report_.error = "read error: " + av_error_string(rr);
                return SYNA_E_IO;
            }
            break;
        }
        const int si = pkt->stream_index;
        if (!keep_stream(static_cast<unsigned>(si))) {
            av_packet_unref(pkt.get());
            continue;
        }
        if (si == vi_) {
            // Packets before K2 in decode order belong to the replaced head (closed GOP: all of
            // them are displayed before K2).
            if (!video_done && pkt->pts != AV_NOPTS_VALUE && pkt->pts >= k2) {
                PacketPtr v = make_packet();
                av_packet_move_ref(v.get(), pkt.get());
                if (syna_result r = copy_until_out(std::move(v), held, video_done); r != SYNA_OK) return r;
            }
            av_packet_unref(pkt.get());
        } else {
            const syna_result r = audio_packet(pkt.get(), si, done);
            av_packet_unref(pkt.get());
            if (r != SYNA_OK) return r;
        }
        if (video_done && audio_done(done)) break;
    }
    if (syna_result r = finish_output(); r != SYNA_OK) return r;
    report_.mode_used = "smart";
    report_.video_method = head_frames > 0 ? "smart" : "copy";
    report_.encoder = head_frames > 0 ? chosen.label : "";
    report_.hardware_encoder = head_frames > 0 && chosen.hardware;
    report_.reencoded_s = splice_rel_us_ / 1e6;
    report_.copied_s = (report_.actual_end - report_.actual_start - splice_rel_us_) / 1e6;
    log::info("trim", "smart cut head_frames={} remapped={} splice_us={} reorder_delay={}", head_frames, remapped, splice_rel_us_, k2_delay);
    return SYNA_OK;
}

// Re-opens the result and checks it against the source and against what was written.
syna_result Trimmer::validate() {
    std::string err;
    FormatPtr f = open_input(temp_, true, &err);
    if (!f) {
        report_.issues.push_back("output cannot be opened: " + err);
        return SYNA_E_VALIDATION;
    }
    MediaInfo m;
    describe_media(f.get(), temp_, false, m);
    report_.v_duration = m.duration_us / 1e6;
    report_.v_audio_codecs.clear();
    report_.v_audio_rates.clear();
    report_.v_audio_layouts.clear();
    for (const AudioTrackInfo& a : m.audio) {
        report_.v_audio_codecs.push_back(a.codec);
        report_.v_audio_rates.push_back(a.sample_rate);
        report_.v_audio_layouts.push_back(a.channel_layout);
    }
    // Every output track is there, in order: copies keep the source format, encodes the encoded one.
    if (m.audio.size() != audio_out_.size()) {
        report_.issues.push_back(std::format("{} audio tracks in the output, expected {}", m.audio.size(), audio_out_.size()));
    } else {
        for (size_t k = 0; k < audio_out_.size(); ++k) {
            const AudioOut& a = audio_out_[k];
            if (a.encoder) {
                validate_encoded_audio(m.audio[k], a, k);
                continue;
            }
            for (const AudioTrackInfo& s : info_.audio) {
                if (s.stream_index != a.sources.front()) continue;
                if (m.audio[k].codec != s.codec || m.audio[k].sample_rate != s.sample_rate || m.audio[k].channels != s.channels)
                    report_.issues.push_back(std::format("audio track {} format changed", k + 1));
            }
        }
    }
    if (vi_ >= 0) {
        if (!m.has_video) {
            report_.issues.push_back("output has no video stream");
            return SYNA_E_VALIDATION;
        }
        report_.v_width = m.video.width;
        report_.v_height = m.video.height;
        report_.v_codec = m.video.codec;
        report_.v_fps = m.video.fps_avg;
        report_.v_frames = m.video.frame_count;
        report_.v_bit_rate = m.video.bit_rate;
        const bool reencoded = report_.video_method == "reencode";
        const int want_w = reencoded && target_.width > 0 ? target_.width : info_.video.width;
        const int want_h = reencoded && target_.height > 0 ? target_.height : info_.video.height;
        if (m.video.width != want_w || m.video.height != want_h)
            report_.issues.push_back(std::format("resolution {}×{}, expected {}×{}", m.video.width, m.video.height, want_w, want_h));
        if (m.video.frame_count > 0 && m.video.frame_count != video_frames_)
            report_.issues.push_back(std::format("{} frames in the container, {} written", m.video.frame_count, video_frames_));
        // Frame cadence must survive: the source's average rate (unless the source is VFR), or the
        // constant rate that was asked for.
        const bool fixed_rate = reencoded && target_.fps.num > 0;
        const double want_fps = fixed_rate ? av_q2d(target_.fps) : info_.video.fps_avg;
        if ((fixed_rate || !info_.video.vfr) && want_fps > 0 && m.video.fps_avg > 0 && video_frames_ > 30 && std::abs(m.video.fps_avg - want_fps) / want_fps > 0.02)
            report_.issues.push_back(std::format("frame rate {:.3f}, expected {:.3f}", m.video.fps_avg, want_fps));
        if (reencoded && target_.lossless) {
            verify_lossless();
            if (!report_.lossless_verified) report_.issues.push_back("the lossless re-encode does not decode to the source's pixels");
        }
        const double expected = (report_.actual_end - report_.actual_start) / 1e6;
        if (std::abs(report_.v_duration - expected) > std::max(0.1, 3.0 / std::max(1.0, info_.video.fps_avg)))
            report_.issues.push_back(std::format("duration {:.3f} s, expected {:.3f} s", report_.v_duration, expected));

        // Decode the beginning (covers a smart-cut splice) and check frames come out cleanly.
        const int vsi = m.video.stream_index;
        AVStream* st = f->streams[vsi];
        for (unsigned i = 0; i < f->nb_streams; ++i) f->streams[i]->discard = static_cast<int>(i) == vsi ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        CodecPtr dec(avcodec_alloc_context3(codec));
        avcodec_parameters_to_context(dec.get(), st->codecpar);
        dec->pkt_timebase = st->time_base;
        dec->thread_count = 0;
        if (!codec || avcodec_open2(dec.get(), codec, nullptr) < 0) {
            report_.issues.push_back("output video cannot be decoded");
        } else {
            const int64_t check_us = splice_rel_us_ + 1500000;
            PacketPtr pkt = make_packet();
            FramePtr frame = make_frame();
            int64_t decoded = 0, corrupt = 0, sent = 0;
            int64_t first_pts = AV_NOPTS_VALUE;
            bool stop = false;
            while (!stop && av_read_frame(f.get(), pkt.get()) >= 0) {
                if (pkt->stream_index == vsi) {
                    if (pkt->pts != AV_NOPTS_VALUE && ts_to_us(pkt->pts, st->time_base) > check_us + 500000) stop = true;
                    else {
                        ++sent;
                        if (avcodec_send_packet(dec.get(), pkt.get()) < 0) ++corrupt;
                    }
                }
                av_packet_unref(pkt.get());
                while (avcodec_receive_frame(dec.get(), frame.get()) >= 0) {
                    if (first_pts == AV_NOPTS_VALUE) first_pts = frame->best_effort_timestamp;
                    if (frame->flags & AV_FRAME_FLAG_CORRUPT || frame->decode_error_flags) ++corrupt;
                    ++decoded;
                    av_frame_unref(frame.get());
                }
            }
            avcodec_send_packet(dec.get(), nullptr);
            while (avcodec_receive_frame(dec.get(), frame.get()) >= 0) {
                if (frame->flags & AV_FRAME_FLAG_CORRUPT || frame->decode_error_flags) ++corrupt;
                ++decoded;
                av_frame_unref(frame.get());
            }
            const int64_t start = f->start_time != AV_NOPTS_VALUE ? f->start_time : 0;
            report_.v_first_pts = first_pts == AV_NOPTS_VALUE ? 0 : ts_to_us(first_pts, st->time_base) - start;
            if (corrupt > 0) report_.issues.push_back(std::format("{} frames decoded with errors", corrupt));
            if (decoded != sent) report_.issues.push_back(std::format("decoded {} of {} checked frames", decoded, sent));
        }
    }
    report_.v_passed = report_.issues.empty();
    return report_.v_passed ? SYNA_OK : SYNA_E_VALIDATION;
}

// An encoded track (the mix, or a track that had to be re-encoded): in the encoded format, as long
// as what was encoded, and decodable from the start.
void Trimmer::validate_encoded_audio(const AudioTrackInfo& a, const AudioOut& out, size_t ordinal) {
    const std::string which = out.sources.size() > 1 ? std::string("the mixed audio track") : std::format("audio track {}", ordinal + 1);
    const AVCodecContext* e = out.encoder->encoder();
    if (a.codec != avcodec_get_name(e->codec_id) || a.sample_rate != e->sample_rate || a.channels != e->ch_layout.nb_channels)
        report_.issues.push_back(std::format("{} is {} {} Hz {} ch, not the encoded format", which, a.codec, a.sample_rate, a.channels));
    const double written = out.encoder->written_us() / 1e6;
    if (a.duration_us > 0 && std::abs(a.duration_us / 1e6 - written) > 0.05)
        report_.issues.push_back(std::format("{} lasts {:.3f} s but {:.3f} s were written", which, a.duration_us / 1e6, written));

    std::string err;
    FormatPtr f = open_input(temp_, false, &err);
    if (!f) return;  // reported by validate()
    AVStream* st = f->streams[a.stream_index];
    for (unsigned i = 0; i < f->nb_streams; ++i) f->streams[i]->discard = static_cast<int>(i) == a.stream_index ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    CodecPtr dec(codec ? avcodec_alloc_context3(codec) : nullptr);
    if (dec) {
        avcodec_parameters_to_context(dec.get(), st->codecpar);
        dec->pkt_timebase = st->time_base;
    }
    if (!dec || avcodec_open2(dec.get(), codec, nullptr) < 0) {
        report_.issues.push_back(which + " cannot be decoded");
        return;
    }
    const int64_t check_us = splice_rel_us_ + 2000000;
    PacketPtr pkt = make_packet();
    FramePtr frame = make_frame();
    int64_t samples = 0, errors = 0;
    while (av_read_frame(f.get(), pkt.get()) >= 0) {
        const bool past = pkt->pts != AV_NOPTS_VALUE && ts_to_us(pkt->pts, st->time_base) > check_us;
        if (!past && pkt->stream_index == a.stream_index && avcodec_send_packet(dec.get(), pkt.get()) < 0) ++errors;
        av_packet_unref(pkt.get());
        while (avcodec_receive_frame(dec.get(), frame.get()) >= 0) {
            if (frame->decode_error_flags) ++errors;
            samples += frame->nb_samples;
            av_frame_unref(frame.get());
        }
        if (past) break;
    }
    if (errors > 0) report_.issues.push_back(std::format("{} packets of {} failed to decode", errors, which));
    if (samples == 0 && written > 0) report_.issues.push_back(which + " decodes to nothing");
}

// A lossless re-encode must decode to exactly the source's pixels. The first frames are compared
// plane by plane, so any conversion on the way (a range or chroma change) is caught here.
void Trimmer::verify_lossless() {
    constexpr size_t kFrames = 3;
    auto first_frames = [](AVFormatContext* fmt, int stream, int64_t from_ts, std::vector<FramePtr>& out) {
        AVStream* st = fmt->streams[stream];
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        CodecPtr dec(codec ? avcodec_alloc_context3(codec) : nullptr);
        if (!dec || avcodec_parameters_to_context(dec.get(), st->codecpar) < 0) return;
        dec->pkt_timebase = st->time_base;
        if (avcodec_open2(dec.get(), codec, nullptr) < 0) return;
        PacketPtr pkt = make_packet();
        FramePtr frame = make_frame();
        auto take = [&] {
            while (out.size() < kFrames && avcodec_receive_frame(dec.get(), frame.get()) >= 0) {
                if (from_ts != AV_NOPTS_VALUE && frame->best_effort_timestamp < from_ts) {
                    av_frame_unref(frame.get());
                    continue;
                }
                out.push_back(make_frame());
                av_frame_move_ref(out.back().get(), frame.get());
            }
        };
        while (out.size() < kFrames && av_read_frame(fmt, pkt.get()) >= 0) {
            if (pkt->stream_index == stream) avcodec_send_packet(dec.get(), pkt.get());
            av_packet_unref(pkt.get());
            take();
        }
        avcodec_send_packet(dec.get(), nullptr);
        take();
    };
    auto same = [](const AVFrame& a, const AVFrame& b) {
        if (a.format != b.format || a.width != b.width || a.height != b.height) return false;
        const auto fmt = static_cast<AVPixelFormat>(a.format);
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
        if (!d) return false;
        for (int p = 0; p < av_pix_fmt_count_planes(fmt); ++p) {
            const int bytes = av_image_get_linesize(fmt, a.width, p);
            const int rows = p == 1 || p == 2 ? AV_CEIL_RSHIFT(a.height, d->log2_chroma_h) : a.height;
            for (int y = 0; y < rows; ++y)
                if (std::memcmp(a.data[p] + static_cast<ptrdiff_t>(y) * a.linesize[p], b.data[p] + static_cast<ptrdiff_t>(y) * b.linesize[p], static_cast<size_t>(bytes)) != 0)
                    return false;
        }
        return true;
    };
    std::string err;
    FormatPtr f = open_input(temp_, true, &err);
    if (!f) return;
    const int vs = best_video_stream(f.get());
    if (vs < 0) return;
    std::vector<FramePtr> got, want;
    first_frames(f.get(), vs, AV_NOPTS_VALUE, got);
    seek_video(start_ts_);
    first_frames(in_.get(), vi_, start_ts_, want);
    const size_t n = std::min(got.size(), want.size());
    bool equal = n > 0;
    for (size_t i = 0; equal && i < n; ++i) equal = same(*got[i], *want[i]);
    report_.lossless_verified = equal;
    log::info("trim", "lossless check: {} of {} frames compared, {}", n, kFrames, equal ? "identical" : "DIFFERENT");
}

syna_result Trimmer::run() {
    const int64_t t0 = qpc_now();
    report_.requested_mode = mode_name(p_.mode);
    report_.requested_in = p_.in_us;
    report_.requested_out = p_.out_us;
    if (p_.out_us <= p_.in_us || p_.in_us < 0) {
        report_.error = "the selection is empty";
        return SYNA_E_INVALID_ARG;
    }
    std::error_code ec;
    if (std::filesystem::exists(p_.destination, ec)) {
        report_.error = "the destination file already exists";
        return SYNA_E_BUSY;
    }
    if (std::filesystem::equivalent(p_.source, p_.destination, ec)) {
        report_.error = "the destination is the source";
        return SYNA_E_INVALID_ARG;
    }
    temp_ = p_.destination + L".partial";
    DeleteFileW(temp_.c_str());
    ScopeExit cleanup([this] { DeleteFileW(temp_.c_str()); });
    const AVOutputFormat* ofmt = av_guess_format(nullptr, to_utf8(p_.destination).c_str(), nullptr);
    if (!ofmt) {
        report_.error = "unsupported output extension";
        return SYNA_E_UNSUPPORTED;
    }
    global_header_ = (ofmt->flags & AVFMT_GLOBALHEADER) != 0;
    ofmt_ = ofmt;

    if (syna_result r = open_source(); r != SYNA_OK) return r;
    if (vi_ >= 0 && in_ts_ >= out_ts_) {
        report_.error = "the selection is outside the video";
        return SYNA_E_INVALID_ARG;
    }

    auto attempt = [&](int mode) -> syna_result {
        reset_output_state();
        report_.reset_method();
        if (syna_result a = prepare_audio(); a != SYNA_OK) return a;
        if (vi_ < 0) return do_audio_only();
        switch (mode) {
            case SYNA_TRIM_QUICK: return do_quick();
            case SYNA_TRIM_SMART: return do_smart();
            default: return do_exact();
        }
    };
    // Settings that change the video re-encode all of it: nothing is left to copy, so every mode
    // cuts frame-accurately, like Exact Cut.
    int mode = p_.mode;
    report_.video_handling = video_handling_name(p_.video);
    report_.lossless_video = p_.video == SYNA_TRIM_VIDEO_LOSSLESS;
    if (vi_ >= 0 && p_.video != SYNA_TRIM_VIDEO_ORIGINAL && mode != SYNA_TRIM_EXACT) {
        report_.fallback_reason = std::format("the output settings re-encode the whole video, so {} cut it frame-accurately like Exact Cut",
                                              mode == SYNA_TRIM_QUICK ? "Quick Cut" : "Smart Cut");
        mode = SYNA_TRIM_EXACT;
    }
    syna_result r = attempt(mode);
    if (r == SYNA_OK) {
        job_.set_progress(0.92);
        r = validate();
        if (r != SYNA_OK && report_.mode_used == "smart") {
            log::warn("trim", "smart cut failed validation; retrying with exact cut");
            std::string issues;
            for (const auto& i : report_.issues) issues += (issues.empty() ? "" : "; ") + i;
            report_.fallback_reason = "the spliced result failed validation (" + issues + "); Exact Cut was used instead";
            r = attempt(SYNA_TRIM_EXACT);
            if (r == SYNA_OK) r = validate();
        }
        if (r == SYNA_E_VALIDATION && report_.error.empty()) report_.error = "the output failed validation";
    }
    if (r == SYNA_OK) {
        out_.reset();
        if (!MoveFileExW(temp_.c_str(), p_.destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            const DWORD e = GetLastError();
            report_.error = std::format("cannot move the result into place (error {})", e);
            r = e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS ? SYNA_E_BUSY : SYNA_E_IO;
        } else {
            report_.success = true;
            report_.output = to_utf8(p_.destination);
        }
    }
    if (r == SYNA_E_CANCELLED) report_.error = "cancelled";
    out_.reset();
    report_.elapsed_s = qpc_to_ms(qpc_now() - t0) / 1000.0;
    log::info("trim", "done result={} requested={} used={} method={} encoder=\"{}\" elapsed_s={:.2f} error=\"{}\"", r, report_.requested_mode,
              report_.mode_used, report_.video_method, report_.encoder, report_.elapsed_s, report_.error);
    return r;
}

}  // namespace

syna_result run_trim(const TrimParams& params, Job& job) {
    Trimmer t(params, job);
    const syna_result r = t.run();
    job.set_result_json(t.report_json());
    return r;
}

}  // namespace syna
