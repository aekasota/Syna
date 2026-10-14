#include "media/media_info.h"

#include "common/json.h"
#include "common/log.h"
#include "common/util.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace syna {
namespace {

std::string safe(const char* s) { return s ? s : ""; }

std::string metadata(AVDictionary* dict, const char* key) {
    AVDictionaryEntry* e = av_dict_get(dict, key, nullptr, 0);
    return e && e->value ? e->value : "";
}

// MP4 handler names identify the muxer, not the content ("SoundHandler", ShadowPlay's "simple_aac").
bool is_generic_handler(const std::string& name) {
    static const char* generic[] = {"SoundHandle", "Core Media Audio", "Audio", "Stereo", "simple_aac"};
    for (const char* g : generic) {
        if (_stricmp(name.c_str(), g) == 0) return true;
    }
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("handler") != std::string::npos;
}

std::string container_display_name(AVFormatContext* fmt, const std::wstring& path) {
    std::wstring ext = std::filesystem::path(path).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    std::string name = fmt->iformat ? fmt->iformat->name : "";
    if (name.find("mp4") != std::string::npos || name.find("mov") != std::string::npos) {
        if (ext == L".mov") return "QuickTime";
        return "MP4";
    }
    if (name.find("matroska") != std::string::npos) return ext == L".webm" ? "WebM" : "Matroska";
    return name;
}

const char* chroma_name(const AVPixFmtDescriptor* d) {
    if (!d || d->nb_components < 3 || (d->flags & AV_PIX_FMT_FLAG_RGB)) return "";
    if (d->log2_chroma_w == 1 && d->log2_chroma_h == 1) return "4:2:0";
    if (d->log2_chroma_w == 1 && d->log2_chroma_h == 0) return "4:2:2";
    if (d->log2_chroma_w == 0 && d->log2_chroma_h == 0) return "4:4:4";
    return "";
}

void describe_video(AVFormatContext* fmt, AVStream* st, bool full, VideoInfo& v) {
    AVCodecParameters* par = st->codecpar;
    v.stream_index = st->index;
    v.codec = avcodec_get_name(par->codec_id);
    if (const AVCodecDescriptor* cd = avcodec_descriptor_get(par->codec_id)) v.codec_long = safe(cd->long_name);
    v.profile = safe(avcodec_profile_name(par->codec_id, par->profile));
    v.level = par->level;
    v.width = par->width;
    v.height = par->height;
    v.sar = par->sample_aspect_ratio.num > 0 ? par->sample_aspect_ratio : AVRational{1, 1};
    if (par->format >= 0) {
        auto pf = static_cast<AVPixelFormat>(par->format);
        v.pixel_format = safe(av_get_pix_fmt_name(pf));
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(pf);
        if (d) v.bit_depth = d->comp[0].depth;
        v.chroma = chroma_name(d);
    }
    if (v.bit_depth == 0 && par->bits_per_raw_sample > 0) v.bit_depth = par->bits_per_raw_sample;
    v.color_range = par->color_range == AVCOL_RANGE_UNSPECIFIED ? "" : safe(av_color_range_name(par->color_range));
    v.color_space = par->color_space == AVCOL_SPC_UNSPECIFIED ? "" : safe(av_color_space_name(par->color_space));
    v.color_primaries = par->color_primaries == AVCOL_PRI_UNSPECIFIED ? "" : safe(av_color_primaries_name(par->color_primaries));
    v.color_transfer = par->color_trc == AVCOL_TRC_UNSPECIFIED ? "" : safe(av_color_transfer_name(par->color_trc));
    if (par->color_trc == AVCOL_TRC_SMPTE2084) v.hdr = "HDR10 (PQ)";
    else if (par->color_trc == AVCOL_TRC_ARIB_STD_B67) v.hdr = "HLG";
    v.fps_avg = st->avg_frame_rate.num > 0 ? av_q2d(st->avg_frame_rate) : 0;
    v.fps_nominal = st->r_frame_rate.num > 0 ? av_q2d(st->r_frame_rate) : 0;
    v.bit_rate = par->bit_rate;
    v.has_b_frames = par->video_delay;
    v.frame_count = st->nb_frames;
    v.start_us = st->start_time == AV_NOPTS_VALUE ? 0 : ts_to_us(st->start_time, st->time_base);

    if (const AVPacketSideData* sd = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX)) {
        double r = av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data));
        if (std::isfinite(r)) v.rotation = -r;
    }

    // The demuxer index (MP4: every sample; Matroska: cues) gives cadence and keyframes without decoding.
    int n = avformat_index_get_entries_count(st);
    const bool mp4 = fmt->iformat && std::string(fmt->iformat->name).find("mp4") != std::string::npos;
    if (v.frame_count <= 0 && n > 1 && mp4) v.frame_count = n;
    // MP4 indexes decode times; with frame reordering a keyframe is displayed some time after it is
    // decoded, so shift keyframes to their display time (Matroska cues already are display times).
    // Recorders write a constant decode delay, which the first sample shows: the stream's first
    // display time minus its first decode time. The reorder depth (video_delay) can understate it:
    // NVENC with B-frames as references decodes three frames ahead and reports two. A gap of more
    // than a second is not a decode delay (an edit list that starts the clip later), so the reorder
    // depth is used then.
    int64_t key_offset = 0;
    const AVRational rate = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
    if (mp4) {
        const AVIndexEntry* first = n > 0 ? avformat_index_get_entry(st, 0) : nullptr;
        const int64_t measured = first && st->start_time != AV_NOPTS_VALUE ? st->start_time - first->timestamp : -1;
        if (measured >= 0 && ts_to_us(measured, st->time_base) <= 1000000) key_offset = measured;
        else if (par->video_delay > 0 && rate.num > 0) key_offset = av_rescale_q(par->video_delay, av_inv_q(rate), st->time_base);
    }
    if (n > 1) {
        int64_t prev = AV_NOPTS_VALUE;
        int64_t min_d = INT64_MAX, max_d = 0;
        std::vector<int64_t> keys;
        int non_key = 0;
        for (int i = 0; i < n; ++i) {
            const AVIndexEntry* e = avformat_index_get_entry(st, i);
            if (!e) continue;
            if (e->flags & AVINDEX_KEYFRAME) keys.push_back(ts_to_us(e->timestamp + key_offset, st->time_base));
            else ++non_key;
            if (prev != AV_NOPTS_VALUE && e->timestamp > prev) {
                min_d = std::min(min_d, e->timestamp - prev);
                max_d = std::max(max_d, e->timestamp - prev);
            }
            prev = e->timestamp;
        }
        if (non_key > 0 && max_d > 0) {
            // Only meaningful when the index lists every frame (not just cues).
            v.frame_interval_min_ms = ts_to_us(min_d, st->time_base) / 1000.0;
            v.frame_interval_max_ms = ts_to_us(max_d, st->time_base) / 1000.0;
            double mean = v.fps_avg > 0 ? 1000.0 / v.fps_avg : 0;
            v.vfr = mean > 0 && (v.frame_interval_max_ms - v.frame_interval_min_ms) > mean * 0.05;
        }
        if (keys.size() > 1) {
            std::vector<int64_t> gaps;
            for (size_t i = 1; i < keys.size(); ++i) gaps.push_back(keys[i] - keys[i - 1]);
            std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
            v.keyframe_interval_s = gaps[gaps.size() / 2] / 1e6;
        }
        if (full) v.keyframes_us = std::move(keys);
    }
    if (!v.vfr && v.fps_avg > 0 && v.fps_nominal > 0 && std::abs(v.fps_avg - v.fps_nominal) / v.fps_nominal > 0.02) v.vfr = true;
}

void describe_audio(AVStream* st, AudioTrackInfo& a) {
    AVCodecParameters* par = st->codecpar;
    a.stream_index = st->index;
    a.codec = avcodec_get_name(par->codec_id);
    a.profile = safe(avcodec_profile_name(par->codec_id, par->profile));
    a.sample_rate = par->sample_rate;
    a.channels = par->ch_layout.nb_channels;
    char layout[128] = {};
    if (av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout)) > 0) a.channel_layout = layout;
    if (par->format >= 0) a.sample_format = safe(av_get_sample_fmt_name(static_cast<AVSampleFormat>(par->format)));
    a.bits_per_sample = par->bits_per_raw_sample > 0 ? par->bits_per_raw_sample : par->bits_per_coded_sample;
    a.bit_rate = par->bit_rate;
    a.language = metadata(st->metadata, "language");
    if (a.language == "und") a.language.clear();
    a.title = metadata(st->metadata, "title");
    if (a.title.empty()) a.title = metadata(st->metadata, "name");
    if (a.title.empty()) {
        std::string handler = metadata(st->metadata, "handler_name");
        if (!handler.empty() && !is_generic_handler(handler)) a.title = handler;
    }
    a.start_us = st->start_time == AV_NOPTS_VALUE ? 0 : ts_to_us(st->start_time, st->time_base);
    a.duration_us = st->duration == AV_NOPTS_VALUE ? 0 : ts_to_us(st->duration, st->time_base);
}

}  // namespace

void infer_track_labels(std::vector<AudioTrackInfo>& tracks) {
    for (size_t i = 0; i < tracks.size(); ++i) {
        AudioTrackInfo& t = tracks[i];
        if (!t.title.empty()) {
            t.label = t.title;
            t.label_is_suggestion = false;
            continue;
        }
        t.label_is_suggestion = true;
        if (tracks.size() == 1) t.label = "Audio";
        else if (tracks.size() == 2) t.label = i == 0 ? "Desktop" : "Microphone";  // ShadowPlay's layout
        else t.label = "Track " + std::to_string(i + 1);
    }
}

void describe_media(AVFormatContext* fmt, const std::wstring& path, bool full, MediaInfo& out) {
    out.path = path;
    out.container = container_display_name(fmt, path);
    out.container_long = fmt->iformat ? safe(fmt->iformat->long_name) : "";
    out.duration_us = media_duration_us(fmt);
    out.bit_rate = fmt->bit_rate;
    out.start_us = fmt->start_time == AV_NOPTS_VALUE ? 0 : fmt->start_time;
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    out.size_bytes = ec ? 0 : static_cast<int64_t>(size);

    int vi = best_video_stream(fmt);
    if (vi >= 0) {
        out.has_video = true;
        describe_video(fmt, fmt->streams[vi], full, out.video);
        // Everything exposed to the UI is on the file timeline (container start = 0).
        for (int64_t& k : out.video.keyframes_us) k -= out.start_us;
        out.video.start_us -= out.start_us;
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        if (st->codecpar->ch_layout.nb_channels <= 0 && st->codecpar->sample_rate <= 0) continue;
        AudioTrackInfo a;
        describe_audio(st, a);
        out.audio.push_back(std::move(a));
    }
    infer_track_labels(out.audio);
}

std::string media_info_json(const MediaInfo& m) {
    JsonWriter w;
    w.begin_object();
    w.field("path", to_utf8(m.path));
    w.field("container", m.container);
    w.field("containerLong", m.container_long);
    w.field("durationUs", m.duration_us);
    w.field("sizeBytes", m.size_bytes);
    w.field("bitRate", m.bit_rate);
    w.field("startUs", m.start_us);
    w.key("video");
    if (m.has_video) {
        const VideoInfo& v = m.video;
        w.begin_object();
        w.field("streamIndex", v.stream_index);
        w.field("codec", v.codec);
        w.field("codecLong", v.codec_long);
        w.field("profile", v.profile);
        w.field("level", v.level);
        w.field("width", v.width);
        w.field("height", v.height);
        w.field("sarNum", v.sar.num);
        w.field("sarDen", v.sar.den);
        w.field("pixelFormat", v.pixel_format);
        w.field("bitDepth", v.bit_depth);
        w.field("chroma", v.chroma);
        w.field("colorRange", v.color_range);
        w.field("colorSpace", v.color_space);
        w.field("colorPrimaries", v.color_primaries);
        w.field("colorTransfer", v.color_transfer);
        w.field("hdr", v.hdr);
        w.field("fpsAvg", v.fps_avg);
        w.field("fpsNominal", v.fps_nominal);
        w.field("vfr", v.vfr);
        w.field("frameIntervalMinMs", v.frame_interval_min_ms);
        w.field("frameIntervalMaxMs", v.frame_interval_max_ms);
        w.field("frameCount", v.frame_count);
        w.field("bitRate", v.bit_rate);
        w.field("hasBFrames", v.has_b_frames);
        w.field("rotation", v.rotation);
        w.field("keyframeIntervalS", v.keyframe_interval_s);
        w.field("startUs", v.start_us);
        w.key("keyframesUs").begin_array();
        for (int64_t k : v.keyframes_us) w.value(k);
        w.end_array();
        w.end_object();
    } else {
        w.null();
    }
    w.key("audio").begin_array();
    for (const AudioTrackInfo& a : m.audio) {
        w.begin_object();
        w.field("streamIndex", a.stream_index);
        w.field("codec", a.codec);
        w.field("profile", a.profile);
        w.field("sampleRate", a.sample_rate);
        w.field("channels", a.channels);
        w.field("channelLayout", a.channel_layout);
        w.field("sampleFormat", a.sample_format);
        w.field("bitsPerSample", a.bits_per_sample);
        w.field("bitRate", a.bit_rate);
        w.field("title", a.title);
        w.field("language", a.language);
        w.field("label", a.label);
        w.field("labelIsSuggestion", a.label_is_suggestion);
        w.field("startUs", a.start_us);
        w.field("durationUs", a.duration_us);
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return w.str();
}

syna_result probe_media(const std::wstring& path, bool full, MediaInfo& out) {
    std::string err;
    int av_err = 0;
    FormatPtr fmt = open_input(path, full, &err, &av_err);
    if (!fmt) {
        set_last_error(err);
        return av_err ? map_av_error(av_err) : SYNA_E_OPEN;
    }
    describe_media(fmt.get(), path, full, out);
    return SYNA_OK;
}

}  // namespace syna
