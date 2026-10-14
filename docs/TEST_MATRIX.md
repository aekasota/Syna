# Test matrix

`scripts\test.ps1` builds, generates missing fixtures and runs everything automated below
(80 native tests, four of which run only with `-RealClip <file>`, and 44 managed tests).

## Fixtures (`scripts\make-fixtures.ps1`)

Synthetic, reproducible media generated with an FFmpeg CLI (NVENC when present, libx264 for the
B-frame case). Video is `testsrc2` with the frame number burned in, tagged BT.709 limited range;
audio is a 440 Hz tone (track 1) and 1 kHz beeps (track 2), so mixing and offsets are measurable.

| ID | File | What it covers |
|---|---|---|
| A | `A_h264_1080p60_2trk.mp4` | ShadowPlay layout: H.264 1080p60, keyframe every second, two AAC tracks named "System sounds" / "Microphone" |
| B | `B_hevc_1080p60_2trk.mp4` | HEVC 1080p60, two tracks |
| C | `C_h264_720p30_1trk.mp4` | 720p30, one track |
| D | `D_h264_4trk.mp4` | four audio tracks |
| E | `E_h264_mixed_rates.mp4` | audio tracks at different sample rates |
| F | `F_h264_offset_track.mp4` | an audio track that starts later than the video |
| G | `G_h264_allintra_short.mp4` | every frame a keyframe, short |
| H | `H_h264_longgop_bframes.mp4` | x264 with 3 B-frames (reorder delay 2), IDR every 5 s, closed GOPs |
| I | `I_h264_vfr.mp4` | variable frame rate (90 kHz timestamps with jitter) |
| J | `J_hevc_main10.mp4` | HEVC Main 10, P010 |
| L | `L_av1_1080p60_2trk.mp4` | AV1, two tracks |
| M | `M_h264_2trk.mkv` | Matroska container with named tracks |
| N | `N_vp9_opus.webm` | VP9 + Opus in WebM |
| O | `O_aac_2trk_jitter.mp4` | audio only: a 440 Hz and a 1 kHz track whose packet timestamps wobble around the sample count like ShadowPlay's (±30 samples, one packet 4 ms early or late every 23 or 31), track 2 also drifting 1000 ppm |
| P | `P_vp9_2trk.webm` | VP9 + two Opus tracks in WebM |
| Q | `Q_h264_aac_opus_mono.mkv` | Matroska: an AAC stereo tone (3 s) and a mono Opus tone that ends a second earlier |
| R | `R_h264_nvenc_bref.mp4` | NVENC with B-frames as references: decodes three frames ahead but declares a reorder depth of two, IDR every 2 s (made only where NVENC is available) |
| K1 | `K1_truncated.mp4` | recording cut off before the `moov` box (crashed recorder) |
| K2 | `K2_corrupt_middle.mp4` | 256 KB of zeroed data in the middle |

Real recordings used (read-only, never committed): ShadowPlay clips on the development machine in
both shapes described in DECISIONS.md (no B-frames / IDR every 0.5 s, and B-frames / IDR every
4.17 s), from 10 s to 2 min, up to 117 Mb/s.

## Automated: native engine (`syna_native_tests`)

| Area | Tests | Fixtures |
|---|---|---|
| Probe | ShadowPlay layout and labels, fast mode, label suggestions for unnamed tracks, VFR detection, 10-bit and B-frames, keyframes in display time with B-frames, also when the decode delay is deeper than the declared reorder depth, other containers, truncated recording rejected, JSON through the ABI | A, B, D, H, I, J, K1, M, N, R |
| Mixer | per-track gain, mute/solo, gain ramps, limiter vs. hard clip, track offset, meters, jitter FIFO | synthetic buffers |
| Audio path | decode → placement → mixer, pulled faster than real time: every sample of each jittered track reaches the mix exactly once (a pure tone's recurrence `x[n+1] + x[n-1] = 2cos(w)x[n]` exposes any inserted, dropped or repeated sample), drift corrected to within a few ms, continuity after one and after repeated seeks, a 44.1 kHz track resampled and a mono Opus track spread over both channels without a discontinuity. The first two failed before the fix with 90 and 35 discontinuities. Opt-in: 90 s of a real two-track recording with no timestamp jumps, underruns or drift above 10 ms | E, O, Q, `SYNA_REAL_CLIP` |
| Player | first frame on D3D11VA, exact seek to the frame containing the target, frame stepping both ways, VFR stepping, long GOP with B-frames frame accuracy, real-time playback with stable sync, rate changes, mute/solo meters, end of stream, A-B loop, other codecs/containers, open failures, corrupt data, software decode fallback, frame snapshot pixels | A, B, C, H, I, J, K1, K2, L, M, N |
| Trim | Quick from keyframe (source untouched, no `.partial` left), Exact frame-accurate with cadence (PSNR against the source frame), Smart splice with renumbered parameter sets (bit-identical after the splice), Smart on a keyframe is a pure copy, Smart without an IDR falls back, Smart splice of a B-frame source (bit-identical after the IDR, continuous timestamps), WebM Exact keeps a WebM codec, VFR timestamps preserved, HEVC 10-bit depth and color kept (Smart and Exact), Matroska Quick, existing destination refused, cancel leaves no files | A, H, I, J, M, N |
| Trim audio | two tracks become one AAC track in all three modes, both sources at their levels (Goertzel), aligned with the source within 1 ms (beep onset) and 0.2 ms (tone phase), as long as the video; a late-starting track stays late with silence before it; mixed sample rates resampled with no discontinuity (two-tone recurrence cascade, with a check that it flags a single removed sample); four tracks all present; a mono Opus track that ends early mixed into stereo AAC with silence after it; jittered audio-only source mixed without discontinuities; WebM gets Opus; a single track is copied byte for byte | A, C, D, E, F, O, P, Q |
| Trim, separate tracks | kept separate in all three modes: one output track per source track, each holding only its own content (the other track's tone absent), copied in the source format, beep onset within 1 ms and tone phase within 0.2 ms of the source (this caught copied tracks sitting up to 1 ms off, from MP4 edit lists in milliseconds); re-encoded per track when a codec is asked for (AAC 128 kb/s, starting exactly at the first frame); 48 and 44.1 kHz tracks keep their rates; a late track stays late; four tracks stay four, each with its own content | A, D, E, F |
| Trim, audio settings | a mix as FLAC (24-bit, lossless) in Matroska, as 44.1 kHz mono AAC, as Opus in MP4: codec, rate and channels as asked, both sources audible | A |
| Trim, mixer | in Quick, Smart and Exact Cut (each keeping its own video path) and audio only: the tone at 50 % measures −6.02 dB ± 0.5 and the muted beeps are gone (Goertzel), still within 0.2 ms of the source; kept separate, a muted track stays a silent track of its own for the whole cut while the untouched track is still copied; −6 dB and +6 dB (200 %) per track; neutral entries change nothing (still copied); offsets of ±300 ms move the beep onset by exactly that (within 1 ms) in mix and separate; a 2.5 s delay and a 1 s advance together (audio read from before IN and past OUT, tracks 3.5 s apart in the mix) keep both tracks complete; malformed entries refused | A, O |
| Trim, output settings | Original Quick Cut decodes to the source's pixels exactly; a re-encode setting makes Quick Cut re-encode and cut frame-accurately (reported), at the source's size and 60 fps, at a lower bitrate; 720p and 30 fps when asked (1280×720, 75 frames, every interval 33.333 ms, audio aligned); a larger size or higher rate than the source is ignored; CQ 18 > 23 > 28 in bitrate, and an average-bitrate request stays under its cap; a re-encode keeps VFR intervals and HEVC 10-bit with its color tags; AV1 when asked; H.264 into WebM refused without writing a file; lossless HEVC and H.264 decode to the source's pixels (luma of three frames identical, engine check of the first three frames on every plane), lossless AV1 refused | A, I, J, N |
| Jobs | waveform envelopes match track content, thumbnail size, system info lists hardware and every usable video encoder with its lossless capability | A |
| Real clip (opt-in) | all three modes on a real recording, validated, one mixed track when the source has several; kept separate in all three modes (the source's tracks and formats); Balanced in Quick Cut (re-encoded, same size and rate, smaller); HEVC 720p30; lossless HEVC verified; the microphone muted, kept separate in all three modes (desktop copied, microphone a silent track) and mixed with the desktop at 50 % (RMS −6.02 dB against the copied desktop track) | `SYNA_REAL_CLIP` |

## Automated: managed (`Syna.Core.Tests`)

Collision-safe output names (suffix, numbering, skipping `.partial`), cache keys, timecode
formatting, waveform file parsing and column reduction, garbage rejection, database round trips
(recents, favorites, positions, sessions), settings round trip and corruption recovery, C ABI struct
layouts (`syna_player_state` 192 bytes, `syna_event` 24, `syna_process_stats` 56), probe JSON
round trip, typed errors for missing files, and the first-run import of the previous version's data:
settings and history arrive, the old folder stays byte-identical, and history still in the write-ahead
log of a running previous version is included.

Notification lifecycle (`NotificationCenter`, with a manual clock and a queue standing in for the UI
thread): dismissal after the duration and removal after the fade, longer for notifications with an
action, closing by hand cancels the timeout, closing just before the timeout dismisses once, a timeout
and a click arriving together in either order resolve to one dismissal, ten at once keep three on
screen and all leave, a new one while another fades, fifty cycles leave nothing behind, an action runs
once; and real timers still fire after forced garbage collections (the original failure).

Trim planning (`TrimPlanner`, on a ShadowPlay-like source and this PC's encoder list): Original copies
in Quick and Smart Cut and names what Exact re-encodes; the Original mix is AAC 320 kb/s in MP4, FLAC
in Matroska and Opus in WebM; separate tracks are copied unless a format is asked for or the container
needs it; one track is kept, four are never dropped; re-encoding profiles say Quick and Smart Cut lose
the copy; profiles order quality and audio bitrate without touching size, frame rate (no forced
30 fps) or codec; Custom values reach the request and the preview; lossless only where an encoder
does it; WebM with H.264 or FLAC, H.264 for 10-bit and lossless AV1 are blocked with a reason; CPU
encoders are named when hardware encoding is off. Settings: the trim choices and folder survive a
save and load, and a file from before profiles still loads. The C ABI layouts of `syna_trim_request`
(112 bytes, field offsets) and `syna_trim_track` (16 bytes) are checked on both sides.

Mixer in trims (`MixerState`, `TrimPlanner`): the player's rule (mute, solo silences the others,
muted wins over soloed, tracks past the mixer); the request carries the mixer by stream index and the
sheet names the changes ("System sounds at 30%, Microphone muted"); separate tracks copy what the mixer
leaves alone and re-encode the rest at the profile's bitrate; renaming changes only the text; a plan
keeps its copy when the clip's mixer changes afterwards; two clips get their own mixers; Reset volume
returns to the configured default. Project text: no UTF-8-read-as-Windows-1252 damage in `src`,
`tests`, `docs` and `scripts` (the check matches both damaged menu strings and nothing in valid text).

## Benchmarks

`scripts\benchmark.ps1` runs `syna_bench` on fixtures A, B, C, H, I, J, L, M, N plus any real clips:
open time, seek and step latency, 8 s of real-time playback (frames, drops, drift, CPU, GPU), memory
across reopens, waveform and thumbnail time, and all three trims with their reports. Results:
[PERFORMANCE.md](PERFORMANCE.md).

## Checked by hand on this machine

Driven through UI Automation and window capture (Windows 11 build 26200, RTX 4060 Ti, 96 DPI):

| Check | Result |
|---|---|
| First launch with a clip, explorer metadata, session thumbnails, waveforms, track labels | works |
| Settings, command palette, media info, trim sheet in dark and light themes | works after fixes: theme switching left icons and shared brushes on the previous theme (brushes moved into theme dictionaries; icons re-read their brush on theme change) |
| Media info panel | found a crash: the native DLL in the app folder was stale (missing `syna_player_diagnostics_json`); rebuilt, and palette commands now log and report failures instead of terminating |
| File activation through the registered "Open with" command | opens the clip |
| Second activation while running (single instance) | redirected to the running instance (`kind=File`), second process exits with 0 |
| Installer: install, Start menu, file activation, single instance, upgrade, downgrade, uninstall | works; see [PACKAGING.md](PACKAGING.md#tested) and the installer section below |
| Startup, playback and idle cost of the full app | see PERFORMANCE.md |
| Screen-reader names | buttons, palette rows, session rail and explorer rows have names |

### Improvement pass (audio, trim, rename to Syna), 3 October 2026

Same machine, app driven through UI Automation while minimized, master volume at 0 (no sound), on
the real two-track recordings:

| Check | Result |
|---|---|
| Two-track playback, 10.3 min continuous (looping a 98 s recording) | 37,107 frames, 0 dropped, A/V drift +2.5 to +3.1 ms; both tracks 0 timestamp jumps, 0 underruns; device starvations 0; drift correction 6 ms and 68 ms in total; memory flat (240–270 MB). Unchanged while the native trim tests ran NVENC encodes in parallel |
| Single-track clip | plays; one track in the diagnostics, 0 dropped |
| File activation and single instance | a second launch with a clip exits with 0 and the running window opens it as a new session |
| Session switching | three sessions; each reopens at its saved position (0:40.049 restored exactly) |
| Frame stepping, skips | ±1 frame per step (16.7 ms), ±1 s skips land on the frame containing the target |
| Per-track mute, solo, volume | toggle and restore; track volume 50 % shown on that lane only |
| Trim from the trim sheet (Smart Cut) | explanation names the mix; result "0.466 s re-encoded with NVIDIA NVENC (H.264), 2.534 s copied", "180 frames, 60 fps, 2 audio tracks mixed into one (AAC · 48 kHz · stereo · 256 kb/s)"; the file has one audio stream holding both tracks at 0.99 gain, aligned within 0.1 ms |
| First run after the rename | settings and history imported from `%LOCALAPPDATA%\HyakClipPlayer`; that folder byte-identical afterwards, no files added |
| Title bar, About, diagnostics overlay | new mark and `syna` wordmark; About lists Syna and the credit line; per-track audio lines shown |

Not re-run in this pass because they need keyboard, mouse-drag or full-screen input on a desktop
that was in use (their code was not changed): keyboard shortcuts, drag and drop, fullscreen. The
installer's upgrade path (replacing an installed Hyak Clip Player) was not run either: it would
replace the copy in daily use on this machine.

### Installed app launch, 3 October 2026

The installed app did not start from the Start menu or from a double-clicked clip, while `run.ps1`
worked. For the cause and the fix, see [PACKAGING.md](PACKAGING.md#startup-checks).

Verification:

- Build sequence at the time: `build.ps1`, `package.ps1 -SkipMsix`, `install.ps1` (the installer has
  replaced the last two since).
- Every launch went through the Windows shell entry points that a click uses:
  - the Start menu entry through `shell:AppsFolder`;
  - Explorer opening a file with its default app (a double-click);
  - *Open with* through the registered ProgID;
  - Explorer's default verb on a multi-selection.
- The window was minimized without activation and the master volume was 0.
- Syna is the default app for .mp4 on this machine (the user's choice).

| Check | Result |
|---|---|
| Start menu, Syna closed | one process, window shown, `activation=Launch`, working folder = install folder |
| Double-click .mp4, Syna closed | one process, `activation=File`, working folder `C:\Windows\system32`, clip opened |
| Double-click four .mp4 one after another, Syna running | each redirected and opened as a new session; the extra processes exit |
| *Open with* > Syna on .mkv, Syna running / closed | redirected and opened / started and opened |
| Clip whose path has spaces and accents | opened |
| Start menu, Syna running | redirected (`kind=Launch`), window to the front, still one process |
| Four clips opened together, Syna closed | four processes start, one remains, all four clips open (the first active, the others in the rail). Before the fix only the first opened: the other redirections arrived before the window existed and were dropped |
| Three clips opened together, Syna running | all three opened, one process |
| Application Error events during all of the above | none |
| Portable build with the stale `Syna.pri` (the original failure, reproduced) | `--self-check` exits with 2; started directly, it shows the error message and logs `startup failed` instead of vanishing |
| `run.ps1` (development build) | starts with and without clips; a second `run.ps1` with a clip is redirected to the running window. `run.ps1 -NoBuild clip.mp4` failed before (the path was bound to `-Configuration`) |
| `test.ps1` | 60 native tests passed, 2 skipped (they need `-RealClip`), 16 managed tests passed |

Not done:

- Physical mouse clicks. The entry points above are what the clicks invoke, but the clicks themselves were not performed.
- A Windows restart. The machine was in use, and the shortcut and the registrations are files and registry entries that a restart does not change.

### Trim output, notifications, Close All, 12 October 2026

Development build, driven through UI Automation with the window minimized (shown without focus only
for screenshots and menus), master volume 0. The user's settings and 314 open clips were backed up
first and restored byte for byte afterwards.

| Check | Result |
|---|---|
| Close all with the user's 314 real open clips | one confirmation ("Close all 314 clips?"); the rail empty within 130 ms (UI Automation polling included), the next UI query answered in 9 ms; private memory 364 → 289 MB, three threads fewer; run 3 s after startup, with thumbnail and duration work for the 314 clips still queued, no errors logged; the button hides at zero clips |
| Reopen right after closing all; close all while playing; one clip | the clip reopens from the empty stage's recent list and plays; closing while it plays stops it; one clip closes without a question |
| What Close all leaves | the 332 remembered files and the stars unchanged; only the session list is emptied |
| Trim sheet in each profile | the preview names what happens to this clip: stream copy for Original Smart Cut on a keyframe, "Balanced re-encodes the video, so Quick Cut cannot copy it", CQ and bitrate, the mixed track's format, the FLAC/MP4 note |
| Saved from the sheet, checked with ffprobe | Original + mix: video copied (bitrate identical to the source), one AAC track; separate: the two source tracks copied at their own bitrates (192 and 100 kb/s); Balanced 23.0 Mb/s and Smaller File 14.5 Mb/s from a 30.5 Mb/s source, all 1920×1080 at 60 fps |
| Notifications | a trim's notification left by itself after 3.7 s (3.5 s plus the fade); the close button removed one within 0.23 s; two from quick successive trims overlapped and both left on time |
| Destination | a folder confirmed earlier is offered for clips from other folders and receives the trim; it survives a restart; when it is missing, the clip's folder is used with a note and the remembered folder is kept; choosing *Use clip folder* and saving is remembered |
| Blocked combination | H.264 into `.webm`: the reason is shown and Save is disabled |
| One-track clip | the arrangement choice is disabled with "This clip has one audio track; it is kept as it is" |
| Light and dark themes | the sheet, the Custom settings, the confirmation and a notification with its close button match the existing controls in both |
| Title bar | the wordmark reads "Syna" |

Not done in this pass: the *Change folder* picker could not be driven by UI Automation here (the dialog
was created but stayed hidden while the window was not under real input); the folder it returns feeds
the same path as the checks above. Physical mouse clicks, keyboard shortcuts (including the new
Ctrl+Shift+W), drag and drop and fullscreen were not exercised: the desktop was in use.

### Mixer in trims and menu text, 13 October 2026

The installed build of 12 October (before) and the development build (after), driven through UI
Automation with master volume 0; context menus opened with UI Automation's ShowContextMenu, as a
right-click does. Two copies of fixture A (440 Hz tone on System sounds, 1 kHz beeps on Microphone);
levels are RMS in a narrow band around each frequency (ffmpeg band-pass + astats). The user's
settings, history and logs were backed up and restored byte for byte afterwards.

| Check | Before (12 October build) | After |
|---|---|---|
| Microphone muted, Mix, Quick Cut, Original | beeps in the export at −17.0 dB, as in the source: the mute was ignored | beeps at −47.5 dB (the tone's leakage into that band, as in the tone track alone), tone −24.1 dB; video copied; sheet: "Mixer applied: Voice muted." |
| Track menu (Microphone lane) | "Rename track" followed by U+00E2 U+20AC U+00A6 (an ellipsis read as Windows-1252) | "Rename track…" (U+2026) |
| Speed menu | "0.25" to "2" each followed by U+00C3 U+2014 (a multiplication sign read as Windows-1252) | "0.25×" … "2×" (U+00D7) |
| Rename track | | dialog "Rename track"; the lane becomes "Voice"; later exports still mute the same stream |
| Muted, kept separate | | two tracks: System sounds copied (every packet byte-identical to the source's; only the priming packet before the cut is left out), Voice an AAC track of digital silence; "1 copied, 1 re-encoded as AAC 320 kb/s" |
| System sounds at 50 % | | 440 Hz band −29.7 dB, which is the tone at −30.1 dB (−6.0 dB) plus the beeps' −40.3 dB leakage; beeps −17.0 dB |
| Reset volume (track menu) | | back to 100 %, export at the source's levels, no mixer note |
| Voice soloed | | tone gone (−40.3 dB, the beeps' leakage), beeps −17.0 dB; "System sounds silent, Voice soloed" |
| Two clips | | A with Voice muted, B with System sounds muted: after switching back and forth each lane shows its own clip's state, A exports only the tone, B (separate, Smart Cut) a silent first track and the beeps copied |
| Balanced | | Quick Cut became Exact (NVENC, 1920×1080, 60 fps, 23 Mb/s), mix AAC 192 kb/s with only the tone |
| Master volume | | 0 throughout; every export kept the tracks' full levels |
| Labels | | "Clip list", "Mute all", "Toggle audio tracks", Custom "Same as source (…)" and speed "Faster / Balanced / Slower" (no truncation), the Settings hints, the Close all text |

Not done: physical right-clicks and the keyboard shortcuts were not used (the desktop was in use);
the menus were opened through UI Automation, which raises the same context request.

### Installer, 13 October 2026

`msiexec /qn` with verbose logs, on the same machine; the full results are in
[PACKAGING.md](PACKAGING.md#tested). The installer log records what the app read back after
registering and unregistering its file types (all five types, then none). The app was started
through the Start menu entry and Explorer, with test clips in folders with spaces and non-ASCII
names. The existing data folder was set aside during the test and restored byte-identically.

When the app is started from a terminal hosted by an MSIX-packaged application, Windows may
redirect new files under `%LOCALAPPDATA%` into that package's private folder, so such a test run
does not see the data folder that a normal start uses. Start the installed app through Explorer or
the Start menu to see what users see.

## Not covered yet

- GPU device removal (TDR) and audio device removal/switching are handled in code (device-lost event
  and player recreation; `IMMNotificationClient` and the null sink) but were not exercised on this
  machine: forcing them needs system-level tools or changing the user's audio settings.
- Display scaling other than 100 %, multiple monitors and high contrast themes were not checked by
  hand.
- The installed app after a Windows restart, the installer's dialogs on a clean machine, a
  per-machine install and Windows 10.
- Closing the app while a trim job runs (the engine's cancel path is tested; the app's shutdown
  ordering is not). Closing the clip itself during a trim was not tried either; the job holds its own
  copy of the request and the mixer.
- Mixer exports of files with more than eight audio tracks (no fixture has more than four).
