#include "media/waveform.h"

#include "common/ffmpeg.h"
#include "common/log.h"
#include "common/util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace syna {
namespace {

struct TrackEnvelope {
    int stream_index = -1;
    AVRational time_base{1, 1};
    CodecPtr decoder;
    SwrPtr swr;
    int channels = 0;
    int sample_rate = 0;
    std::vector<float> peak;
    std::vector<double> energy;
    std::vector<uint32_t> count;
    std::vector<float> scratch;
    int64_t next_sample = INT64_MIN;
};

void accumulate(TrackEnvelope& t, const float* data, int frames, int64_t first_sample, int bps) {
    const int64_t buckets = static_cast<int64_t>(t.peak.size());
    for (int i = 0; i < frames; ++i) {
        const int64_t n = first_sample + i;
        if (n < 0) continue;
        const int64_t b = n * bps / t.sample_rate;
        if (b >= buckets) break;
        const float* s = data + static_cast<size_t>(i) * t.channels;
        float m = 0;
        double e = 0;
        for (int c = 0; c < t.channels; ++c) {
            m = std::max(m, std::fabs(s[c]));
            e += static_cast<double>(s[c]) * s[c];
        }
        t.peak[static_cast<size_t>(b)] = std::max(t.peak[static_cast<size_t>(b)], m);
        t.energy[static_cast<size_t>(b)] += e / t.channels;
        t.count[static_cast<size_t>(b)] += 1;
    }
}

void consume_frame(TrackEnvelope& t, AVFrame* frame, int64_t origin_us, int bps) {
    if (!t.swr) {
        SwrContext* s = nullptr;
        if (swr_alloc_set_opts2(&s, &frame->ch_layout, AV_SAMPLE_FMT_FLT, frame->sample_rate, &frame->ch_layout,
                                static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr) < 0 ||
            swr_init(s) < 0) {
            swr_free(&s);
            return;
        }
        t.swr.reset(s);
        t.channels = frame->ch_layout.nb_channels;
        t.sample_rate = frame->sample_rate;
    }
    if (frame->sample_rate != t.sample_rate || frame->ch_layout.nb_channels != t.channels) return;
    t.scratch.resize(static_cast<size_t>(frame->nb_samples) * t.channels);
    uint8_t* out = reinterpret_cast<uint8_t*>(t.scratch.data());
    const int n = swr_convert(t.swr.get(), &out, frame->nb_samples, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
    if (n <= 0) return;
    const int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
    int64_t first = t.next_sample;
    if (ts != AV_NOPTS_VALUE) first = av_rescale(ts_to_us(ts, t.time_base) - origin_us, t.sample_rate, 1000000);
    if (first == INT64_MIN) first = 0;
    accumulate(t, t.scratch.data(), n, first, bps);
    t.next_sample = first + n;
}

bool write_le(FILE* f, const void* data, size_t size) { return fwrite(data, 1, size, f) == size; }

}  // namespace

syna_result extract_waveform(const std::wstring& path, const std::wstring& out_file, int bps, Job& job) {
    bps = std::clamp(bps, 10, 1000);
    std::string err;
    int av_err = 0;
    FormatPtr fmt = open_input(path, false, &err, &av_err);
    if (!fmt) {
        set_last_error(err);
        return av_err ? map_av_error(av_err) : SYNA_E_OPEN;
    }
    const int64_t origin = fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time : 0;
    const int64_t duration = media_duration_us(fmt.get());
    const int64_t buckets = duration * bps / 1000000 + 1;

    std::vector<TrackEnvelope> tracks;
    std::vector<int> map(fmt->nb_streams, -1);
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        st->discard = AVDISCARD_ALL;
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO || tracks.size() >= SYNA_MAX_AUDIO_TRACKS) continue;
        if (st->codecpar->ch_layout.nb_channels <= 0 && st->codecpar->sample_rate <= 0) continue;
        TrackEnvelope t;
        t.stream_index = static_cast<int>(i);
        t.time_base = st->time_base;
        t.peak.assign(static_cast<size_t>(buckets), 0.0f);
        t.energy.assign(static_cast<size_t>(buckets), 0.0);
        t.count.assign(static_cast<size_t>(buckets), 0);
        if (const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id)) {
            CodecPtr ctx(avcodec_alloc_context3(codec));
            avcodec_parameters_to_context(ctx.get(), st->codecpar);
            ctx->pkt_timebase = st->time_base;
            ctx->thread_count = 1;
            if (avcodec_open2(ctx.get(), codec, nullptr) >= 0) {
                t.decoder = std::move(ctx);
                st->discard = AVDISCARD_DEFAULT;
                map[i] = static_cast<int>(tracks.size());
            }
        }
        tracks.push_back(std::move(t));
    }

    PacketPtr pkt = make_packet();
    FramePtr frame = make_frame();
    int packets = 0;
    while (true) {
        if (job.cancelled()) return SYNA_E_CANCELLED;
        int r = av_read_frame(fmt.get(), pkt.get());
        if (r < 0) break;
        const int ti = pkt->stream_index < static_cast<int>(map.size()) ? map[static_cast<size_t>(pkt->stream_index)] : -1;
        if (ti >= 0) {
            TrackEnvelope& t = tracks[static_cast<size_t>(ti)];
            if (avcodec_send_packet(t.decoder.get(), pkt.get()) >= 0) {
                while (avcodec_receive_frame(t.decoder.get(), frame.get()) >= 0) {
                    consume_frame(t, frame.get(), origin, bps);
                    av_frame_unref(frame.get());
                }
            }
            if (duration > 0 && (++packets & 63) == 0 && pkt->pts != AV_NOPTS_VALUE)
                job.set_progress(static_cast<double>(ts_to_us(pkt->pts, t.time_base) - origin) / duration);
        }
        av_packet_unref(pkt.get());
    }
    for (TrackEnvelope& t : tracks) {
        if (!t.decoder) continue;
        avcodec_send_packet(t.decoder.get(), nullptr);
        while (avcodec_receive_frame(t.decoder.get(), frame.get()) >= 0) {
            consume_frame(t, frame.get(), origin, bps);
            av_frame_unref(frame.get());
        }
    }

    const std::wstring temp = out_file + L".partial";
    FILE* f = nullptr;
    if (_wfopen_s(&f, temp.c_str(), L"wb") != 0 || !f) {
        set_last_error("cannot write waveform cache");
        return SYNA_E_IO;
    }
    bool ok = write_le(f, "SYNW", 4);
    const uint32_t version = 1, count = static_cast<uint32_t>(tracks.size()), rate = static_cast<uint32_t>(bps);
    ok = ok && write_le(f, &version, 4) && write_le(f, &count, 4) && write_le(f, &rate, 4) && write_le(f, &duration, 8);
    std::vector<uint16_t> row;
    for (const TrackEnvelope& t : tracks) {
        const uint32_t n = static_cast<uint32_t>(t.peak.size());
        ok = ok && write_le(f, &n, 4);
        row.resize(static_cast<size_t>(n) * 2);
        for (uint32_t b = 0; b < n; ++b) {
            const double rms = t.count[b] ? std::sqrt(t.energy[b] / t.count[b]) : 0.0;
            row[b * 2] = static_cast<uint16_t>(std::lround(std::clamp(t.peak[b], 0.0f, 1.0f) * 65535.0f));
            row[b * 2 + 1] = static_cast<uint16_t>(std::lround(std::clamp(rms, 0.0, 1.0) * 65535.0));
        }
        ok = ok && write_le(f, row.data(), row.size() * sizeof(uint16_t));
    }
    ok = (fclose(f) == 0) && ok;
    if (!ok || !MoveFileExW(temp.c_str(), out_file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        set_last_error("cannot write waveform cache");
        return SYNA_E_IO;
    }
    log::info("waveform", "extracted tracks={} buckets={} file=\"{}\"", tracks.size(), buckets, to_utf8(path));
    return SYNA_OK;
}

}  // namespace syna
