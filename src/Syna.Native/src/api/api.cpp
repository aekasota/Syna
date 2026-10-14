// C ABI boundary. No C++ exception or FFmpeg/D3D type crosses this file.
#include "syna_api.h"

#include "common/ffmpeg.h"
#include "common/log.h"
#include "common/util.h"
#include "diagnostics/diagnostics.h"
#include "jobs/job.h"
#include "media/media_info.h"
#include "media/player.h"
#include "media/thumbnail.h"
#include "media/waveform.h"
#include "trim/trim.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <mutex>

using namespace syna;

namespace {

std::once_flag g_init_once;

template <class F>
syna_result guarded(F&& f) {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        set_last_error("out of memory");
        return SYNA_E_FAIL;
    } catch (const std::exception& e) {
        set_last_error(e.what());
        log::error("api", "exception: {}", e.what());
        return SYNA_E_FAIL;
    } catch (...) {
        set_last_error("unknown exception");
        return SYNA_E_FAIL;
    }
}

Player* as_player(syna_player* p) { return reinterpret_cast<Player*>(p); }

}  // namespace

extern "C" {

SYNA_API uint32_t syna_api_version(void) { return SYNA_API_VERSION; }

SYNA_API syna_result syna_initialize(const wchar_t* log_dir, int32_t verbose) {
    return guarded([&]() -> syna_result {
        std::call_once(g_init_once, [&] {
            log::init(log_dir ? log_dir : L"", verbose != 0);
            av_log_set_level(verbose ? AV_LOG_VERBOSE : AV_LOG_WARNING);
            log::info("engine", "initialized api={} avcodec={} avformat={} license=\"{}\"", SYNA_API_VERSION,
                      AV_STRINGIFY(LIBAVCODEC_VERSION), AV_STRINGIFY(LIBAVFORMAT_VERSION), avcodec_license());
        });
        return SYNA_OK;
    });
}

SYNA_API void syna_shutdown(void) { log::shutdown(); }

SYNA_API const char* syna_last_error(void) { return last_error(); }

SYNA_API void syna_free_string(char* s) { std::free(s); }

SYNA_API syna_result syna_probe_json(const wchar_t* path, int32_t mode, char** out_json) {
    if (!path || !out_json) return SYNA_E_INVALID_ARG;
    *out_json = nullptr;
    return guarded([&]() -> syna_result {
        MediaInfo info;
        syna_result r = probe_media(path, mode == SYNA_PROBE_FULL, info);
        if (r != SYNA_OK) return r;
        *out_json = dup_for_abi(media_info_json(info));
        return *out_json ? SYNA_OK : SYNA_E_FAIL;
    });
}

SYNA_API syna_result syna_player_create(syna_player** out) {
    if (!out) return SYNA_E_INVALID_ARG;
    *out = nullptr;
    return guarded([&]() -> syna_result {
        auto player = std::make_unique<Player>();
        std::string error;
        if (!player->init(&error)) {
            set_last_error(error);
            log::error("api", "player init failed: {}", error);
            return SYNA_E_DEVICE;
        }
        *out = reinterpret_cast<syna_player*>(player.release());
        return SYNA_OK;
    });
}

SYNA_API void syna_player_destroy(syna_player* p) {
    guarded([&]() -> syna_result {
        delete as_player(p);
        return SYNA_OK;
    });
}

SYNA_API syna_result syna_player_attach_panel(syna_player* p, void* panel) {
    if (!p || !panel) return SYNA_E_INVALID_ARG;
    return guarded([&]() -> syna_result {
        HRESULT hr = as_player(p)->attach_panel(static_cast<IUnknown*>(panel));
        if (FAILED(hr)) {
            set_last_error(std::format("SetSwapChain failed (0x{:08X})", static_cast<uint32_t>(hr)));
            return SYNA_E_DEVICE;
        }
        return SYNA_OK;
    });
}

SYNA_API syna_result syna_player_resize(syna_player* p, int32_t width_px, int32_t height_px, float scale_x, float scale_y) {
    if (!p) return SYNA_E_INVALID_ARG;
    return guarded([&]() -> syna_result {
        as_player(p)->resize(width_px, height_px, scale_x, scale_y);
        return SYNA_OK;
    });
}

SYNA_API syna_result syna_player_set_background(syna_player* p, float r, float g, float b) {
    if (!p) return SYNA_E_INVALID_ARG;
    return guarded([&]() -> syna_result {
        as_player(p)->set_background(r, g, b);
        return SYNA_OK;
    });
}

SYNA_API int64_t syna_player_open(syna_player* p, const wchar_t* path, const syna_open_options* options) {
    if (!p || !path) return 0;
    int64_t id = 0;
    guarded([&]() -> syna_result {
        syna_open_options o{};
        o.hw_decode = 1;
        if (options) o = *options;
        id = as_player(p)->open(path, o);
        return SYNA_OK;
    });
    return id;
}

SYNA_API syna_result syna_player_close(syna_player* p) {
    if (!p) return SYNA_E_INVALID_ARG;
    return guarded([&]() -> syna_result { as_player(p)->close(); return SYNA_OK; });
}

SYNA_API syna_result syna_player_media_json(syna_player* p, char** out_json) {
    if (!p || !out_json) return SYNA_E_INVALID_ARG;
    *out_json = nullptr;
    return guarded([&]() -> syna_result {
        std::string json;
        syna_result r = as_player(p)->media_json(json);
        if (r != SYNA_OK) return r;
        *out_json = dup_for_abi(json);
        return SYNA_OK;
    });
}

SYNA_API syna_result syna_player_play(syna_player* p) {
    if (!p) return SYNA_E_INVALID_ARG;
    return guarded([&]() -> syna_result { as_player(p)->play(); return SYNA_OK; });
}

SYNA_API syna_result syna_player_pause(syna_player* p) {
    if (!p) return SYNA_E_INVALID_ARG;
    return guarded([&]() -> syna_result { as_player(p)->pause(); return SYNA_OK; });
}

SYNA_API syna_result syna_player_seek(syna_player* p, int64_t position_us, int32_t mode) {
    if (!p) return SYNA_E_INVALID_ARG;
    return guarded([&]() -> syna_result { as_player(p)->seek(position_us, mode); return SYNA_OK; });
}

SYNA_API syna_result syna_player_step(syna_player* p, int32_t frames) {
    if (!p || frames == 0) return SYNA_E_INVALID_ARG;
    return guarded([&]() -> syna_result { as_player(p)->step(frames > 0 ? frames : -1); return SYNA_OK; });
}

SYNA_API syna_result syna_player_set_rate(syna_player* p, double rate) {
    if (!p || !(rate > 0)) return SYNA_E_INVALID_ARG;
    return guarded([&]() -> syna_result { as_player(p)->set_rate(rate); return SYNA_OK; });
}

SYNA_API syna_result syna_player_set_loop(syna_player* p, int32_t enabled, int64_t a_us, int64_t b_us) {
    if (!p) return SYNA_E_INVALID_ARG;
    return guarded([&]() -> syna_result { as_player(p)->set_loop(enabled != 0, a_us, b_us); return SYNA_OK; });
}

SYNA_API syna_result syna_player_set_track_gain(syna_player* p, int32_t track, float gain) {
    if (!p || track < 0 || track >= SYNA_MAX_AUDIO_TRACKS || !(gain >= 0)) return SYNA_E_INVALID_ARG;
    as_player(p)->settings().tracks[track].gain.store(std::min(gain, 4.0f));
    return SYNA_OK;
}

SYNA_API syna_result syna_player_set_track_mute(syna_player* p, int32_t track, int32_t muted) {
    if (!p || track < 0 || track >= SYNA_MAX_AUDIO_TRACKS) return SYNA_E_INVALID_ARG;
    as_player(p)->settings().tracks[track].mute.store(muted != 0);
    return SYNA_OK;
}

SYNA_API syna_result syna_player_set_track_solo(syna_player* p, int32_t track, int32_t solo) {
    if (!p || track < 0 || track >= SYNA_MAX_AUDIO_TRACKS) return SYNA_E_INVALID_ARG;
    as_player(p)->settings().tracks[track].solo.store(solo != 0);
    return SYNA_OK;
}

SYNA_API syna_result syna_player_set_track_offset(syna_player* p, int32_t track, int64_t offset_us) {
    if (!p || track < 0 || track >= SYNA_MAX_AUDIO_TRACKS) return SYNA_E_INVALID_ARG;
    as_player(p)->settings().tracks[track].offset_us.store(std::clamp<int64_t>(offset_us, -5000000, 5000000));
    return SYNA_OK;
}

SYNA_API syna_result syna_player_set_master_gain(syna_player* p, float gain) {
    if (!p || !(gain >= 0)) return SYNA_E_INVALID_ARG;
    as_player(p)->settings().master_gain.store(std::min(gain, 4.0f));
    return SYNA_OK;
}

SYNA_API syna_result syna_player_set_master_mute(syna_player* p, int32_t muted) {
    if (!p) return SYNA_E_INVALID_ARG;
    as_player(p)->settings().master_mute.store(muted != 0);
    return SYNA_OK;
}

SYNA_API syna_result syna_player_set_limiter(syna_player* p, int32_t enabled) {
    if (!p) return SYNA_E_INVALID_ARG;
    as_player(p)->settings().limiter.store(enabled != 0);
    return SYNA_OK;
}

SYNA_API syna_result syna_player_set_av_offset(syna_player* p, int64_t offset_us) {
    if (!p) return SYNA_E_INVALID_ARG;
    as_player(p)->settings().av_offset_us.store(std::clamp<int64_t>(offset_us, -2000000, 2000000));
    return SYNA_OK;
}

SYNA_API syna_result syna_player_get_state(syna_player* p, syna_player_state* out) {
    if (!p || !out) return SYNA_E_INVALID_ARG;
    return guarded([&]() -> syna_result {
        as_player(p)->get_state(*out);
        return SYNA_OK;
    });
}

SYNA_API syna_result syna_player_diagnostics_json(syna_player* p, char** out_json) {
    if (!p || !out_json) return SYNA_E_INVALID_ARG;
    *out_json = nullptr;
    return guarded([&]() -> syna_result {
        *out_json = dup_for_abi(as_player(p)->diagnostics_json());
        return *out_json ? SYNA_OK : SYNA_E_FAIL;
    });
}

SYNA_API int32_t syna_player_poll_events(syna_player* p, syna_event* events, int32_t capacity) {
    if (!p || !events || capacity <= 0) return 0;
    return as_player(p)->poll_events(events, capacity);
}

SYNA_API syna_result syna_player_save_frame(syna_player* p, const wchar_t* out_path, int32_t format) {
    if (!p || !out_path) return SYNA_E_INVALID_ARG;
    return guarded([&]() -> syna_result { return as_player(p)->save_frame(out_path, format); });
}

SYNA_API syna_result syna_query_system_json(char** out_json) {
    if (!out_json) return SYNA_E_INVALID_ARG;
    *out_json = nullptr;
    return guarded([&]() -> syna_result {
        *out_json = dup_for_abi(system_info_json());
        return *out_json ? SYNA_OK : SYNA_E_FAIL;
    });
}

SYNA_API syna_result syna_make_thumbnail(const wchar_t* path, const wchar_t* out_jpeg, int32_t max_width, int64_t at_us) {
    if (!path || !out_jpeg || max_width <= 0) return SYNA_E_INVALID_ARG;
    return guarded([&]() -> syna_result { return make_thumbnail(path, out_jpeg, max_width, at_us); });
}

SYNA_API syna_result syna_waveform_start(const wchar_t* path, const wchar_t* out_file, int32_t buckets_per_second, syna_job** out) {
    if (!path || !out_file || !out) return SYNA_E_INVALID_ARG;
    *out = nullptr;
    return guarded([&]() -> syna_result {
        std::wstring src = path, dst = out_file;
        auto job = std::make_unique<Job>([src, dst, buckets_per_second](Job& j) { return extract_waveform(src, dst, buckets_per_second, j); },
                                         /*background_priority*/ true);
        *out = reinterpret_cast<syna_job*>(job.release());
        return SYNA_OK;
    });
}

SYNA_API syna_result syna_trim_start(const syna_trim_request* request, syna_job** out) {
    if (!request || !request->source || !request->destination || !out) return SYNA_E_INVALID_ARG;
    *out = nullptr;
    return guarded([&]() -> syna_result {
        TrimParams params;
        params.source = request->source;
        params.destination = request->destination;
        params.in_us = request->in_us;
        params.out_us = request->out_us;
        params.mode = request->mode;
        params.allow_hw_encode = request->allow_hw_encode != 0;
        params.video = std::clamp(request->video, 0, static_cast<int>(SYNA_TRIM_VIDEO_LOSSLESS));
        params.video_codec = std::clamp(request->video_codec, 0, static_cast<int>(SYNA_VIDEO_CODEC_VP9));
        params.video_height = std::max(0, request->video_height);
        if (request->video_fps_num > 0 && request->video_fps_den > 0) params.video_fps = {request->video_fps_num, request->video_fps_den};
        params.video_quality = std::clamp(request->video_quality, 0, 51);
        params.video_bitrate = std::max(0, request->video_bitrate_kbps) * int64_t{1000};
        params.video_max_bitrate = std::max(0, request->video_max_bitrate_kbps) * int64_t{1000};
        params.video_speed = std::clamp(request->video_speed, 0, static_cast<int>(SYNA_ENCODER_SPEED_SMALLER));
        params.audio = request->audio == SYNA_TRIM_AUDIO_SEPARATE ? SYNA_TRIM_AUDIO_SEPARATE : SYNA_TRIM_AUDIO_MIX;
        params.audio_codec = std::clamp(request->audio_codec, 0, static_cast<int>(SYNA_AUDIO_CODEC_FLAC));
        params.audio_bitrate = std::max(0, request->audio_bitrate_kbps) * int64_t{1000};
        params.audio_sample_rate = std::max(0, request->audio_sample_rate);
        params.audio_channels = std::clamp(request->audio_channels, 0, 2);
        if (request->track_count < 0 || (request->track_count > 0 && !request->tracks)) return SYNA_E_INVALID_ARG;
        for (int32_t i = 0; i < request->track_count; ++i) {
            const syna_trim_track& t = request->tracks[i];
            if (t.stream_index < 0 || !std::isfinite(t.gain)) return SYNA_E_INVALID_ARG;
            params.tracks.push_back({t.stream_index, std::clamp(t.gain, 0.0f, 4.0f), std::clamp<int64_t>(t.offset_us, -60000000, 60000000)});
        }
        auto job = std::make_unique<Job>([params](Job& j) { return run_trim(params, j); }, /*background_priority*/ false);
        *out = reinterpret_cast<syna_job*>(job.release());
        return SYNA_OK;
    });
}

SYNA_API syna_result syna_job_status_get(syna_job* job, syna_job_status* out) {
    if (!job || !out) return SYNA_E_INVALID_ARG;
    *out = reinterpret_cast<Job*>(job)->status();
    return SYNA_OK;
}

SYNA_API syna_result syna_job_result_json(syna_job* job, char** out_json) {
    if (!job || !out_json) return SYNA_E_INVALID_ARG;
    *out_json = nullptr;
    return guarded([&]() -> syna_result {
        Job* j = reinterpret_cast<Job*>(job);
        if (j->status().state == SYNA_JOB_RUNNING) return SYNA_E_BUSY;
        *out_json = dup_for_abi(j->result_json());
        return SYNA_OK;
    });
}

SYNA_API void syna_job_cancel(syna_job* job) {
    if (job) reinterpret_cast<Job*>(job)->cancel();
}

SYNA_API void syna_job_destroy(syna_job* job) {
    guarded([&]() -> syna_result {
        delete reinterpret_cast<Job*>(job);
        return SYNA_OK;
    });
}

SYNA_API syna_result syna_sample_process_stats(syna_process_stats* out) {
    if (!out) return SYNA_E_INVALID_ARG;
    return guarded([&]() -> syna_result { return sample_process_stats(*out); });
}

}  // extern "C"
