# Media engine

`Syna.Native.dll` (C++20, MSVC, static CRT) wraps FFmpeg 8.1 (libavformat, libavcodec,
libavutil, libswresample; libswscale and libavfilter are delay-loaded), Direct3D 11 and WASAPI
behind the C ABI in `include/syna_api.h`.

## Threads of one player

| Thread | Work |
|---|---|
| Control | Serializes commands (open, close, seek, step, rate, loop). Opening a file builds a new `Pipeline` off the UI thread. |
| Render | Presents frames against the master clock, handles seek completion, stepping, end of stream, A-B loop, device loss. |
| Demux | Reads packets, routes video to a bounded packet queue and audio to per-track decoders. Performs seeks. |
| Video decode | Decodes into a 6-frame queue (`kFrameQueueCapacity`). |
| Audio decode | Decodes every audio track, converts it to the output format (float32, device rate and layout) and places it on the timeline, up to 400 ms ahead of the mix. |
| Audio output | One WASAPI shared-mode stream, event driven; pulls the mixer. |

All queues are bounded and every blocking wait also waits on an abort event, so closing or
seeking never deadlocks behind a full queue.

## Seeking

Every seek starts a new **generation** (serial number). Packets, frames and audio carry the
serial they were produced for; stale data is dropped at every stage instead of being flushed with
locks. A burst of seeks while scrubbing collapses to the latest one.

- **Fast seek** presents the keyframe at or before the target.
- **Exact seek** presents the frame whose display interval contains the target. The demuxer seeks
  on the DTS index to a keyframe a little before the target (a margin covers frame reordering),
  frames before the target are decoded and discarded, and if the first decoded frame turns out to
  be displayed after the target (B-frames, odd indexes) the seek retries from earlier with a growing
  margin, up to 64 s. Timestamps are converted with explicit rounding toward the past, so a target
  that falls exactly on a frame boundary lands on that frame.
- **Frame stepping** forward presents the next frame; backward re-seeks exactly to the frame that
  was displayed before the current one, which works across keyframes and with VFR timestamps.

`SYNA_EVENT_SEEK_DONE` carries the presented frame's timestamp, so the UI always shows the frame
actually on screen.

## Clock and synchronization

Audio is the master clock. The output maps device position (`IAudioClock`) to file time through
segments (start time, rate), so the clock stays correct across pauses, seeks and speed changes.
With no audio device, or media without audio, a null sink consumes audio at wall-clock pace through
the same code path, so the clock never switches implementation.

The render thread presents the newest frame whose timestamp is due (within an 8 ms lead), counts
any older due frames as dropped, and sleeps precisely until the next frame is due. Measured drift
between presented video and the audio clock is 3 ms on every fixture (see [PERFORMANCE.md](PERFORMANCE.md)).

Speed changes (0.25× to 2×) keep pitch through FFmpeg's `atempo` filter (libavfilter is only
loaded the first time the speed differs from 1×).

## Video

- **Hardware decoding**: D3D11VA on the renderer's own D3D11 device (multithread protected). The
  decoder's surface pool is created with `D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE`, so the
  renderer samples decoded surfaces directly through shader resource views on the texture-array
  slices: no copy and no CPU round trip. If the driver refuses shader-bindable decoder surfaces, the
  engine falls back to a GPU copy per frame; if hardware decoding is unavailable for the stream it
  falls back to software decoding and reports `SYNA_EVENT_DECODER_FALLBACK`. The state struct and the
  diagnostics always name the path actually in use.
- **Pool size**: FFmpeg's count for the codec's reference frames plus the decode queue and renderer
  headroom (27 surfaces for H.264/HEVC). Each surface is a full frame of GPU memory, so this was
  measured rather than guessed (see PERFORMANCE.md).
- **Software decoding**: frame and slice threads; frames are uploaded to dynamic textures.
- **Rendering**: a composition swap chain (flip sequential, frame latency waitable object) attached
  to the WinUI `SwapChainPanel` through `ISwapChainPanelNative`. One pixel shader converts YUV to
  RGB with a matrix computed on the CPU from the stream's color space and range (BT.601/709/2020,
  limited/full), honors chroma siting, scales with Catmull-Rom, tone maps PQ and HLG to SDR, and
  dithers the output. Display aspect follows the sample aspect ratio. Rotation metadata (common in phone recordings, absent from gameplay captures) is shown in media info but not applied yet.
- **Snapshots**: the current frame is rendered at source resolution and encoded with WIC (PNG or
  JPEG) into a temporary file that is moved into place.
- **Device loss**: `DXGI_ERROR_DEVICE_REMOVED/RESET` produces `SYNA_EVENT_DEVICE_LOST`; the app
  recreates the player and reopens the clip at the same position.

## Audio

- One WASAPI shared-mode stream (event driven, float32, `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`), so
  the mixer's format stays stable when the default device changes. `IMMNotificationClient` follows
  default-device changes and removals; if no device exists the null sink keeps time.
- **Mixer**: every track is decoded into its own buffer; one mixer sums them into the one output
  stream, on the one clock. Per-track gain (ramped to avoid clicks), mute, solo and timing offset;
  master gain and mute; an instant-attack peak limiter at −0.3 dBFS (optional; without it samples
  are clipped at full scale). A track with nothing decoded for the current block contributes
  silence and counts an underrun; the other tracks are not held back. Per-track peak meters are
  taken after track gain, before the master. The mixer runs inside the output's buffer callback and
  keeps no queue of its own: the per-track buffers are the only decoded-audio buffering.
- Tracks keep their identity end to end: labels come from container metadata (`name`/`title`), and
  unnamed tracks get suggestions (for example two stereo tracks in a ShadowPlay layout become
  "System sounds" and "Microphone"). Labels the user edits are stored per file.

### Placing decoded audio on the timeline

Recorders stamp each audio packet with their capture clock, not with the running sample count. In
ShadowPlay recordings the stamps of consecutive 1024-sample AAC frames advance by 1002 to 1051
samples, wander up to ±15 ms around the sample count, and single packets land 4 ms early or late,
in a pattern that differs per track.

The mixer used to place every decoded frame at its own timestamp, through a buffer that filled any
gap larger than 2 ms with silence and cut any overlap larger than 2 ms. On real recordings that
inserted a few milliseconds of silence and cut as many samples a moment later, several times a
second, on whichever track had the least regular stamps: the short "pip" heard on one track during
dual-track playback. None of it counted as an underrun, so the diagnostics looked healthy. A
synthetic fixture with ShadowPlay-like stamps (`O_aac_2trk_jitter.mp4`) reproduced it: 90 sample
discontinuities in 15 s of one track.

`TrackConverter` (`audio/track_converter.cpp`) now places audio the way FFmpeg's `aresample`
asynchronous mode does:

- decoded samples are continuous: each frame follows the previous one;
- timestamps anchor a track after opening or seeking, and a jump of 100 ms or more (a real gap or
  overlap in the recording) is honored with silence or a cut and counted as a timestamp jump;
- the distance between where the timestamps place the track and where its samples are is smoothed
  over about a second; past 5 ms the track is resampled by 0.2 % (about 3.5 cents, inaudible) until
  it is back within 1 ms. Real recordings drift about 50 ppm, so this engages for a few seconds every
  few minutes, and keeps the tracks within a few milliseconds of their timestamps and of each other.

Each track's resampler is created once (in resampling mode even at equal rates, so drift correction
never re-initializes it) and reset only on seek, together with the decoder and the track's buffer.
Trim uses the same class for its mixdown (see [TRIM.md](TRIM.md)), so an exported mix lines the
tracks up exactly as playback does.

## Probing

`syna_probe_json` returns one JSON document (schema in `Core/Engine/MediaInfo.cs`): container,
duration, size, bit rate; for video codec, profile, level, size, sample aspect, pixel format, bit
depth, chroma, color range/space/primaries/transfer, HDR type, average and nominal frame rate, VFR
detection with min/max frame interval, frame count, B-frames, rotation, keyframe interval and the
keyframe timestamps (used by the timeline and the trim explanations); for every audio track codec,
rate, channels, layout, bit rate, title, language and the label suggestion. `FAST` reads headers
only; `FULL` also decodes a few frames for exact pixel properties.

## Waveforms

`syna_waveform_start` decodes every audio track (the video stream is discarded at the demuxer, so
its data is not even read) and writes peak/RMS envelopes aligned to the file timeline. The app uses
200 buckets per second and caches one file per clip version.

File layout (little-endian):

```
char[4]  "SYNW"
u32      version (1)
u32      track_count
u32      buckets_per_second
i64      duration_us
per track:
  u32    bucket_count
  bucket_count × { u16 peak, u16 rms }      linear amplitude, 65535 = 0 dBFS
```

`Core/Services/WaveformData.cs` parses it and reduces buckets to screen columns (max of peaks, RMS).

## Thumbnails

`syna_make_thumbnail` decodes one frame near the requested time (default: 10 % into the clip, at
most 3 s) and writes a JPEG of the requested width through WIC.

## Logging and diagnostics

`native-*.log` receives the engine's structured log and FFmpeg's log (throttled, so a corrupt file
cannot flood the disk). `syna_player_diagnostics_json` reports the decoder path (for example
"D3D11VA (h264, zero-copy)"), GPU, renderer, the audio output (format, device buffer, queued audio,
underruns, and device starvations: times the device played everything queued, a gap made by the
output side) and, for every audio track, its decoder state, last timestamp, decoded audio buffered
ahead of the mix, timestamp drift being corrected, timestamp jumps with the silence inserted and
samples cut for them, time added or removed by drift correction, underruns, codec, rate and
channels. The diagnostics overlay (Ctrl+Shift+D) shows them; when a file closes, the log records
the per-track totals:

```
audio track 1 summary: jumps=0 silence=0ms trimmed=0ms stretched=11ms drift=-1.2ms underruns=0
audio output totals since start: underruns=0 device_starvations=0
```

`syna_sample_process_stats` reads CPU time, memory and the GPU engines used by this process (PDH
"GPU Engine" counters: video decode, 3D, video encode).
