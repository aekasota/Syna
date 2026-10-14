# License audit for the first release

Audit of what `Syna-Setup-1.0.0-x64.msi` redistributes, done on 2026-10-13 before the first public
release. It records what was examined, what was found and what has to change. It reads the license
texts as a developer would; it is not legal advice.

## Status

The FFmpeg findings are resolved: the BtbN build was replaced the same day by Syna's own LGPL build
of the same FFmpeg commit ([FFMPEG.md](FFMPEG.md)), which `scripts/check-ffmpeg-license.ps1`
accepts. The rest of this page is the audit of the BtbN build as it was done.

| Finding | Resolution |
|---|---|
| FFTW (GPL-2.0-or-later) in `avformat-62.dll` | Not in the new build: no Chromaprint, no FFTW, and the check finds no FFTW code |
| GPL-2.0-only files of libzvbi in `avcodec-62.dll` | Not in the new build: no libzvbi, and the check refuses `--enable-libzvbi` |
| Sources and license texts of 78 other libraries not shipped | The new build has 11 libraries. Releases attach their archives unchanged with the recipe that built the DLLs; their license texts are installed in `licenses\ffmpeg\` |
| BtbN deleting the pinned build after about two weeks | Nothing is downloaded from BtbN: each source comes from its project, pinned by SHA-256, and CI builds FFmpeg itself |
| Uncertainty 1: licenses of individual files | Every library's source tree was searched: none holds GPL-licensed source. In FFmpeg's own tree, two GPL-licensed files are built without `--enable-gpl` by upstream's configure (`perlin.c`, and `qp_table.c` for `codecview`); both filters are disabled, and `build.sh` now fails when any compiled object comes from, or includes, a GPL-licensed file |
| Uncertainty 2: Rust crates, shaderc's dependencies | Not in the new build |
| Uncertainty 3: libssh and SVT-AV1 license files not read | libssh is not in the new build; SVT-AV1's license and patent files were read in the 4.2.0 archive the build uses |
| Uncertainty 4: pass-through of Microsoft's terms | Unchanged: a question about Microsoft's terms, not about FFmpeg |
| Uncertainty 5: patents | Kept apart from the copyright questions in [FFMPEG.md](FFMPEG.md#patents); still to be assessed before commercial distribution |

## Result

- **The FFmpeg DLLs must not be distributed with Syna.** BtbN labels the build LGPL, and FFmpeg's
  own code in it is LGPL. But two DLLs also contain GPL code from libraries built into them: FFTW
  in `avformat-62.dll` and GPL-2.0-only files of libzvbi in `avcodec-62.dll`
  ([details](#gpl-code-in-the-dlls)). The FFmpeg build has to be replaced before 1.0.0 is
  published ([way forward](#way-to-a-releasable-build)).
- **Even without that code, the release would not carry what the licenses require.** The DLLs
  contain 78 other libraries. The release attached the source of FFmpeg and BtbN's build scripts,
  but not the source of the 19 LGPL and MPL libraries among them, and none of their license texts
  or notices ([details](#corresponding-source-and-notices)).
- **The Microsoft components may be redistributed** under their terms. Two gaps were closed: the
  Windows App SDK's notice file is now installed, and the installer's license page now also covers
  the Windows SDK projection ([details](#microsoft-components)).
- `scripts/check-ffmpeg-license.ps1` now checks the FFmpeg DLLs, and the release workflow stops when
  it fails ([details](#how-the-release-is-guarded)). On the current DLLs it fails.

## What was examined

| Item | Identity |
|---|---|
| Installer | `Syna-Setup-1.0.0-x64.msi`, SHA-256 `41a042282c34edb890272a86eb06b705d9c57cc9f56092c0fbcc02f36c078f63`, built from commit `55e8f6f` |
| FFmpeg DLLs, extracted from the installer | `avcodec-62.dll` `e7f1183e…3055`, `avformat-62.dll` `30062c73…c4ff`, `avutil-60.dll` `88befd1b…1a1e`, `avfilter-11.dll` `d854a2c4…1e74`, `swresample-6.dll` `8d71f620…2a87`, `swscale-9.dll` `73175a66…bdf7` (SHA-256), byte-identical to those in the pinned archive |
| Pinned archive | `ffmpeg-n8.1.3-14-g330caae0c1-win64-lgpl-shared-8.1.zip` from BtbN release `autobuild-2026-10-01-13-06`, SHA-256 `bf545d8f…68a4` (`scripts/fetch-deps.ps1`) |
| Build configuration | Read from the DLLs through `avutil_configuration()` and the other libraries' equivalents, and through their `*_license()` functions: the same in all six DLLs, and identical to [ffmpeg-buildconf.txt](ffmpeg-buildconf.txt) |
| Build recipe | BtbN FFmpeg-Builds at tag `autobuild-2026-10-01-13-06`. Its dependency resolution (`generate.sh`) was re-run for target `win64`, variant `lgpl-shared`, add-in `8.1`; it yields exactly the `--enable` and `--disable` options in the DLLs |
| Licenses | The license files of every library at the commit BtbN pins, fetched from the projects' repositories; for libzvbi, game-music-emu and Chromaprint also the files that decide which parts are built |
| Microsoft terms | `license.txt` and `NOTICE.txt` of the Windows App SDK packages in the build; the Windows SDK license and its redistribution list; the WebView2 SDK's `LICENSE.txt` |

## The FFmpeg build

- FFmpeg 8.1.3 (`n8.1.3-14-g330caae0c1`), configured with `--enable-version3` and without
  `--enable-gpl` and `--enable-nonfree`. All six DLLs report "LGPL version 3 or later".
- 61 `--enable` options name external libraries or APIs. With their dependencies, BtbN builds 87
  libraries; the 80 that the six DLLs use are linked into them statically
  (`--pkg-config-flags=--static`). The DLLs import only Windows system libraries and each other.
- Toolchain: GCC 16.2.0 and mingw-w64 (commit `57b5950390`, UCRT), with `-static-libgcc`,
  `-static-libstdc++`, winpthreads and `--extra-libs=-lgomp`.
- Syna ships `avcodec`, `avformat`, `avutil`, `avfilter`, `swresample` and `swscale`. It does not
  ship `avdevice-62.dll`, `ffmpeg.exe`, `ffprobe.exe` or `ffplay.exe`, so SDL2, OpenAL Soft and
  libsamplerate, which only they use, are not distributed.

## GPL code in the DLLs

### FFTW in avformat-62.dll

BtbN builds Chromaprint (FFmpeg's `chromaprint` muxer) with `-DFFT_LIB=fftw3` and builds FFTW
3.3.11 for every variant (`scripts.d/50-chromaprint.sh`, `scripts.d/25-fftw3.sh`). FFTW is
GPL-2.0-or-later (its `COPYRIGHT` file at commit `93ed4c7869`). `avformat-62.dll` contains the
string `fftw-3.3.11 fftw_wisdom` and 985 distinct `fftw_codelet_*` names. Chromaprint's license file
itself warns that a binary distribution has to consider the license of the FFT library it was built
with. FFmpeg's configure treats Chromaprint as LGPL-compatible and does not know about FFTW, which
is why the DLLs still report LGPL.

### GPL-2.0-only code of libzvbi in avcodec-62.dll

libzvbi (commit `d3a5ee9f2b`) is LGPL-2.0-or-later except for some files. Its `COPYING.md` lists
`src/packet-830.*` and `src/pdc.*` as GPL-2.0-only, and their file headers say version 2 without
"or later". Both are part of the library's sources (`libzvbi_la_SOURCES`). FFmpeg's
`libzvbi_teletext` decoder calls `vbi_decode()` (`vbi.c`), which calls `vbi_decode_teletext()`
(`packet.c`), which calls `vbi_decode_teletext_8301_local_time()` and
`vbi_decode_teletext_8302_pdc()` from `packet-830.c`. `avcodec-62.dll` contains assertion strings of
`vbi.c`, `teletext.c` and `lang.c`, so the decoder is linked in, and BtbN links without
`--gc-sections`, which keeps every object file the linker pulls in. `pdc.c`'s strings are absent,
so `pdc.o` is apparently not linked; `packet-830.o` has to be, or the link would fail.

### Consequence

A DLL with GPL code in it can only be distributed under the GPL as a whole. For `avcodec-62.dll`
even that is not possible: GPL-2.0-only code cannot be combined with the Apache-2.0 and LGPL-3.0
code in the same DLL. On the FSF's reading, which treats a program and the libraries it loads into
its process as one work, Syna itself would also have to be distributed under the GPL. Its MIT code
allows that, but the Windows App SDK in the same process does not: Syna redistributes it under
Microsoft's terms, and the GPL's exception for system libraries does not cover a library that the
application ships itself.

BtbN's `lgpl` variants share this. It may be worth reporting upstream.

## Libraries built into the DLLs

Obligation for distributing the DLLs: **GPL**: cannot be distributed with Syna; **S**:
corresponding source, license text and notices (LGPL, MPL); **N**: license text or copyright
notice; **-**: nothing for binaries; **x**: built by BtbN, but not in the six DLLs Syna ships. The
pinned commits are in the BtbN scripts at the tag above.

| Library | Pinned at | License | Obligation |
|---|---|---|---|
| FFmpeg | `330caae0c1` | LGPL-2.1-or-later, used as LGPL-3.0-or-later (`--enable-version3`) | S |
| FFTW 3.3.11 | `93ed4c7869` | GPL-2.0-or-later | GPL |
| libzvbi | `d3a5ee9f2b` | LGPL-2.0-or-later, but src/packet-830.c and src/pdc.c are GPL-2.0-only | GPL |
| aribb24 (three BtbN patches) | `5e9be272f9` | LGPL-3.0-or-later | S |
| cairo | `74755964ed` | LGPL-2.1 or MPL-1.1 | S |
| Chromaprint (built with FFTW) | `aed8eba220` | MIT, with LGPL-2.1 parts: LGPL-2.1 as a whole | S |
| Game_Music_Emu (Nuked OPN2 core, the default) | `f68963b1de` | LGPL-2.1-or-later | S |
| GLib | `5a124d1e66` | LGPL-2.1-or-later | S |
| GMP (BtbN's mirror) | `763b40df71` | LGPL-3.0-or-later or GPL-2.0-or-later | S |
| GNU FriBidi | `4c914a92e9` | LGPL-2.1-or-later | S |
| GNU libiconv (with gnulib) | `1df3087ba8` | LGPL-2.1-or-later (library) | S |
| LAME | `r6835` | LGPL-2.0-or-later | S |
| libbluray | `a24f4fad4d` | LGPL-2.1-or-later | S |
| libplacebo | `1937beef3a` | LGPL-2.1-or-later | S |
| librsvg, with its Rust crates | `7612431eb0` | LGPL-2.1-or-later (crates MIT, Apache-2.0, some MPL-2.0) | S |
| libssh | `76e0a8639e` | LGPL-2.1 (upstream; file not retrievable at the commit) | S |
| libudfread | `b0bc6957e7` | LGPL-2.1-or-later | S |
| Pango | `8e74c27c11` | LGPL-2.0-or-later | S |
| SoX Resampler | `945b592b70` | LGPL-2.1-or-later | S |
| SRT | `ff8ab25c57` | MPL-2.0 | S |
| TwoLAME | `6fced852d4` | LGPL-2.1-or-later | S |
| ZeroMQ (libzmq) | `4649337021` | MPL-2.0 | S |
| AMD AMF headers | `6277e353fd` | MIT | N |
| Brotli | `a3abcbee0d` | MIT | N |
| dav1d | `9711965b60` | BSD-2-Clause | N |
| fontconfig | `bd8f7b597d` | MIT-style (HPND) | N |
| FreeType | `d333439633` | FTL (or GPL-2.0); FTL asks for credit in the documentation | N |
| HarfBuzz | `b3bab62307` | MIT (Old MIT) | N |
| Intel libvpl (dispatcher) | `674d015bcb` | MIT | N |
| Kvazaar | `2b06691bb5` | BSD-3-Clause | N |
| LCEVCdec | `17804ac54d` | BSD-3-Clause-Clear; grants no patent license | N |
| libaom (one BtbN patch) | `b4ce2cd097` | BSD-2-Clause, AOMedia Patent License 1.0 | N |
| libaribcaption | `c64c23b890` | MIT | N |
| libass | `f61db567e6` | ISC | N |
| libffi | `bc55386736` | MIT | N |
| libjxl, with Highway and skcms | `b87738951c` | BSD-3-Clause (Highway Apache-2.0 or BSD-3-Clause) | N |
| libogg | `06a5e0262c` | BSD-3-Clause | N |
| libopenmpt | `f83cedb0cd` | BSD-3-Clause | N |
| librist | `4f45ef8f78` | BSD-2-Clause | N |
| libtheora | `28fd5ec77f` | BSD-3-Clause | N |
| libva | `6b07f71005` | MIT | N |
| libvmaf | `86da14d030` | BSD-2-Clause-Patent | N |
| libvorbis | `1b75110b5a` | BSD-3-Clause | N |
| libvpx | `5e680f3080` | BSD-3-Clause, with a patent grant | N |
| libwebp | `5c168cd23b` | BSD-3-Clause, with a patent grant | N |
| libxml2 | `91586dc674` | MIT | N |
| lilv | `4b8f30055f` | ISC | N |
| Little CMS | `a0b0d7a69b` | MIT | N |
| LV2 | `3c57dae600` | ISC | N |
| Mbed TLS (librist) | `v4.2.0` | Apache-2.0 or GPL-2.0-or-later | N |
| mingw-std-threads | `c931bac289` | BSD-2-Clause | N |
| mingw-w64 runtime and winpthreads | `57b5950390` | ZPL-2.1, MIT, BSD and public-domain parts | N |
| nv-codec-headers | `eddcea9e27` | MIT | N |
| OpenAPV | `a58ce739be` | BSD-3-Clause | N |
| OpenCL-Headers and OpenCL-ICD-Loader | `e6060189f4` | Apache-2.0 | N |
| opencore-amr | `7dba8c3223` | Apache-2.0 | N |
| OpenH264 | `8b2d28faad` | BSD-2-Clause | N |
| OpenJPEG | `8314119b06` | BSD-2-Clause | N |
| OpenSSL | `openssl-3.6.4` | Apache-2.0 | N |
| Opus | `503d81b138` | BSD-3-Clause | N |
| PCRE2 | `09eb19dc11` | BSD-3-Clause with the PCRE2 exception | N |
| pixman | `96c04d1b87` | MIT | N |
| rav1e, with its Rust crates and the Rust standard library | `31435de9d7` | BSD-2-Clause (crates mostly MIT or Apache-2.0) | N |
| serd | `b6053bb2a4` | ISC | N |
| shaderc, with glslang, SPIRV-Tools and SPIRV-Headers at the revisions in its DEPS file | `a8abeb0b8a` | Apache-2.0 (shaderc, SPIRV-Tools); glslang BSD-3-Clause/MIT/Apache-2.0 with a Bison-exception parser | N |
| Snappy | `9c28114a38` | BSD-3-Clause | N |
| sord | `4b5232bb91` | ISC | N |
| SPIRV-Cross (libplacebo's Direct3D 11 backend) | `aa217aeb6c` | Apache-2.0 | N |
| SPIRV-Headers | `2b7475f6d6` | MIT-style (Khronos) | N |
| sratom | `9e8ee84eed` | ISC | N |
| SVT-AV1 | `5ed9ffbcb5` | BSD-3-Clause-Clear, AOMedia Patent License 1.0 (upstream; file not retrievable at the commit) | N |
| uavs3d | `0e20d2c291` | BSD-3-Clause | N |
| Vulkan-Headers | `v1.4.363` | Apache-2.0 OR MIT | N |
| Vulkan-Shim-Loader (BtbN) | `65b3936528` | MIT | N |
| VVenC | `b1cfb2ad49` | BSD-3-Clause-Clear | N |
| zix | `17d632393f` | ISC | N |
| GCC runtime (libgcc, libstdc++, libgomp) | GCC 16.2.0 | GPL-3.0-or-later with the GCC Runtime Library Exception | - |
| liblzma (XZ Utils) | `3b1efb04d1` | 0BSD | - |
| libpng | `964b413594` | libpng-2.0 | - |
| libunibreak | `28a2756b86` | Zlib | - |
| zimg | `67e0603271` | WTFPL | - |
| zlib | `767c4c9478` | Zlib | - |
| libpsl, nghttp2, nghttp3, ngtcp2 | | MIT; dependencies of libcurl, which BtbN enables only for FFmpeg after 9.0 | x |
| libsamplerate (SDL2 dependency) | `0844c208f6` | BSD-2-Clause | x |
| OpenAL Soft (avdevice only) | `c89b8cf7bb` | LGPL-2.0-or-later | x |
| SDL2 (avdevice and ffplay only) | `b90ac95029` | Zlib | x |

Schannel, Media Foundation, Direct3D and the other Windows APIs FFmpeg uses are part of Windows.

## Corresponding source and notices

The release workflow attached two archives: FFmpeg's source at commit `330caae0c1`, and BtbN's
build scripts at the tag, which include BtbN's patches to aribb24 and libaom. That covers FFmpeg's
own code. It does not cover:

- the source of the LGPL and MPL libraries in the DLLs (the **S** rows), which the LGPL and the MPL
  require to be offered with the binaries. The scripts name them only by repository and commit, and
  the release cannot rely on those repositories staying available;
- the license texts and notices of the libraries under BSD, MIT, ISC, Apache-2.0, FTL and ZPL
  licenses (the **N** rows), which require them to accompany binary distributions. Neither the BtbN
  archive nor the installer contained them; the installer had only FFmpeg's LGPL text and the GPL-3.0
  text.

## Microsoft components

### Windows App SDK

Syna references the Runtime 2.5.1, Foundation 2.3.12, InteractiveExperiences 2.1.9 and WinUI 2.3.9
packages (and, through them, Base 2.0.x) and publishes self-contained. The `license.txt` in all of
these packages is the same file, "Microsoft Software License Terms, Microsoft Windows App SDK"
(SHA-256 `5b11e634…`). The relevant sections:

- **3(a)(i)**: files that the SDK's NuGet packages place next to an application are distributable
  code, in framework-dependent and in self-contained deployments. Syna's installer contains exactly
  those files.
- **3(b)**: the distributor must (i) add significant primary functionality, (ii) require
  distributors and external end users to agree to terms that protect the code and Microsoft at least
  as much as the license, and (iii) indemnify Microsoft against claims related to the application,
  except claims based only on the unmodified distributable code.
- **3(c)**: (i) no use of Microsoft's trademarks that suggests the application comes from or is
  endorsed by Microsoft; (ii) the source code of distributable code must not be modified or
  distributed so that it becomes subject to a license that requires disclosure of source code.
- **4(c)** and **4(e)**: notices of Microsoft and its suppliers must not be removed, and nothing but
  the distributable code may be redistributed.

How Syna meets them: Syna is the application (3(b)(i)). The installer's license page shows these
terms, and installing requires accepting them (3(b)(ii)). Syna's name and icon do not use
Microsoft's marks (3(c)(i)). The indemnity in 3(b)(iii) is an obligation the publisher takes on by
distributing; it needs no action in the build. 3(c)(ii) concerns the SDK's source code, which Syna
does not have. Under this audit, `package.ps1` now also installs the Runtime package's `NOTICE.txt`,
Microsoft's third-party notices for the SDK, as `WindowsAppSDK-NOTICE.txt` (4(c)). The WinUI
package's `NOTICE.txt` is a byte-for-byte prefix of it.

### Windows SDK projection

`Microsoft.Windows.SDK.NET.dll` and `WinRT.Runtime.dll` come from `Microsoft.Windows.SDK.NET.Ref`
10.0.26100.57. Its license is the Windows SDK license (https://aka.ms/WinSDKLicenseURL). Section
2(a)(i) makes the files on Microsoft's redistribution list distributable. That list (Microsoft
Learn, "Windows SDK redist lists", updated 2024-10-21) names both files and allows them to be
distributed unmodified as part of a program. Section 2(a)(ii) asks, among other things, that end users
agree to terms that protect the code at least as much as the license, that the program display a
valid copyright notice, and that the distributor indemnify Microsoft. The installer's license page
now names these terms by address for the two files; it did not mention them before.

### Others

- Microsoft Edge WebView2 SDK 1.0.3719.77: a BSD-3-Clause-style license that asks for the notice in
  documentation of binary distributions: installed as `WebView2-LICENSE.txt`. Its `NOTICE.txt` is
  now installed too.
- .NET runtime 10.0.12 and Microsoft.Data.Sqlite: MIT, with the runtime's third-party notices:
  installed. SQLitePCLRaw: Apache-2.0, text installed. SQLite: public domain.

## Uncertainties

1. **File-level licenses.** The inventory gives each project's license. Files under other licenses
   were looked for only where a project is known to mix licenses: libzvbi, Game_Music_Emu,
   Chromaprint, XZ Utils, GMP, Mbed TLS, FreeType. The other projects were not read file by file.
2. **Rust crates** in librsvg and rav1e, and **shaderc's dependencies** (glslang, SPIRV-Tools,
   SPIRV-Headers at the revisions in shaderc's `DEPS` file), were not enumerated.
3. **libssh and SVT-AV1** are listed with their upstream licenses: their repositories refused
   automated downloads, so the license file at the pinned commit was not read.
4. **Pass-through terms.** Whether naming the Windows SDK terms by address on the installer's license
   page meets section 2(a)(ii) of that license, and whether accepting terms in the installer binds
   everyone who receives the files (3(b)(ii) of the Windows App SDK terms): the installer is the only
   point where Syna can ask for acceptance.
5. **Patents** are outside these licenses. H.264, HEVC, AAC and others are covered by patent pools
   in some countries; LCEVCdec states that it grants no patent license; Cisco's patent coverage for
   OpenH264 applies only to binaries Cisco distributes.

## Way to a releasable build

Replace the FFmpeg DLLs with a build that contains neither FFTW nor libzvbi, and ship the license
texts and sources that the remaining libraries require.

- **A custom build limited to what Syna uses (recommended).** Syna needs FFmpeg's own demuxers,
  decoders, muxers and parsers, the `atempo` filter, swresample and swscale, D3D11VA, and these
  external libraries: dav1d (AV1 software decoding), Opus and LAME (Opus and MP3 audio export),
  OpenH264, Kvazaar, SVT-AV1 and libvpx (software video encoders), nv-codec-headers, AMF and libvpl
  (hardware encoders), zlib, plus Media Foundation from Windows. Of these, only LAME is copyleft, so
  the corresponding source would be FFmpeg's, LAME's and the build script; about a dozen license
  texts would ship. Without MP3 export, no copyleft library would remain. The DLLs would also shrink
  from about 155 MB to a fraction of that. The cost is maintaining the build, for example with BtbN's
  scripts and a custom variant, or a cross-compile in CI, and testing playback and export again.
- **BtbN's build without Chromaprint and libzvbi.** This keeps the other 77 libraries, so the
  release would have to carry the source of the 18 remaining LGPL and MPL libraries and about 55
  license texts, and each library's files would need the closer check that uncertainty 1 describes.

Either way, the archive should be kept in Syna's own repository (for example as an asset of a
dedicated release) and pinned there. BtbN deletes automatic builds once they are neither among the 14
newest nor the last of their month (`util/prunetags.sh`): `autobuild-2026-10-01-13-06` will soon
disappear, and `fetch-deps.ps1` will then fail on a fresh clone and in CI without a cache.

The new build is ready when `scripts/check-ffmpeg-license.ps1 -SourceDir artifacts\source` passes:
it lists each enabled library with its license text in `licenses\ffmpeg\` and, where required, its
source archive, `docs/ffmpeg-buildconf.txt` matches the DLLs, and
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) describes the new build.

## How the release is guarded

`scripts/check-ffmpeg-license.ps1` reads the FFmpeg DLLs (without loading them) and fails when:

- a DLL reports a GPL or nonfree license, or the configuration contains `--enable-gpl` or
  `--enable-nonfree`;
- a DLL contains FFTW, or the configuration enables libzvbi;
- the DLLs disagree about their configuration, or it differs from `docs/ffmpeg-buildconf.txt`;
- the package was not built by the current `scripts/ffmpeg/build.sh`, or its `BUILDINFO.txt` does
  not record the build's check that no GPL-licensed source was compiled;
- an enabled option is not listed in the script, or a license text it needs is missing from the
  package;
- with `-SourceDir`: a source archive the build used is missing or changed, or the recipe or a
  listed library's archive is missing.

It sees only what the configuration names, which is why the build passes `--disable-autodetect`.
The release workflow runs it before packaging and stops when it fails; CI runs it too and fails.

On the BtbN DLLs it reported FFTW in `avformat-62.dll`, `--enable-libzvbi`, and 61 enabled libraries
without license texts and sources. On the new build it passes; copies with a license text, a source
archive, the recipe or the recorded GPL check taken away or changed fail it.
