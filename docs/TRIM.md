# Trimming

Three modes, each with a stated guarantee, combined with an output profile and an audio
arrangement. Before saving, the trim sheet shows what will actually happen to this clip (what is
copied, what is re-encoded and with which encoder); afterwards the report says what was done and
why a fallback happened. Implementation: `src/Syna.Native/src/trim/` (engine) and
`src/Syna.Core/Engine/TrimPlanner.cs` (what the sheet shows and sends).

| Mode | Video (Original profile) | Start | End | Re-encoded |
|---|---|---|---|---|
| Quick Cut | stream copy | keyframe at or before IN (may start early) | see "End rule" | no video; bit-identical to the source |
| Smart Cut | re-encode from IN to the first IDR inside the selection, copy the rest | frame containing IN | see "End rule" | the head only (at most one GOP) |
| Exact Cut | re-encode everything | frame containing IN | last frame displayed before OUT | the whole selection |

What a trim never does: modify or overwrite the source, drop an audio track, call a lossy re-encode
lossless, or change resolution, frame rate, frame timing, color tags or bit depth unless the Custom
profile asks for it (and then never upwards).

## Output profiles

| Profile | Video | Mixed audio (MP4 / Matroska / WebM) |
|---|---|---|
| Original | copied as far as the mode allows; re-encoded parts keep the source's codec, size and frame timing at near-transparent quality (CQ 16; a Smart Cut head CQ 15) | AAC 320 kb/s / FLAC (lossless) / Opus 256 kb/s |
| High Quality | the whole selection re-encoded, source codec, size and timing, CQ 18, slower preset | AAC 256 / AAC 256 / Opus 192 |
| Balanced | re-encoded, CQ 23 | AAC 192 / AAC 192 / Opus 160 |
| Smaller File | re-encoded, CQ 28, slower preset (better compression) | AAC 128 / AAC 128 / Opus 96 |
| Custom | copy when possible, re-encode or lossless, with the settings below | the chosen codec and bitrate |

CQ values are on the H.264 constant-quality scale (lower is better); each encoder gets the
equivalent on its own scale (NVENC HEVC +2, NVENC AV1 ×1.6, SVT-AV1 +8, libvpx +5, Media Foundation
0–100). Separate tracks are copied in every profile unless the mixer changes them, the container
cannot store their codec, or Custom asks for an audio format. A separate track that has to be encoded
keeps an AAC source as AAC (Opus in WebM) at the profile's bitrate from the table (Original: AAC 320
or Opus 256 kb/s).

**Profiles and modes.** A profile that re-encodes the video leaves nothing to copy, so Quick Cut and
Smart Cut behave like Exact Cut: the whole selection is re-encoded and the cut is frame-accurate.
The sheet says so before saving ("Balanced re-encodes the video, so Quick Cut cannot copy it: the
cut is frame-accurate and takes longer than a copy") and the report repeats it
(`fallbackReason`). Copying with re-encoding settings is impossible, so no combination pretends to.

**Custom.**

- Video: *Copy when possible* (as Original), *Re-encode*, or *Lossless*.
- Codec: same as source, H.264, HEVC, AV1 or VP9. Only codecs with a working encoder on this PC are
  enabled. A requested codec is used or the trim fails; it is never swapped silently.
- Resolution: 1440p, 1080p, 720p or 480p, only below the source, aspect kept, Lanczos scaling.
- Frame rate: *Same as source* keeps every frame at its own time (variable frame rate stays variable).
  60, 50, 30, 25 or 24 fps (only below the source) produce a constant rate by frame selection: each
  output slot shows the source frame displayed at that instant, with no interpolation.
- Rate control: constant quality (CQ 10–40), or an average bitrate with an optional peak.
- Speed: faster, balanced or slower; slower compresses better (NVENC p3/p5/p7, SVT-AV1 10/8/5,
  libvpx cpu-used 6/4/2, AMF/Quick Sync presets).
- Audio: automatic (copy when possible), AAC, Opus or FLAC; bitrate 128–320 kb/s; sample rate same as
  source, 48 or 44.1 kHz; channels same as source, stereo or mono.

Combinations a container cannot hold (H.264 or FLAC in WebM, H.264 for a 10-bit source, lossless AV1)
are blocked in the sheet with the reason, and Save is disabled. The profile, the audio arrangement and
the Custom settings are kept in `settings.json`.

## What Original guarantees

Three different things are often called "original quality":

1. **The source bitstream.** Quick Cut, and the part after the first IDR in a Smart Cut, write the
   source's packets unchanged: decoding them gives the source's pixels exactly (tested frame by frame).
2. **Source parameters while re-encoding.** Exact Cut and the Smart Cut head re-encode with the
   source's codec, size, frame timing, color tags and bit depth at near-transparent quality. This is a
   new lossy encode: close to the source, never identical, and never labelled lossless.
3. **Lossless re-encoding** (Custom → Lossless): every frame decodes to the source's pixels. Only
   encoders that really have a lossless mode are offered: NVENC HEVC (Main profile, plays widely),
   NVENC H.264 (High 4:4:4 Predictive profile, which many players and GPUs cannot decode) and libvpx
   VP9. NVENC refuses lossless AV1 on current GPUs, so it is not offered. Files are very large. The
   engine verifies the claim: the first frames of the output are decoded and compared plane by plane
   with the source's; if they differ the trim fails instead of saving a mislabelled file.

Mixing tracks always makes a new audio stream, so it is always encoded. In MP4 the Original profile
uses AAC at 320 kb/s rather than lossless FLAC: Windows' own media stack (Media Player, Photos,
Explorer previews) reports FLAC inside MP4 as an unsupported subtype and would play the clip silently.
In Matroska, where Windows plays FLAC, the Original mix is FLAC (24-bit, lossless); FLAC in MP4 stays
available in Custom, with that warning.

## Audio

Two arrangements, chosen in the sheet (they only apply with two or more tracks):

| Source | Mix into one track | Keep separate tracks |
|---|---|---|
| one track | that track, stream-copied | the same |
| two or more | one track mixing all of them, cut at the exact samples | every track as its own track, stream-copied |
| none | no audio | no audio |

More than two tracks are never dropped: they are all mixed, or all kept. Playback keeps the tracks
separate and individually controllable in either case; the arrangement only shapes the exported file.

### The mixer

A trim exports the tracks the way the player plays them when Save is pressed: each track's volume,
mute, solo and timing offset from the audio lanes (kept per clip, saved with the file).

| A track's mixer | Mix into one track | Keep separate tracks |
|---|---|---|
| untouched (100 %, no offset, not muted, no other track soloed) | mixed at its recorded level | copied bit for bit |
| volume changed, muted, silenced by a solo, or moved by an offset | mixed at that level and time | decoded, adjusted and encoded on its own |

- The rule is the player's (`audio/mixer.cpp`): a track is silent when it is muted or when another
  track is soloed, and otherwise plays at its volume (0–200 %). `MixerState.EffectiveGain` applies the
  same rule in the app. The request carries one entry per source track (`syna_trim_track`, API 3):
  its stream index from the probe (never its editable name), its effective gain and its offset.
- A muted or solo-silenced track is exported silent. Kept separate, it stays in the file as a silent
  track, so the number and order of tracks never change.
- The clip's mixer is read when the sheet opens and again when Save is pressed, and the request holds
  a copy. Changing the mixer, switching clips or closing the clip while a trim runs does not reach
  it, and another clip's mixer never applies.
- An offset moves a track exactly as playback does (positive = later). The engine reads that much
  audio before the output's first frame (a delay) or past OUT (an advance), and the mix buffers tracks
  that far apart.
- Gains multiply the decoded samples. The peak limiter runs on a mix of several tracks and on a single
  track the mixer boosts above 100 %; it only touches peaks above −0.3 dBFS, and nothing is normalized.
- Not applied: the master volume and Mute all (M), which set the listening level for every clip; the
  audio sync offset in Settings (output latency); and the playback speed.
- The video is unaffected: Quick Cut still copies it when the audio has to be re-encoded.

**Copied tracks** keep their packets bit for bit: the packets that start inside the selection, so a
track can begin up to one audio frame (about 21 ms for AAC at 48 kHz) after the first video frame. MP4
places such a track with an edit list in the movie timescale, which FFmpeg writes in milliseconds by
default and which shifted copied tracks by up to 1 ms (truncated, not rounded); Syna writes it at
1/90000 s, so copied tracks sit within 11 µs of their source timing. A track is re-encoded instead of
copied when the mixer changes it, when Custom asks for a codec, rate or channel count, or when the
container cannot store its codec (AAC into WebM becomes Opus).

**Encoded tracks** (the mix, or a separate track that has to be re-encoded) go through
`trim/audio_mixdown.cpp`:

- Every track is decoded from 200 ms before the output's first frame, so the first samples are
  complete, and placed on the output timeline by `TrackConverter`, the code playback uses: decoded
  samples stay continuous, timestamps anchor the track and reveal real gaps (100 ms or more, kept as
  silence), and slow drift between a track's timestamps and its sample count is absorbed by
  resampling it by at most 0.2 %. The tracks keep their recorded timing relative to each other and
  to the video; nothing is shifted to line them up. Encoded tracks start exactly at the first frame.
- Different sample rates and channel layouts are converted to the requested ones, or else the highest
  rate and the widest layout among the tracks (a mono microphone lands in both channels of a stereo mix).
- A track that starts later, ends earlier or has gaps contributes silence there. The output ends at
  OUT, or earlier where the longest track ends.
- Each track is scaled by its mixer gain and moved by its offset. A mix is summed through the playback
  mixer's peak limiter (−0.3 dBFS, instant attack), so the sum cannot clip; a single encoded track
  passes without it unless the mixer boosts it.
- Codec: the requested one, or automatically the first source track's codec when the container
  accepts it, then AAC, Opus, Vorbis, FLAC or MP3 (`avformat_query_codec`). Default bitrate AAC 128
  kb/s and Opus 80 kb/s per channel; FLAC is written with 24-bit samples. The encoder delay is recorded
  in the container (MP4 edit list, Matroska codec delay), so audio and video start together.
- A mixed track is named after the tracks it contains when they have names ("System sounds +
  Microphone") and keeps their language when they agree; a separate re-encoded track keeps its own
  title, language and disposition.

If the tracks cannot be mixed (a track cannot be decoded, or the container takes none of the codecs)
they are kept separate and the report says why.

## End rule (stream copy)

Copying cannot cut inside a GOP at the end either, but it can stop early: the output receives the
decode-order prefix of packets that contains every frame displayed before OUT. Without B-frames this
is exact. With B-frames, a reference frame displayed after OUT may be needed by a B-frame displayed
before it, so the output can include one or two frames past OUT. The report's actual range shows it.

## Smart Cut

1. Find the first IDR inside the selection (K2) by scanning packets, without decoding. With frame
   reordering, the GOP that starts at K2 must be closed: no later packet may be displayed before K2.
2. Decode from the keyframe before IN and re-encode the frames displayed in [IN, K2) with the same
   codec, no B-frames, and the highest quality preset.
3. Copy the source packets from K2 to the end rule.

**H.264.** The encoder's SPS/PPS rarely match the source's. They are renumbered (next free IDs)
and stored next to the source's in the `avcC` box, and every re-encoded slice header is rewritten
to reference the new PPS (full slice header parse, CABAC alignment re-established). The result is
a standards-compliant `avc1` stream with two parameter-set pairs, not a stream that relies on
in-band parameter-set switching. In-band SPS/PPS in the copied part (ShadowPlay repeats them before
every IDR) keep their original IDs, so they never collide with the head's.

**B-frames.** The copied part keeps the source's decode delay (DTS = PTS − reorder delay). The
head has no reordering, so its decode timestamps are placed the same delay before presentation;
decode order then stays strictly increasing across the splice, and every frame keeps its
presentation time.

**HEVC** splices only when the encoder emits exactly the source's VPS/SPS/PPS (rare across
encoders).

**Falls back to Exact Cut** (and says why) when: the codec is not H.264/HEVC, the parameter sets are
not in the container header, the container is not MP4/MOV/Matroska, there is no IDR inside the
selection, K2 opens an open GOP, no encoder for the source codec is available, the parameter sets
cannot be renumbered, or the finished file fails validation.

## Re-encoding (Exact Cut and re-encoding profiles)

Frames are decoded in software (frame-threaded) and encoded with the first encoder that actually
opens, in this order: NVENC → AMF → Quick Sync → a GPU encoder through Media Foundation → CPU
(Media Foundation software, OpenH264, Kvazaar, SVT-AV1, libvpx). Each candidate is probed once per
process by opening it at a small size (and again in lossless mode for lossless requests), so a missing
driver costs one probe, not a failed trim. The probe results are also what the trim sheet offers
(`syna_query_system_json` → `videoEncoders`).

- Without a requested codec, the source codec is kept when an encoder exists for it; otherwise the
  most compatible codec that the output container accepts is used (`avformat_query_codec`): WebM only
  ever receives VP9 or AV1. 10-bit sources stay 10-bit (H.264 has no 10-bit encoder here, so they go to
  HEVC Main 10).
- Unless a frame rate is requested, the encoder time base is the stream's time base and source
  timestamps are passed through, so variable frame rate recordings keep their exact frame timing.
  A requested rate uses a 1/rate time base and the frame selection described under Custom.
- Color range, matrix, primaries, transfer and chroma location are copied to the encoder; HDR and
  rotation side data are carried over. The keyframe interval matches the source's (in seconds).
- Rate control: NVENC VBR with a constant-quality target or an average bitrate, optional peak
  (`maxrate`, buffer 2× peak), spatial AQ, no B-frames; equivalent settings for the other encoders.
  Hardware encoding can be disabled in Settings.

## Output safety and destination

- The destination must not exist and must not be the source; the sheet proposes collision-free names
  (`clip_trimmed.mp4`, then `clip_trimmed_2.mp4`, ...). The name follows the folder until edited.
- The trim writes `<destination>.partial`, validates it, then moves it into place without
  replacing anything (`MoveFileEx` without `REPLACE_EXISTING`). Cancelling, a failure or a crash
  leaves no partial file behind on the next attempt.
- The destination folder is remembered (`settings.json` → `trimFolder`): a folder chosen in the
  sheet with *Change folder*, or *Use clip folder* (empty = next to each source clip), becomes the
  default for every clip once a trim to it succeeds, across restarts. When the remembered folder is
  missing (an unplugged drive), the clip's folder is used with a note, and the remembered folder is
  kept for when it comes back. Settings > Files shows and changes the same value.
- Errors are specific: disk full, access denied, read errors, no usable encoder, an unavailable folder.

## Validation

After writing, the output is reopened and checked:

- it opens and contains video (if the source had video);
- it has exactly the expected audio tracks, in order: copied tracks with the source's codec, rate and
  channel count; encoded tracks in the encoder's format, as long as what was encoded (within 50 ms),
  their first two seconds decoding without errors;
- resolution equals the source's, or the requested size;
- the container's frame count equals the number of frames written (formats that store one);
- the average frame rate is within 2 % of the source's (non-VFR sources) or of the requested rate;
- the duration matches the written range within max(0.1 s, 3 frames);
- the first frames, through 1.5 s past a Smart Cut splice, decode without errors and every packet
  sent produces a frame;
- a lossless re-encode decodes to exactly the source's pixels (first three frames, every plane).

A Smart Cut that fails validation is redone as an Exact Cut automatically; any other failure is
reported and nothing is moved into place.

## Report

`syna_job_result_json` (typed as `TrimReport` in Core):

| Field | Meaning |
|---|---|
| `requestedMode`, `modeUsed` | `quick`, `smart` or `exact` |
| `success`, `error`, `fallbackReason` | outcome and plain-language reasons |
| `requestedInUs`, `requestedOutUs`, `actualStartUs`, `actualEndUs` | file timeline, microseconds |
| `videoHandling` | what was asked: `original`, `encode` or `lossless` |
| `videoMethod` | what was done: `copy`, `smart` (head re-encoded, rest copied) or `reencode` |
| `encoder`, `hardwareEncoder` | encoder label actually used, and whether it is a GPU encoder |
| `losslessVideo`, `losslessVerified` | lossless was requested; the output's first frames matched the source's pixels |
| `reencodedSeconds`, `copiedSeconds`, `elapsedSeconds` | what was done and how long it took |
| `audioMethod`, `audioTracks`, `audioEncoder`, `audioNote` | `mix`, `copy`, `encode`, `copy+encode` or `none`; source track count; the first encoded format (e.g. `AAC · 48 kHz · stereo · 256 kb/s`); why tracks were kept separate when a mix was asked for |
| `audioOutputs` | per output track: `method` (`copy`, `encode`, `mix`), source `tracks`, `description`, `adjusted` (a mixer gain or offset was applied) |
| `validation` | `passed`, duration, size, codec, fps, video bitrate, frame count, first frame time, audio streams/codecs/rates/layouts, `issues` |

## Known limitations

- Subtitle and data streams are not carried into trimmed files (video and audio are).
- A mix is always encoded, even by Quick Cut (the video is still copied). The AAC encoder runs about
  40× real time: a 40 s selection takes about 1 s longer. The same holds for a separate track the
  mixer changes; it is encoded lossy (AAC or Opus) at the profile's bitrate, FLAC only through Custom.
- Tracks after the eighth have no mixer controls (the player plays eight); they are exported at their
  recorded level, or silent while another track is soloed.
- The AAC encoder works at an average rate: simple content (test tones) uses well below the chosen
  bitrate; dense gameplay audio reaches it.
- Smart Cut needs an IDR inside the selection; for very short selections between keyframes it
  becomes an Exact Cut.
- VP9 and AV1 sources are trimmed with Quick or Exact Cut only.
- Frame-rate conversion selects frames; it does not blend or interpolate, so uneven ratios (60 → 25)
  repeat the source's motion unevenly.
- Re-encoding is software decode + GPU encode; CPU-only encoders are much slower than
  NVENC/AMF/Quick Sync (see PERFORMANCE.md for measured times).
