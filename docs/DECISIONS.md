# Decisions

Short record of the choices that shape the codebase and why. Newest entries at the bottom of each section.

## Environment (inspected 2026-10-02)

| Item | Found |
|---|---|
| OS | Windows 11 Pro 10.0.26200 |
| Visual Studio | Community 2026 18.10.2, MSVC 14.51 (v145), bundled CMake + Ninja |
| Windows SDK | 10.0.26100 |
| .NET SDK | 10.0.401 (.NET 10 LTS) |
| Windows App SDK | 2.5.1 stable (NuGet + runtime installed) |
| GPU | NVIDIA GeForce RTX 4060 Ti (Ada), driver 581.80 |
| CPU / RAM | Ryzen 5 5600 (6C/12T), 32 GB |
| Local FFmpeg | `C:\ffmpeg` static GPL CLI build (used only to generate test fixtures) |

ShadowPlay output depends on its settings and driver version. Clips on this machine (probed
read-only) come in two shapes, and both drive the defaults below:

- H.264 High@5.1 1920×1080 yuv420p BT.709 limited range, 90 kHz timebase, **no B-frames**,
  **IDR every 0.5 s**, parameter sets only in `avcC`, timestamps jittering between 16.0 and 17.5 ms
  (nominal 60 fps, effectively VFR), two AAC-LC 48 kHz stereo tracks tagged `name=System sounds`
  and `name=Microphone`.
- H.264 High@4.2 1920×1080 60 fps with **B-frames** (I P B B …, reorder delay of one frame),
  **closed GOPs with an IDR every 250 frames** (4.17 s), SPS/PPS repeated in-band before every IDR,
  CBR filler NAL units, one AAC-LC 48 kHz stereo track.

## Stack

- **UI: C# / WinUI 3 / Windows App SDK 2.5.1 on .NET 10.** Latest stable SDK and LTS runtime.
- **Engine: C++20 DLL (`Syna.Native`) with a C ABI of opaque handles.** The UI never sees
  FFmpeg or D3D types. Calls are command/state oriented: one state poll per UI tick, a drained event
  queue for discrete events, JSON for rich, low-frequency data (media info, trim reports).
  No native→managed callbacks, so no callback can outlive its owner.
- **FFmpeg 8.1.3, Syna's own LGPL build** from pinned sources (`scripts/ffmpeg/build.sh`,
  [FFMPEG.md](FFMPEG.md)). Libraries used: avformat, avcodec, avutil, swresample, swscale
  (thumbnails, exotic software formats), avfilter (only `atempo` for speed changes); swscale and
  avfilter are delay-loaded.
  - Development started on BtbN's "LGPL" shared build. The audit before the first release found GPL
    code in it that FFmpeg's configuration does not show (FFTW through Chromaprint, GPL-2.0-only
    files of libzvbi), and the sources and notices of about 80 bundled libraries to ship
    ([LICENSE-AUDIT.md](LICENSE-AUDIT.md)). Syna's build keeps all of FFmpeg's own code and only the
    11 external libraries Syna uses; it is checked for GPL-licensed source and is reproducible.
  - It runs on Linux (a WSL distribution of its own on Windows, a pinned container in CI) with the
    llvm-mingw toolchain, which targets the Universal C Runtime like the BtbN build did. MSVC would
    have needed a POSIX build environment for FFmpeg's configure anyway.
  - 9.0 was tried first. FFmpeg ≥ 9.0 builds against NVENC SDK 13.1 headers (as BtbN did), which
    require driver ≥ 610; this machine's driver 581.80 (API 13.0) made every NVENC encoder fail to
    open. 8.1 with SDK 13.0 headers (drivers since early 2025) keeps NVENC working on current
    drivers. Revisit when 610+ drivers are common.
  - The build ships GNU-style import libraries, which MSVC cannot delay-load; CMake regenerates
    native import libraries from the shipped `.def` files.
- **Static CRT** for the engine DLL, so it does not depend on an installed VC++ redistributable.
- **Build: CMake + Ninja for native, `dotnet build` for managed**, orchestrated by `scripts/*.ps1`.
  No Visual Studio UI steps required.

## Video

- **Decode path: D3D11VA through FFmpeg, sharing the renderer's D3D11 device.** On NVIDIA this runs
  on the NVDEC engine; on AMD/Intel on their decode blocks. Frames stay in GPU texture arrays that are
  created with `D3D11_BIND_SHADER_RESOURCE`, so the renderer samples decoder surfaces directly
  (no GPU→CPU→GPU copy, no intermediate GPU copy). The CUDA/NVDEC path was rejected: it outputs CUDA
  memory that would need CUDA↔D3D11 interop and a copy before presentation, and it is NVIDIA-only.
- **Fallback: software decode** with an explicit reason exposed to diagnostics. The engine reports
  the backend from the format of frames actually produced, never from what was requested.
- **Presentation: composition swap chain hosted in a WinUI `SwapChainPanel`**, flip-sequential,
  frame-latency waitable, presented from a dedicated render thread. YUV→RGB in a pixel shader with
  a CPU-computed matrix (BT.601/709/2020, limited/full range, 8/10-bit), PQ/HLG tone-mapped to SDR,
  dithering for >8-bit sources.

## Audio

- **One output stream.** Each audio track has its own decoder + resampler into a common float format;
  the mixer sums tracks (gain, mute, solo, per-track offset) and runs a peak limiter on the master bus,
  so overlapping tracks never clip. Gains are ramped per buffer to avoid zipper noise.
- **WASAPI shared mode, event driven.** Exclusive mode is not used. Default-device changes are detected
  with `IMMNotificationClient`; the output is rebuilt in place with an output-side converter, so the
  decode pipeline and video keep running.
- **Audio is the master clock** (device position via `IAudioClock`, extrapolated with QPC). Files with no
  audio, or no output device, use a null sink that advances at wall-clock pace through the same code
  path, so there is always exactly one clock.
- **Speed changes use FFmpeg's `atempo` on the master bus** (pitch preserved). 1.0× bypasses it.
- **Decoded audio is continuous; timestamps only anchor it.** Recorders stamp audio with a capture
  clock that wanders around the sample count (ShadowPlay: ±15 ms, single packets 4 ms off).
  Placing each frame at its own timestamp turned that wander into inserted silence and cut samples
  on one track (audible pops). Each track is now laid out sample after sample from an anchor; only
  jumps of 100 ms or more are treated as real gaps, and slow drift is corrected by resampling the
  track by at most 0.2 %. Rejected: a wider tolerance (still pops at the outliers, and lets the
  tracks drift apart), dropping or repeating samples to follow timestamps (audible), and ignoring
  timestamps entirely (no gap handling, unbounded drift against video on long recordings).

## Sessions

- One window, one playback pipeline. Only the active session owns decoders; inactive sessions keep
  metadata, thumbnail, waveform cache and mixer state.
- Single instance via `AppInstance.FindOrRegisterForKey` + `RedirectActivationToAsync`.
- **Close all clips is one batch.** Closing sessions one by one refreshes the rail and persists the
  list once per clip (hundreds of times with 314 clips open); closing all releases the player once,
  marks every session closed so queued thumbnail and duration work is skipped, and clears the list
  with one reset. Measured on 314 real sessions: the rail was empty within 130 ms and private memory
  dropped by 75 MB. One confirmation (only for more than one clip) guards against an accidental click,
  since the list cannot be undone.

## Notifications

- **Notifications sometimes stayed on screen: their timers were collected.** Each toast created its
  dismiss timer and its fade-out timer (`DispatcherQueue.CreateTimer`) as locals. A
  `DispatcherQueueTimer` that nothing references can be garbage collected before it fires: in an
  isolated test, 40 unreferenced timers all fired without a collection and none fired after one, while
  referenced timers fired after it too. A trim allocates heavily (progress polling, the report), so a
  collection during a toast's 3.5 s was likely, and the toast stayed until newer ones pushed it out.
- `NotificationCenter` (Core) now owns the lifecycle: each notification keeps its timer referenced
  until it fires or is replaced, every step runs on the UI thread, and timers carry a generation so a
  timeout that arrives after a click is ignored. Every toast has a close button; the view only adds,
  fades and removes elements.

## Trim

- Source files are never modified. Output goes to a temp file next to the destination and is moved into
  place only after the result has been re-opened and validated.
- **Quick Cut**: stream copy from the keyframe at or before IN. Reported as keyframe constrained.
- **Several audio tracks are mixed into one by default, and can be kept separate.** Playback keeps
  desktop and microphone separate, but most players, editors and upload sites only play a file's first
  audio track, so a trim that copied the tracks separately lost the microphone wherever the clip went
  next. The mix is made in PCM by the same placement code and limiter as playback, then encoded once;
  one source track is stream-copied bit for bit. *Keep separate tracks* (for editors that use them)
  copies every track as it is. Rejected: one track only (loses audio), a libavfilter graph
  (`aresample` + `amix`: a second placement algorithm, so an export could line the tracks up
  differently from playback), and re-encoding the video to carry the change (only the audio changes).
- **A trim exports the clip's mixer (since 13 October 2026).** Until then the request carried no mixer
  state at all, and the docs said so ("the player's per-track volume, mute and solo apply to playback
  only"): every trim mixed or copied the tracks at their recorded levels, whatever the lanes showed.
  Now each track enters at the level and time the player gives it. The rule (muted or silenced by a
  solo is 0, else the volume) stays in the player and is mirrored once in `MixerState.EffectiveGain`.
  The request carries the result per source stream index, so a renamed track still maps to its own
  stream, and it is a copy taken when Save is pressed. The export reuses `AudioMixdown` and the
  playback limiter: no second mixer. In *Keep separate tracks* an untouched track is still copied and
  only changed ones are encoded, so a clip nobody adjusted exports exactly as before. Not applied: the
  master volume and Mute all, which set the listening level for every clip (13 % on the development
  PC; baking it in would make every export nearly silent), the audio sync offset (output latency) and
  the playback speed. Rejected: reading the player's live mixer settings from the trim job (shared by
  every clip, changed by the next activation) and dropping muted tracks from separate exports (the
  layout would change behind the user's back).
- **Reset volume returns to the default volume from Settings.** That default is what a clip starts
  with, so resetting to a fixed 100 % disagreed with it whenever the default had been changed.
- **Output profiles are presets of one request.** Original, High Quality, Balanced and Smaller File
  are fixed values of the same engine request that Custom fills in by hand (`TrimPlanner`). A profile
  that changes the video re-encodes the whole selection in every mode: a stream copy cannot carry a
  different codec, size or quality, and splicing re-encoded and copied parts needs identical stream
  parameters. The sheet and the report say so rather than quietly ignoring the mode or the profile.
- **The Original mix in MP4 is AAC 320 kb/s, not FLAC.** Lossless audio was the first choice, so it
  was tested against Windows' own media stack (`Windows.Media.Core.MediaSource`, which Media Player,
  Photos and Explorer previews use): FLAC inside MP4 reports `UnsupportedSubtype` (the clip plays
  silently), ALAC inside MP4 shows no audio track at all, while FLAC inside Matroska and AAC inside MP4
  are fully supported. So the Original mix is FLAC in Matroska and AAC at its highest common rate in
  MP4; FLAC in MP4 stays available in Custom with a warning.
- **Lossless only where it is real, and checked.** NVENC H.264 and HEVC encode losslessly here
  (decoded frames identical to the source's, checked with the FFmpeg CLI first); NVENC refuses lossless
  AV1, and the other encoders are not offered for it. Each lossless trim decodes its first frames and
  compares them plane by plane with the source's, failing rather than saving a lossy file under a
  lossless label.
- **A requested codec is never swapped.** Without a request the engine falls back to the most
  compatible codec (and says so); with one, it uses that codec or fails, and the sheet blocks
  combinations the container cannot hold.
- **MP4 movie timescale 1/90000 s.** A copied audio track starts at its first packet after S and is
  placed by an edit list counted in the movie timescale, which FFmpeg writes in milliseconds: copied
  tracks were up to 1 ms off (a comb filter risk when desktop and microphone are later summed).
  Measured with the separate-tracks test; the finer timescale keeps them within 11 µs.
- **The trim destination is remembered when it is chosen.** A folder picked in the sheet (or *Use
  clip folder*) becomes the default once a trim to it succeeds. Falling back to the clip's folder
  because the remembered one is unavailable does not overwrite it, so an unplugged drive is not
  forgotten after one trim.
- **Exact Cut**: video re-encoded (NVENC → AMF → QSV → GPU MFT via Media Foundation → CPU encoders),
  each candidate verified by actually opening it; source timestamps preserved (encoder time base =
  stream time base, no CFR conversion). Re-encode
  segments are decoded in software (frame-threaded) and uploaded to the encoder: simple, codec
  agnostic, and fast enough for clip lengths (4 s of 1080p60 in ~1.3 s including validation).
- **Smart Cut** (H.264): re-encode only from IN to the first IDR inside the selection, stream-copy
  the rest. The encoder's SPS/PPS are renumbered (ID 1) and stored next to the source's in `avcC`;
  re-encoded slice headers are rewritten to reference the new PPS (full header parse, CABAC alignment
  re-established). The result is a compliant `avc1` stream; no in-band parameter-set switching.
  HEVC splices only when the encoder's VPS/SPS/PPS are identical to the source's.
  Sources with B-frames are spliced when the IDR that starts the copied part opens a closed GOP (no
  later frame displays before it). The head is encoded without B-frames and its decode timestamps
  are placed the source's reorder delay before presentation, so DTS stays strictly increasing across
  the splice; the copied tail uses Quick Cut's decode-order end rule, which can add one or two
  reordered frames displayed after OUT (visible in the reported actual range). Open GOPs, a missing
  IDR inside the selection, other codecs, or a failed post-write validation fall back to Exact Cut
  with the reason in the report.
- Exact Cut keeps the source codec when an encoder for it exists; otherwise it picks the most
  compatible codec the output container accepts (`avformat_query_codec`): WebM output only ever gets
  VP9 or AV1, never H.264.

## Distribution

- One distribution format: a per-user MSI built with the WiX Toolset, containing a self-contained
  build (no prerequisites). MSIX was dropped because an unsigned MSIX cannot be installed without
  the user trusting its certificate, while an unsigned MSI installs after a SmartScreen warning.
  Worth revisiting once releases are signed ([PACKAGING.md](PACKAGING.md#why-an-msi)).
- The installer lets the app register its file types (`Syna.exe --register-file-types`, through
  `ActivationRegistrationManager`) instead of authoring them in MSI tables, so file activation uses
  the Windows App SDK's activation path and the ProgID stays the one earlier installs created.
  Default-app selection always goes through Windows Settings (`ms-settings:defaultapps?...`).
- The installer's app build is published from scratch in its own folders and never reuses the
  development build's `bin\` and `obj\`. The two builds get WinUI from different places, and a
  reused `Syna.pri` once produced an installed app that WinUI ended before it showed anything.
  `package.ps1` runs `Syna.exe --self-check` on the build and on the unpacked installer, so such a
  build is rejected before it reaches the user.
- The Windows App SDK is referenced by component (`Runtime`, `Foundation`, `InteractiveExperiences`,
  `WinUI`) instead of the `Microsoft.WindowsAppSDK` metapackage. The metapackage also brings the AI,
  ML, Search and Widgets components; in a self-contained build that is about 70 MB (ONNX Runtime,
  DirectML, indexing libraries) the app never loads.
- The FFmpeg DLLs keep all of FFmpeg's own codecs, formats and filters but only the external
  libraries Syna uses: 44 MiB instead of the 154 MiB of BtbN's build with every library. Trimming
  FFmpeg's own components further would save more but risks dropping a format someone plays, and they
  cost nothing in licensing ([FFMPEG.md](FFMPEG.md#configuration)).

## Changes driven by measurement

- **Decoder surface pool.** The engine asked FFmpeg for `extra_hw_frames` and then added the same
  headroom again to the pool size, so H.264/HEVC pools had 37 surfaces instead of 27. Each surface is
  a full frame of GPU memory (about 3 MB at 1080p NV12, 25 MB at 4K P010). Fixed after the open/close
  benchmark showed a one-time 110 MB jump: private memory dropped from about 230–300 MB to 186 MB and
  dedicated GPU memory from 140 to 111 MB, with no change in dropped frames.
- **Smart Cut for B-frame sources.** The second ShadowPlay shape uses B-frames, so Smart Cut always
  fell back to a full re-encode (4.2–4.7 s for a 15 s cut). Splicing at closed-GOP IDRs with shifted
  head decode times brought it to about 1 s.
- **Encoder probing.** The first trim of a session paid about 0.5–1.2 s to probe encoders, and the
  probe opened every candidate (CPU encoders too) even when NVENC worked. Candidates are now probed
  lazily, stopping at the first that opens, and the GPU encoders are probed once in the background
  after startup.
- **Keyframe times.** MP4 indexes decode times; with B-frames every keyframe appeared one or two frames
  early on the timeline and in the trim explanations. Keyframes are shifted by the file's decode
  delay, read from its first sample: the stream's first display time minus its first decode time
  (Matroska cues are already display times). The reorder delay a stream declares is not enough. NVENC
  with B-frames as references decodes three frames ahead and declares two, which left keyframes a frame
  early and the trim sheet predicting a re-encode that the engine then did not need (fixture R).

## Theming

- Color tokens live in theme dictionaries (Dark, Light, HighContrast), and so do the brushes built
  from them. Brushes defined once outside the theme dictionaries were shared across themes and were
  re-resolved for whichever element changed theme last, which left parts of the window on the previous
  theme after a switch in Settings.
- Code that creates brushes (timeline, toasts) resolves tokens for the element's actual theme
  (`ThemeColors`), never through `Application.Current.Resources`, which answers for the app theme.
