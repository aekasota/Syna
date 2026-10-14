#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SYNA_BUILDING_DLL
#define SYNA_API __declspec(dllexport)
#else
#define SYNA_API __declspec(dllimport)
#endif

#define SYNA_API_VERSION 3
#define SYNA_MAX_AUDIO_TRACKS 8

typedef int32_t syna_result;

enum {
    SYNA_OK = 0,
    SYNA_E_INVALID_ARG = -1,
    SYNA_E_FAIL = -2,
    SYNA_E_OPEN = -3,
    SYNA_E_UNSUPPORTED = -4,
    SYNA_E_NO_VIDEO = -5,
    SYNA_E_DEVICE = -6,
    SYNA_E_CANCELLED = -7,
    SYNA_E_IO = -8,
    SYNA_E_DISK_FULL = -9,
    SYNA_E_ACCESS_DENIED = -10,
    SYNA_E_NOT_FOUND = -11,
    SYNA_E_BUSY = -12,
    SYNA_E_VALIDATION = -13,
    SYNA_E_BUFFER_TOO_SMALL = -14,
};

typedef struct syna_player syna_player;
typedef struct syna_job syna_job;

/* Engine */

SYNA_API uint32_t syna_api_version(void);

/* Initializes logging and FFmpeg. log_dir may be NULL (logging to file disabled). Idempotent. */
SYNA_API syna_result syna_initialize(const wchar_t* log_dir, int32_t verbose);
SYNA_API void syna_shutdown(void);

/* Thread-local description of the last error returned on the calling thread (UTF-8). */
SYNA_API const char* syna_last_error(void);

/* JSON: adapters, hardware decode/encode capabilities. Caller frees with syna_free_string. */
SYNA_API syna_result syna_query_system_json(char** out_json);
SYNA_API void syna_free_string(char* s);

/* Media inspection (blocking: call from a worker thread) */

enum { SYNA_PROBE_FAST = 0, SYNA_PROBE_FULL = 1 };

/* JSON media description. FAST reads only container headers; FULL also decodes a few frames
   to fill pixel format / color properties and lists keyframe timestamps. */
SYNA_API syna_result syna_probe_json(const wchar_t* path, int32_t mode, char** out_json);

/* Decodes one frame near at_us (keyframe-aligned) and writes a JPEG scaled to max_width. */
SYNA_API syna_result syna_make_thumbnail(const wchar_t* path, const wchar_t* out_jpeg, int32_t max_width, int64_t at_us);

/* Playback */

enum {
    SYNA_STATE_IDLE = 0,
    SYNA_STATE_OPENING = 1,
    SYNA_STATE_PAUSED = 2,
    SYNA_STATE_PLAYING = 3,
    SYNA_STATE_ENDED = 4,
    SYNA_STATE_ERROR = 5,
};

enum {
    SYNA_DECODER_NONE = 0,
    SYNA_DECODER_SOFTWARE = 1,
    SYNA_DECODER_D3D11VA = 2,
};

enum {
    SYNA_SEEK_EXACT = 0,  /* present the frame displayed at the target time */
    SYNA_SEEK_FAST = 1,   /* present the keyframe at or before the target */
};

enum {
    SYNA_EVENT_OPENED = 1,          /* value = duration_us */
    SYNA_EVENT_OPEN_FAILED = 2,     /* code = syna_result */
    SYNA_EVENT_SEEK_DONE = 3,       /* value = presented frame pts (us) */
    SYNA_EVENT_ENDED = 4,
    SYNA_EVENT_ERROR = 5,           /* code = syna_result */
    SYNA_EVENT_AUDIO_DEVICE = 6,    /* code = 1 device active, 0 no device */
    SYNA_EVENT_DECODER_FALLBACK = 7,/* hardware decoder unavailable, software in use */
    SYNA_EVENT_DEVICE_LOST = 8,     /* D3D device removed: recreate the player */
    SYNA_EVENT_SOURCE_ERROR = 9,    /* read error, e.g. the file disappeared */
    SYNA_EVENT_LOOPED = 10,
};

typedef struct syna_event {
    int32_t type;
    int32_t code;
    int64_t open_id;
    int64_t value;
} syna_event;

typedef struct syna_open_options {
    int64_t start_us;          /* initial position */
    int32_t autoplay;          /* start playing once the first frame is shown */
    int32_t hw_decode;         /* 0 = force software, 1 = prefer hardware */
} syna_open_options;

typedef struct syna_player_state {
    int32_t state;
    int32_t decoder;               /* SYNA_DECODER_* actually in use */
    int64_t open_id;
    int64_t position_us;           /* presented frame time while paused, clock time while playing */
    int64_t duration_us;
    int64_t frame_pts_us;          /* pts of the frame on screen, INT64_MIN if none */
    int64_t frame_duration_us;
    double rate;
    int32_t seeking;
    int32_t audio_tracks;
    int32_t audio_device_ok;
    int32_t loop_enabled;
    float track_peak[SYNA_MAX_AUDIO_TRACKS]; /* post-gain linear peak since the previous poll */
    float master_peak[2];
    float limiter_gain_reduction_db;
    /* diagnostics */
    int64_t frames_presented;
    int64_t frames_dropped;
    int64_t audio_underruns;
    double presented_fps;
    double av_drift_ms;            /* presented video pts minus audio clock at present time */
    double audio_buffer_ms;        /* device buffer length */
    double audio_latency_ms;       /* queued in device */
    double render_ms;              /* GPU submit cost of the last frame (CPU side) */
    int32_t video_queue;
    int32_t video_queue_max;
} syna_player_state;

/* Creates the D3D11 device, swap chain, render thread and audio output. Call on the UI thread. */
SYNA_API syna_result syna_player_create(syna_player** out);
SYNA_API void syna_player_destroy(syna_player* p);

/* panel = IUnknown* of a Microsoft.UI.Xaml.Controls.SwapChainPanel. UI thread only. */
SYNA_API syna_result syna_player_attach_panel(syna_player* p, void* panel);
SYNA_API syna_result syna_player_resize(syna_player* p, int32_t width_px, int32_t height_px, float scale_x, float scale_y);
SYNA_API syna_result syna_player_set_background(syna_player* p, float r, float g, float b);

/* Asynchronous. Returns an open id echoed in events and state; completion is SYNA_EVENT_OPENED/OPEN_FAILED. */
SYNA_API int64_t syna_player_open(syna_player* p, const wchar_t* path, const syna_open_options* options);
SYNA_API syna_result syna_player_close(syna_player* p);
/* JSON media info of the open file (same schema as syna_probe_json FULL). */
SYNA_API syna_result syna_player_media_json(syna_player* p, char** out_json);

SYNA_API syna_result syna_player_play(syna_player* p);
SYNA_API syna_result syna_player_pause(syna_player* p);
SYNA_API syna_result syna_player_seek(syna_player* p, int64_t position_us, int32_t mode);
SYNA_API syna_result syna_player_step(syna_player* p, int32_t frames); /* +1 / -1 while paused */
SYNA_API syna_result syna_player_set_rate(syna_player* p, double rate);
SYNA_API syna_result syna_player_set_loop(syna_player* p, int32_t enabled, int64_t a_us, int64_t b_us);

SYNA_API syna_result syna_player_set_track_gain(syna_player* p, int32_t track, float gain);
SYNA_API syna_result syna_player_set_track_mute(syna_player* p, int32_t track, int32_t muted);
SYNA_API syna_result syna_player_set_track_solo(syna_player* p, int32_t track, int32_t solo);
SYNA_API syna_result syna_player_set_track_offset(syna_player* p, int32_t track, int64_t offset_us);
SYNA_API syna_result syna_player_set_master_gain(syna_player* p, float gain);
SYNA_API syna_result syna_player_set_master_mute(syna_player* p, int32_t muted);
SYNA_API syna_result syna_player_set_limiter(syna_player* p, int32_t enabled);
/* Positive delays all audio relative to video. */
SYNA_API syna_result syna_player_set_av_offset(syna_player* p, int64_t offset_us);

SYNA_API syna_result syna_player_get_state(syna_player* p, syna_player_state* out);
/* JSON: decoder path actually in use, GPU, audio output format. Low frequency (diagnostics panel). */
SYNA_API syna_result syna_player_diagnostics_json(syna_player* p, char** out_json);
SYNA_API int32_t syna_player_poll_events(syna_player* p, syna_event* events, int32_t capacity);

/* Renders the frame on screen at source resolution. format: 0 = PNG, 1 = JPEG. */
SYNA_API syna_result syna_player_save_frame(syna_player* p, const wchar_t* out_path, int32_t format);

/* Background jobs (waveform, trim) */

enum {
    SYNA_JOB_RUNNING = 0,
    SYNA_JOB_SUCCEEDED = 1,
    SYNA_JOB_FAILED = 2,
    SYNA_JOB_CANCELLED = 3,
};

typedef struct syna_job_status {
    int32_t state;
    syna_result error;
    double progress;     /* 0..1 */
} syna_job_status;

/* Peak/RMS envelopes for every audio track, written to out_file (format: docs/MEDIA_ENGINE.md). */
SYNA_API syna_result syna_waveform_start(const wchar_t* path, const wchar_t* out_file, int32_t buckets_per_second, syna_job** out);

enum {
    SYNA_TRIM_QUICK = 0,
    SYNA_TRIM_SMART = 1,
    SYNA_TRIM_EXACT = 2,
};

/* What happens to the video. */
enum {
    SYNA_TRIM_VIDEO_ORIGINAL = 0,  /* copied as far as the mode allows; re-encoded parts keep the source's codec, size and timing */
    SYNA_TRIM_VIDEO_ENCODE = 1,    /* the whole selection re-encoded with the video_* settings (every mode cuts frame-accurately) */
    SYNA_TRIM_VIDEO_LOSSLESS = 2,  /* the whole selection re-encoded losslessly, at the source's size and timing */
};

enum {
    SYNA_VIDEO_CODEC_SOURCE = 0,   /* the source's codec, or the most compatible one the container takes */
    SYNA_VIDEO_CODEC_H264 = 1,
    SYNA_VIDEO_CODEC_HEVC = 2,
    SYNA_VIDEO_CODEC_AV1 = 3,
    SYNA_VIDEO_CODEC_VP9 = 4,
};

enum { SYNA_ENCODER_SPEED_FASTER = 0, SYNA_ENCODER_SPEED_BALANCED = 1, SYNA_ENCODER_SPEED_SMALLER = 2 };

/* How the audio tracks are arranged in the output. */
enum {
    SYNA_TRIM_AUDIO_MIX = 0,       /* every track mixed into one track (a single track is kept as it is) */
    SYNA_TRIM_AUDIO_SEPARATE = 1,  /* every track kept as its own track */
};

enum {
    SYNA_AUDIO_CODEC_AUTO = 0,     /* tracks are copied when nothing requires encoding; encoded tracks use the container's usual codec */
    SYNA_AUDIO_CODEC_AAC = 1,
    SYNA_AUDIO_CODEC_OPUS = 2,
    SYNA_AUDIO_CODEC_FLAC = 3,     /* lossless */
};

/* One source audio track as the player's mixer plays it. A muted track, or one silenced because
   another is soloed, has gain 0. Tracks that are not listed are used as recorded. */
typedef struct syna_trim_track {
    int32_t stream_index;  /* the source stream (probe JSON audio[].streamIndex) */
    float gain;            /* linear, 0..4; 1 = as recorded */
    int64_t offset_us;     /* positive = the track plays later (syna_player_set_track_offset) */
} syna_trim_track;

typedef struct syna_trim_request {
    const wchar_t* source;
    const wchar_t* destination;   /* final path; written via a temp file in the same folder */
    int64_t in_us;
    int64_t out_us;
    int32_t mode;
    int32_t allow_hw_encode;
    /* Video. Zero values keep the source's properties. */
    int32_t video;                  /* SYNA_TRIM_VIDEO_* */
    int32_t video_codec;            /* SYNA_VIDEO_CODEC_* (ENCODE, LOSSLESS) */
    int32_t video_height;           /* scaled down to this height, aspect kept (ENCODE) */
    int32_t video_fps_num;          /* constant output frame rate (ENCODE); 0 keeps the source's frame timing */
    int32_t video_fps_den;
    int32_t video_quality;          /* constant quality on the H.264 CQ scale, 1..51, lower is better; 0 = default */
    int32_t video_bitrate_kbps;     /* average bitrate instead of constant quality (ENCODE) */
    int32_t video_max_bitrate_kbps; /* peak bitrate cap (ENCODE) */
    int32_t video_speed;            /* SYNA_ENCODER_SPEED_* */
    /* Audio. */
    int32_t audio;                  /* SYNA_TRIM_AUDIO_* */
    int32_t audio_codec;            /* SYNA_AUDIO_CODEC_* */
    int32_t audio_bitrate_kbps;     /* 0 = default for the codec (ignored for FLAC) */
    int32_t audio_sample_rate;      /* 0 = the source's */
    int32_t audio_channels;         /* 0 = the source's, 1 = mono, 2 = stereo */
    /* Mixer. A track at gain 1 with no offset is used as recorded (copied where the rest allows);
       any other one is decoded, adjusted and encoded with the audio_* settings. Copied by
       syna_trim_start. */
    const syna_trim_track* tracks;
    int32_t track_count;
} syna_trim_request;

SYNA_API syna_result syna_trim_start(const syna_trim_request* request, syna_job** out);

SYNA_API syna_result syna_job_status_get(syna_job* job, syna_job_status* out);
/* JSON report (trim: what was done, encoder, validation). Available once the job finished. */
SYNA_API syna_result syna_job_result_json(syna_job* job, char** out_json);
SYNA_API void syna_job_cancel(syna_job* job);
/* Cancels if still running and blocks until the worker exits. */
SYNA_API void syna_job_destroy(syna_job* job);

/* Diagnostics */

typedef struct syna_process_stats {
    double cpu_percent;          /* of all logical processors, since the previous call */
    double gpu_decode_percent;   /* GPU video-decode engine utilization by this process */
    double gpu_3d_percent;
    double gpu_encode_percent;
    int64_t private_bytes;
    int64_t working_set_bytes;
    int64_t gpu_dedicated_bytes; /* this process's local video memory usage */
} syna_process_stats;

SYNA_API syna_result syna_sample_process_stats(syna_process_stats* out);

#ifdef __cplusplus
}
#endif
