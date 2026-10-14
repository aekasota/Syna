# FFmpeg in Syna

Syna's engine uses FFmpeg's libraries. They are built for Syna from pinned sources by
[`scripts/ffmpeg/build.sh`](../scripts/ffmpeg/build.sh): an LGPL build with no GPL code, shipped
as six DLLs next to `Syna.exe`. This page records what Syna uses, how the build is configured,
what it contains and what the licenses ask, and how to build and update it.
[LICENSE-AUDIT.md](LICENSE-AUDIT.md) explains why the earlier BtbN build had to be replaced.

## What Syna uses

| Area | FFmpeg feature | Code |
|---|---|---|
| Opening files | `avformat_open_input` without a forced format, so every demuxer; the `file` protocol. The app offers `.mp4 .m4v .mov .mkv .webm` | `common/ffmpeg.cpp`, `Syna.Core/Services/MediaFiles.cs` |
| Decoding | `avcodec_find_decoder` for each stream's codec, so any decoder FFmpeg has. Software AV1 goes through libdav1d, which FFmpeg prefers; hardware AV1 through FFmpeg's own `av1` decoder | `video/video_decoder.cpp`, `media/*.cpp`, `trim/*.cpp` |
| Hardware decoding | D3D11VA (`AV_HWDEVICE_TYPE_D3D11VA`) | `video/video_decoder.cpp`, `renderer/renderer.cpp` |
| Speed changes | Filter graph `abuffer` → `atempo` → `abuffersink`; FFmpeg adds format conversion filters itself | `audio/tempo.cpp` |
| Audio conversion | libswresample | `audio/track_converter.cpp`, `media/waveform.cpp`, `trim/audio_mixdown.cpp` |
| Pixel conversion | libswscale (thumbnails, unusual pixel formats); thumbnails are written by Windows' WIC | `media/thumbnail.cpp`, `renderer/renderer.cpp`, `trim/trim.cpp` |
| Video export | The first encoder that opens, in this order: `h264_nvenc` `hevc_nvenc` `av1_nvenc` (NVIDIA), `h264_amf` `hevc_amf` `av1_amf` (AMD), `h264_qsv` `hevc_qsv` `av1_qsv` `vp9_qsv` (Intel), `h264_mf` `hevc_mf` (Media Foundation), then `libopenh264`, `libkvazaar`, `libsvtav1`, `libvpx-vp9` on the CPU | `trim/encoders.cpp`, `trim/trim.cpp` |
| Audio export | `aac` (FFmpeg's), `libopus`, `libmp3lame` | `trim/audio_mixdown.cpp` |
| Containers written | `mp4`/`mov` and `matroska`/`webm`, from the extension (`.mp4 .mkv .webm`) | `trim/trim.cpp`, `Syna.Core/Engine/TrimPlanner.cs` |

## Configuration

All of FFmpeg's own code stays in: its decoders, demuxers, muxers, parsers, bitstream filters,
filters and protocols are LGPL, cost nothing in licensing, and keep Syna playing whatever FFmpeg can
decode. External code is limited to what the table above needs. `--disable-autodetect` keeps
configure from picking up anything else, and each component is enabled by name:

| Options | Provide |
|---|---|
| `--enable-w32threads` | Windows threads |
| `--enable-d3d11va --enable-d3d12va --enable-dxva2` | Windows hardware decoding (Syna uses D3D11VA) |
| `--enable-mediafoundation` | `h264_mf`, `hevc_mf` |
| `--enable-schannel` | Windows TLS for FFmpeg's network protocols, as in the earlier build; Syna itself opens local files |
| `--enable-ffnvcodec --enable-cuda --enable-cuvid --enable-nvdec --enable-nvenc` | NVIDIA encoding and decoding, through the driver at run time |
| `--enable-amf` | AMD encoding, through the driver |
| `--enable-libvpl` | Intel Quick Sync, through the oneVPL dispatcher |
| `--enable-zlib` | zlib-compressed data (PNG, compressed MP4 headers, Matroska) |
| `--enable-libdav1d` | AV1 software decoding |
| `--enable-libopus --enable-libmp3lame` | Opus and MP3 export |
| `--enable-libopenh264 --enable-libkvazaar --enable-libsvtav1 --enable-libvpx` | H.264, HEVC, AV1 and VP9 software encoding |
| `--disable-filter=perlin --disable-filter=codecview` | See [below](#gpl-files-in-ffmpegs-own-tree) |

There is no `--enable-gpl`, `--enable-nonfree` or `--enable-version3`: none of these libraries needs
them, so FFmpeg is LGPL-2.1-or-later and every DLL reports "LGPL version 2.1 or later". The complete
configuration is in [ffmpeg-buildconf.txt](ffmpeg-buildconf.txt); `ffmpeg -version` reports
`8.1.3-g330caae0c1-syna`.

Compared with the BtbN build that 1.0.0 was first tested with: FFmpeg is the same commit; the
libraries Syna uses are their latest releases instead of BtbN's development snapshots, with the
same build options (libvpx with high bit depth VP9, for example); nearly 70 other libraries are gone.
Among them were FFTW and libzvbi, whose GPL code made that build undistributable, and with them
features Syna never used: JPEG XL, AVS3, APV encoding, ARIB and teletext captions, LCEVC enhancement,
module and game music, Blu-ray, SVG, text and subtitle rendering filters, Vulkan, OpenCL and
libplacebo filters, VMAF, and the SFTP, SRT, RIST and ZeroMQ protocols. The six DLLs shrank from
154 MiB to 44 MiB.

### GPL files in FFmpeg's own tree

FFmpeg keeps its GPL-only parts in the same tree and builds them only with `--enable-gpl`. Two of
them are not marked that way: `libavfilter/perlin.c` (the `perlin` source filter) and
`libavfilter/qp_table.c` with `qp_table.h` (used by the `codecview` filter) carry GPL-2.0-or-later
notices, but configure builds them into every LGPL build. This is so in 8.1.3 and in FFmpeg's
current master. Syna's build disables both filters, and `build.sh` checks every object it compiles:
it fails if any comes from a source file, or includes a header, whose notice is the GPL. The
libraries' trees may not hold such files at all. The result is recorded in `BUILDINFO.txt`
(`GPL source check: passed`), and `check-ffmpeg-license.ps1` requires that line.

## Libraries in the DLLs

| Library | Version | License | Shipped with the release |
|---|---|---|---|
| FFmpeg | 8.1.3, commit `330caae0c1` | LGPL-2.1-or-later | source, license text |
| LAME | 3.100 | LGPL-2.0-or-later | source, license text |
| dav1d | 1.5.4 | BSD-2-Clause; AOMedia Patent License 1.0 | source, license and patent texts |
| Opus | 1.6.1 | BSD-3-Clause | source, license text |
| OpenH264 | 2.6.0 | BSD-2-Clause | source, license text |
| Kvazaar | 2.3.2 | BSD-3-Clause | source, license text |
| SVT-AV1 | 4.2.0 | BSD-3-Clause-Clear; AOMedia Patent License 1.0 | source, license and patent texts |
| libvpx | 1.17.0 | BSD-3-Clause; Google's patent grant | source, license, patent and authors texts |
| Intel libvpl (dispatcher) | 2.17.0 | MIT | source, license text |
| nv-codec-headers | 13.0.19.1 (Video Codec SDK 13.0, drivers from early 2025) | MIT | source, license text |
| AMD AMF headers | 1.5.3 | MIT | source, license text |
| zlib | 1.3.2 | Zlib | source, license text |
| mingw-w64 runtime and winpthreads (from llvm-mingw 20261006) | 15.0 development snapshot | ZPL-2.1 with MIT, BSD and public-domain parts; winpthreads MIT and BSD-3-Clause | license texts |
| LLVM libc++, libunwind, compiler-rt (from llvm-mingw 20261006) | LLVM 23.1.3 | Apache-2.0 with the LLVM exception | license text |

Each license was read in the archive the build uses. Every library's source tree was searched for
files under other licenses: none of them holds GPL-licensed source. Kvazaar's tree has a test
library under ISC (`LICENSE.EXT.greatest`), which is not built.

What the licenses ask of a distributor, and how the release does it:

- **LGPL (FFmpeg, LAME):** the license text, and the complete corresponding source of the DLLs offered
  next to them. The release attaches every source archive unchanged together with the recipe that
  built the DLLs from them, and the archive `scripts/ffmpeg/` gives the exact steps. The DLLs are
  loaded by name, so a user can build modified ones from these sources and replace them. Syna's MIT
  License allows the reverse engineering needed to debug such a change.
- **BSD, MIT, ZPL, Zlib (the rest):** their copyright notices and license texts with the binaries.
  They are installed in `licenses\ffmpeg\`.
- **Apache-2.0 with the LLVM exception (the compiler's runtime):** the exception waives the notice
  requirements for these runtime parts. The text ships anyway.
- The permissive libraries' sources are attached too: the DLLs combine them with FFmpeg, and the
  LGPL's corresponding source covers the whole library.

## Sources and notices in a release

- `build.sh` downloads each archive from the URL in the script and checks its SHA-256 before using
  it. The archives stay unchanged in `source\` next to the package, with
  `syna-ffmpeg-recipe-<recipe>.tar.gz` (the `scripts/ffmpeg/` folder that ran) and
  `ffmpeg-sources.sha256`.
- The license texts come from the same archives: `build.sh` copies them into the package's
  `licenses\` folder, and the app installs that folder as `licenses\ffmpeg\`. The NVIDIA and AMD
  headers carry their MIT license only in the header files; it is copied from there.
- The release workflow builds FFmpeg in the same run, collects these files
  (`fetch-deps.ps1 -SourceTo`), checks them (`check-ffmpeg-license.ps1 -SourceDir`: every archive the
  build used, unchanged; the recipe of this build; each library's archive; every license text), and
  attaches them to the draft release. A failure stops the release before packaging.

## Building

- **On Windows:** `.\scripts\build-ffmpeg.ps1`. The first run creates a WSL distribution of its own,
  `syna-build`, from Ubuntu's 24.04 WSL image (pinned by SHA-256) and leaves other distributions
  alone. Every run installs the build tools there (`setup-ubuntu.sh`), builds into
  `third_party\ffmpeg-build\` and unpacks the result into `third_party\ffmpeg\`. A build takes about
  four minutes on a 6-core CPU, plus the first downloads (about 560 MB, the image included).
- **On Linux:** on Ubuntu 24.04, `sudo bash scripts/ffmpeg/setup-ubuntu.sh`, then
  `bash scripts/ffmpeg/build.sh <output folder>`.
- **In CI:** the `ffmpeg` job builds in an `ubuntu:24.04` container pinned by digest, and reuses the
  last build while `scripts/ffmpeg/` is unchanged. The release workflow always builds afresh.

The build tools come from the Ubuntu archive as it was on 2026-10-01 (`setup-ubuntu.sh`): NASM 2.16.01,
CMake 3.28.3, Meson 1.3.2, Ninja 1.11.1. The compiler is llvm-mingw 20261006 (clang 23.1.3, Universal
C Runtime), pinned by SHA-256. Flags: `-O2`, stack protection and `_FORTIFY_SOURCE=2`; every library,
the compiler's runtime included, is linked statically, so the DLLs import only Windows system
libraries. Build paths and dates do not end up in the binaries: two builds of the same recipe in
the WSL distribution gave byte-identical packages. CI builds in the same pinned environment, so its
package should be identical too.

The package is named `ffmpeg-syna-<recipe>-win64.zip`, where `<recipe>` is the first 12 hex digits
of the SHA-256 of `build.sh`. `fetch-deps.ps1` only uses the package of the current recipe, and
`check-ffmpeg-license.ps1` fails when `BUILDINFO.txt` names another one.

## Updating FFmpeg or a library

1. Change the archive name, URL and SHA-256 (or an option) in `scripts/ffmpeg/build.sh`.
2. Build it (`.\scripts\build-ffmpeg.ps1`) and record the configuration:
   `third_party\ffmpeg\bin\ffmpeg.exe -hide_banner -buildconf > docs\ffmpeg-buildconf.txt`.
3. Run `.\scripts\check-ffmpeg-license.ps1 -SourceDir third_party\ffmpeg-build\source`. A new library
   needs an entry in its list, with its license text (copied in `build.sh`) and its source archive.
4. Run the full test suite, and update this page and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Patents

The licenses above are copyright licenses, and the release meets them. Whether using or
distributing these codecs needs patent licenses is a separate question, which this page does not
settle:

- Royalty-free patent licenses come with dav1d and SVT-AV1 (AOMedia Patent License 1.0, for AV1),
  libvpx (Google's grant for VP8 and VP9) and Opus (the IETF declarations its `COPYING` refers to).
  Each has conditions, such as ending if the licensee sues over patents.
- H.264 (OpenH264, FFmpeg's decoder), HEVC (Kvazaar, FFmpeg's decoder) and AAC (FFmpeg's encoder and
  decoder) are covered by patent pools in some countries, and no license comes with the code. Cisco
  pays the H.264 royalties only for OpenH264 binaries it distributes itself, not for OpenH264 built
  from source. BSD-3-Clause-Clear (SVT-AV1) explicitly grants no patent rights beyond the separate
  patent license.
- The hardware encoders and Media Foundation run codec implementations of the GPU vendor or of
  Windows.
- MP3's patents have expired.

Anyone distributing Syna commercially should have this assessed for their markets.
