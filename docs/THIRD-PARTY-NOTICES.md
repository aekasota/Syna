# Third-party notices

Syna's own source code is licensed under the MIT License ([LICENSE](../LICENSE)). The installed
application also contains the third-party components below. Each remains under its own license; the
MIT License does not apply to them. The license texts are installed in the `licenses` folder next to
`Syna.exe`.

This page records what is redistributed and what the licenses ask of a distributor, as understood
from the license texts. It is not legal advice. [FFMPEG.md](FFMPEG.md) describes the FFmpeg build
in detail, and [LICENSE-AUDIT.md](LICENSE-AUDIT.md) the audit behind both pages.

| Component | Version | License | Files in the installation | License text |
|---|---|---|---|---|
| FFmpeg, with the libraries built into its DLLs | 8.1.3, commit `330caae0c1` | FFmpeg: LGPL-2.1-or-later. Built in: LAME (LGPL-2.0-or-later), dav1d, Opus, OpenH264, Kvazaar, SVT-AV1, libvpx, libvpl, NVIDIA and AMD headers, zlib, and the compiler's runtime (BSD, MIT, Zlib, ZPL) | `avcodec-62.dll`, `avformat-62.dll`, `avutil-60.dll`, `avfilter-11.dll`, `swresample-6.dll`, `swscale-9.dll` | `ffmpeg\` (one or more per library) |
| .NET runtime | 10.0.12 | MIT, with its own third-party notices | `coreclr.dll`, `hostfxr.dll`, `System.*.dll` and the rest of the runtime | `dotnet-LICENSE.txt`, `dotnet-THIRD-PARTY-NOTICES.txt` |
| Windows App SDK: Runtime 2.5.1, Foundation 2.3.12, WinUI 2.3.9, InteractiveExperiences 2.1.9 | as listed | Microsoft Software License Terms, Microsoft Windows App SDK (not open source; redistributable as described below) | `Microsoft.UI.Xaml.dll`, `Microsoft.WindowsAppRuntime.dll`, `Microsoft.UI.*.dll`, `CoreMessagingXP.dll`, `dcompi.dll` and the other files the SDK places next to the app | `WindowsAppSDK-LICENSE.txt`, `WindowsAppSDK-NOTICE.txt` |
| Microsoft Edge WebView2 SDK (a dependency of WinUI; Syna does not use WebView2) | 1.0.3719.77 | BSD-style license, Copyright (C) Microsoft Corporation | `Microsoft.Web.WebView2.Core.dll`, `Microsoft.Web.WebView2.Core.Projection.dll`, `WebView2Loader.dll` | `WebView2-LICENSE.txt`, `WebView2-NOTICE.txt` |
| Windows SDK projection for .NET | 10.0.26100.57 (package `Microsoft.Windows.SDK.NET.Ref`) | Windows SDK license terms, https://aka.ms/WinSDKLicenseURL; both files are on Microsoft's redistribution list | `Microsoft.Windows.SDK.NET.dll`, `WinRT.Runtime.dll` | at that address |
| Microsoft.Data.Sqlite | 10.0.12 | MIT, Copyright (c) .NET Foundation and Contributors | `Microsoft.Data.Sqlite.dll` | `dotnet-LICENSE.txt` (same MIT text) |
| SQLitePCLRaw | 2.1.12 | Apache-2.0 | `SQLitePCLRaw.core.dll`, `SQLitePCLRaw.batteries_v2.dll`, `SQLitePCLRaw.provider.e_sqlite3.dll` | `Apache-2.0.txt` |
| SQLite | 3.53.3 (built into `e_sqlite3.dll` by SQLitePCLRaw 2.1.12) | Public domain, https://sqlite.org/copyright.html | `e_sqlite3.dll` | none required |

## FFmpeg

| | |
|---|---|
| Version | FFmpeg 8.1.3, commit `330caae0c1acccd2222edc52a05940c574561ce5` of the `release/8.1` branch; the DLLs report `8.1.3-g330caae0c1-syna` |
| Build | Syna's own, by [`scripts/ffmpeg/build.sh`](../scripts/ffmpeg/build.sh) from pinned source archives ([FFMPEG.md](FFMPEG.md)) |
| License | LGPL version 2.1 or later: no `--enable-gpl`, `--enable-nonfree` or `--enable-version3`, and no GPL-licensed source compiled in (the build checks every object) |
| Linking | dynamic; six DLLs next to `Syna.exe`, loaded by name, so they can be replaced with compatible builds of the same major versions |
| Configuration | [ffmpeg-buildconf.txt](ffmpeg-buildconf.txt) (`ffmpeg -buildconf`), also shown in Settings > About |

### Libraries built into the FFmpeg DLLs

| Library | Version | License | Texts in `licenses\ffmpeg\` |
|---|---|---|---|
| LAME | 3.100 | LGPL-2.0-or-later | `LAME-COPYING.txt`, `LAME-LICENSE.txt` |
| dav1d | 1.5.4 | BSD-2-Clause; AOMedia Patent License 1.0 | `dav1d-COPYING.txt`, `dav1d-PATENTS.txt` |
| Opus | 1.6.1 | BSD-3-Clause | `opus-COPYING.txt` |
| OpenH264 | 2.6.0 | BSD-2-Clause | `OpenH264-LICENSE.txt` |
| Kvazaar | 2.3.2 | BSD-3-Clause | `Kvazaar-LICENSE.txt` |
| SVT-AV1 | 4.2.0 | BSD-3-Clause-Clear; AOMedia Patent License 1.0 | `SVT-AV1-LICENSE.md`, `SVT-AV1-PATENTS.md` |
| libvpx | 1.17.0 | BSD-3-Clause; Google's patent grant | `libvpx-LICENSE.txt`, `libvpx-PATENTS.txt`, `libvpx-AUTHORS.txt` |
| Intel libvpl | 2.17.0 | MIT | `libvpl-LICENSE.txt` |
| nv-codec-headers | 13.0.19.1 | MIT | `nv-codec-headers-LICENSE.txt` |
| AMD AMF headers | 1.5.3 | MIT | `AMF-LICENSE.txt` |
| zlib | 1.3.2 | Zlib | `zlib-LICENSE.txt` |
| mingw-w64 runtime, winpthreads (llvm-mingw 20261006) | 15.0 development snapshot | ZPL-2.1 and others; MIT and BSD-3-Clause | `mingw-w64-runtime-COPYING.txt`, `winpthreads-COPYING.txt` |
| LLVM libc++, libunwind, compiler-rt (llvm-mingw 20261006) | 23.1.3 | Apache-2.0 with the LLVM exception | `LLVM-runtime-LICENSE.txt` |

FFmpeg's own texts are `FFmpeg-COPYING.LGPLv2.1.txt` and `FFmpeg-LICENSE.md`. Syna uses the
libraries directly: dav1d decodes AV1 in software; OpenH264, Kvazaar, SVT-AV1 and libvpx encode
H.264, HEVC, AV1 and VP9 when no hardware encoder is available; Opus and LAME encode the audio of
exports; libvpl, the NVIDIA and the AMD headers give access to the GPU encoders.

### Corresponding source

Every release attaches the source archive of FFmpeg and of each library above, unchanged, the
recipe that built the DLLs from them (`syna-ffmpeg-recipe-<recipe>.tar.gz`, the
`scripts/ffmpeg/` folder of that build) and `ffmpeg-sources.sha256`. `scripts/ffmpeg/build.sh` lists
each archive with the URL it comes from and its SHA-256.

### Codec patents

The licenses above are copyright licenses. H.264, HEVC and AAC are covered by patent pools in some
jurisdictions, and no patent license comes with FFmpeg's, OpenH264's or Kvazaar's code. Cisco pays the
H.264 royalties only for OpenH264 binaries it distributes itself, not for OpenH264 built from source
as here. dav1d and SVT-AV1 come with the AOMedia Patent License for AV1, libvpx with Google's grant
for VP8 and VP9. Syna prefers the GPU's hardware decoders and encoders; the software codecs are
fallbacks. Anyone distributing Syna commercially should review codec licensing for their markets
([FFMPEG.md](FFMPEG.md#patents)).

## Windows App SDK

The Windows App SDK files are redistributed under section 3 ("Distributable code") of Microsoft's
license: files the SDK's NuGet packages place next to an application may be redistributed with it,
in self-contained deployments too (3(a)(i)). The license requires the distributor to add significant
primary functionality to them, to have distributors and end users agree to terms that protect them
and Microsoft at least as much as the license does, and to indemnify Microsoft as the license
describes (3(b)); not to use Microsoft's trademarks in a way that suggests endorsement (3(c)); and
not to remove the notices of Microsoft or its suppliers (4(c)). The installer therefore presents
Microsoft's terms for these files on its license page, together with Syna's MIT License, and installs
Microsoft's third-party notices for the SDK as `WindowsAppSDK-NOTICE.txt`.

`Microsoft.Windows.SDK.NET.dll` and `WinRT.Runtime.dll` are redistributable under section 2(a) of
the Windows SDK license, which has similar requirements; the installer's license page names those
terms for these two files.

## What a distributor of Syna's installer has to do

1. Keep the `licenses` folder and this notice in the installation (the build does this).
2. Offer the corresponding source of the FFmpeg DLLs next to the installer: the archives a release
   attaches. `scripts/check-ffmpeg-license.ps1` checks that they are complete and unchanged, and that
   the DLLs have no GPL code and every license text they need.
3. Present Microsoft's terms for the Windows App SDK and the Windows SDK projection to end users (the
   installer's license page does this).

## Development tools (not redistributed)

Visual Studio, CMake, Ninja, the .NET SDK, the WiX Toolset (which builds the installer), xUnit, the
FFmpeg build tools (llvm-mingw's compiler, NASM, Meson and the Ubuntu environment they run in), and
the FFmpeg command-line build used by `scripts/make-fixtures.ps1` to create test media are used only
to build and test Syna. None of them is part of the installation, apart from the runtime parts of
llvm-mingw listed above.
