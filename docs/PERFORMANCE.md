# Performance

Every number here was measured on the machine below with the Release build; nothing is estimated.
Engine numbers come from `scripts\benchmark.ps1` (the `syna_bench` tool drives the public C ABI the
way the app does); app numbers come from the running app, its log and process counters.

| | |
|---|---|
| CPU | AMD Ryzen 5 5600 (6 cores / 12 threads), 32 GB RAM |
| GPU | NVIDIA GeForce RTX 4060 Ti, driver 581.80 (32.0.15.8180) |
| OS | Windows 11 Pro build 26200, 96 DPI |
| Date | 2026-10-02 |

Real clips are two of the user's ShadowPlay recordings (H.264 High@4.2 1080p60 with B-frames, IDR
every 4.17 s, about 6 Mb/s). Fixtures are described in [TEST_MATRIX.md](TEST_MATRIX.md).

## The app

Launched with a real clip (`00-26-13`, 1080p60), session restore on:

| Measurement | Result |
|---|---|
| Process start → first video frame on screen | 1.47 s |
| Open → first frame (first clip) | 211 ms |
| Open → first frame (second clip, sent from another launch through single-instance redirection) | 139 ms |
| CPU while playing 1080p60 | 1.8 % of all cores (21 % of one core), steady state over 12 s |
| CPU while paused | 0.04 % |
| GPU video decode / 3D while playing | 6.1 % / 0.9 % |
| Memory while playing | 268 MB working set, 220 MB private, 110 threads |

Startup breakdown from the log: about 0.5 s until `App.OnLaunched` (.NET and Windows App SDK
start-up), 0.75 s for the window, XAML and the player (creating the D3D11 device, swap chain and
WASAPI stream takes about 200 ms), then 211 ms to decode and present the first frame. The video is
presented by the engine's render thread; the UI thread polls state once per frame.

## Engine

`scripts\benchmark.ps1 -Clips <two real clips>`, 8 s of real-time playback per file, audio muted at
the master (mixer and clock running):

| File | Decoder | Open ms | Exact seek p50/p95 ms | Step back p95 ms | FPS | Dropped | Drift max ms | CPU % | GPU decode % |
|---|---|---|---|---|---|---|---|---|---|
| A H.264 1080p60, 2 tracks | d3d11va | 90 | 31/47 | 32 | 60.0 | 0 | 3.2 | 0.53 | 7.0 |
| B HEVC 1080p60 | d3d11va | 57 | 16/16 | 31 | 60.0 | 0 | 3.2 | 0.21 | 3.6 |
| C H.264 720p30 | d3d11va | 68 | 16/47 | 31 | 30.0 | 0 | 3.2 | 0.17 | 1.9 |
| H H.264 B-frames, 5 s GOP | d3d11va | 63 | 171/327 | 379 | 60.0 | 0 | 3.2 | 0.52 | 7.2 |
| I H.264 VFR | d3d11va | 218 | 16/32 | 48 | 59.7 | 0 | 5.4 | 0.71 | 3.8 |
| J HEVC Main 10 | d3d11va | 70 | 16/47 | 31 | 60.0 | 0 | 3.2 | 0.37 | 3.6 |
| L AV1 1080p60 | d3d11va | 66 | 16/32 | 32 | 60.0 | 0 | 3.2 | 0.63 | 4.5 |
| M H.264 in Matroska | d3d11va | 81 | 16/31 | 31 | 60.0 | 0 | 3.2 | 0.82 | 3.7 |
| N VP9 + Opus WebM | d3d11va | 64 | 16/31 | 17 | 30.0 | 0 | 3.2 | 0.07 | 1.2 |
| Real clip `00-26-13` | d3d11va | 99 | 108/263 | 140 | 60.0 | 0 | 3.2 | 0.71 | 6.2 |
| Real clip `00-12-02` | d3d11va | 99 | 140/280 | 185 | 60.0 | 0 * | 3.2 * | 0.55 | 6.2 |

\* In the full run this clip recorded 8 dropped frames and one 328 ms drift spike while other
applications were busy with audio (Discord, Spotify, the Windows audio engine). Three immediate
reruns of the same file gave 0 drops and 3.1–3.2 ms maximum drift; those are the values shown. The
player recovered on its own (it drops late frames to rejoin the audio clock). The same full run also
recorded a one-off 1 s player creation on fixture I (normally about 200 ms), hence its 218 ms open.

Reading the table:

- Seeks are presented on the next display refresh, so latencies cluster at multiples of 16.7 ms.
  Exact seeks into long GOPs (H, the real clips) must decode from the previous keyframe; their cost
  grows with the distance to it (up to 4 s of 1080p60 here).
- A/V drift is the difference between the presented frame's time and the audio clock at
  presentation; about 3 ms is the presentation lead plus the audio buffer granularity.
- CPU is the whole process over the 8 s window, as a share of all 12 logical processors.

### Memory and GPU memory

| | Before fix | After |
|---|---|---|
| Decoder surfaces (H.264/HEVC) | 37 | 27 |
| Private memory after playback, fixture A | 227–295 MB, +110 MB on the first reopen | 186 MB, flat |
| Dedicated GPU memory, 1080p | 140 MB | 111 MB |

Forty consecutive open/close cycles of the same clip keep private memory within 1 MB (185.6 →
186.4 MB): no growth per open. The steady values in the full benchmark are 137–194 MB per file (10-bit
HEVC is the largest because its surfaces are P010).

### Background work

| Job | Fixture A (10 s 1080p60) | Real clip (36–38 s) |
|---|---|---|
| Probe, headers only | 2 ms | 2 ms |
| Probe, full (pixel format, keyframe list) | 11 ms | 36–61 ms |
| Waveform, both tracks, 200 buckets/s | 27 ms | 38–40 ms |
| Thumbnail | 16 ms | 40–47 ms |
| GPU encoder probe (once per run, in the background after startup) | 0.9–1.3 s | |

## Trimming

Selection from 30 % to 70 % of each file, NVENC available, output validated (all passed):

| File | Quick | Smart | Exact |
|---|---|---|---|
| A H.264 1080p60 (IN on a keyframe) | 246 ms, copy | 266 ms, copy | 1,395 ms, NVENC |
| H B-frames, 5 s GOP (IDR inside) | 149 ms, copy | 1,713 ms, 4 s re-encoded | 2,179 ms, NVENC |
| J HEVC Main 10 | 297 ms, copy | 283 ms, copy | 950 ms, NVENC HEVC |
| N VP9 WebM | 74 ms, copy | Exact (VP9 is not spliced), 1,101 ms | 1,093 ms, libvpx VP9 (CPU) |
| Real clip `00-26-13`, 14.7 s out | 189 ms, copy | **847 ms**, 1.5 s re-encoded | 3,658 ms, NVENC |
| Real clip `00-12-02`, 15.3 s out | 225 ms, copy | **1,101 ms**, 1.05 s re-encoded | 4,190 ms, NVENC |

Before Smart Cut handled B-frame sources, both real clips fell back to Exact Cut (4.2 and 4.7 s).
Before the encoder probe moved to startup, Smart Cut on them took 1.5 s.

C, I and M (H.264) splice after re-encoding 0.2 s and finish faster than Exact Cut. B (HEVC: the
encoder's parameter sets differ from the source's) and L (AV1) fall back to Exact Cut and take 0.1–0.2 s
longer than a direct Exact Cut, the time spent deciding; their reports name the reason.

### Long two-track playback

10.3 minutes of continuous playback of a two-track ShadowPlay recording in the app (looping a 98 s
clip, diagnostics overlay on): 37,107 frames presented, 0 dropped, A/V drift between +2.5 and
+3.1 ms, no audio underruns, no device starvations, no timestamp jumps on either track. Drift
correction resampled track 1 for 6 ms and track 2 for 68 ms in total (about 110 ppm), each time by
0.2 %. CPU 1–5 % of all cores, GPU video decode about 10 %, private memory 240–270 MB throughout.

### Cost of mixing several audio tracks

The table above predates mixing audio tracks. A trim of a two-track file that mixes them (the
default arrangement) decodes both tracks, mixes them and encodes the mix once; keeping the tracks
separate copies them, as in the table above. Mixing, measured with the same benchmark (30–70 % of
each file, NVENC, every output validated):

| File | Quick | Smart | Exact |
|---|---|---|---|
| A H.264 1080p60, 4 s out | 329 ms (was 246), video copied | 329 ms (was 266), video copied | 1,374 ms (was 1,395), NVENC |
| ShadowPlay recording, 2 tracks, 39.8 s out | 1,902 ms, video copied | 2,325 ms, 0.3 s re-encoded | 10,589 ms, NVENC |

FFmpeg's CLI needs 1.0 s to decode, mix and AAC-encode the same 40 s of two-track audio while the
video copy alone reads in 0.46 s, so on long Quick and Smart cuts the audio is now the larger part:
about 1 s per 40 s of output (the AAC encoder runs about 40× real time on one core). Frame count,
cadence (60.00 fps) and resolution were unchanged in every output.

## How to reproduce

```powershell
scripts\build.ps1
scripts\benchmark.ps1 -Clips D:\Videos\clip1.mp4, D:\Videos\clip2.mp4
```

Each run writes one JSON report per file and `summary.md` to `artifacts\benchmarks\<timestamp>\`.
App-level startup times are written to `%LOCALAPPDATA%\Syna\Logs\app-*.log`
(`first frame id=… ms after open, … ms after process start`).
