# Building from source

## Requirements

| | Version used for development | Notes |
|---|---|---|
| Windows | 11 x64 | The build is Windows-only |
| Visual Studio 2026 | Community 18.10 | Workload *Desktop development with C++*: the MSVC v145 x64 tools, *C++ CMake tools for Windows* (CMake and Ninja) and a Windows SDK (for the `fxc` shader compiler) |
| .NET SDK | 10.0.401 | Any .NET 10 SDK |
| Windows PowerShell | 5.1 | The scripts are written for the PowerShell that comes with Windows |
| WSL 2 | | Only to build the FFmpeg DLLs (`build-ffmpeg.ps1`); see [FFmpeg](#ffmpeg) |
| Network access | | NuGet packages and, for the first FFmpeg build, its sources, compiler and Ubuntu image (about 560 MB) |

The scripts find Visual Studio through `vswhere` and load its x64 developer environment themselves,
so they run from any PowerShell window, not only a developer prompt.

## Quick start

From the repository root:

```powershell
.\scripts\build.ps1 -Configuration Release     # engine and app
.\scripts\test.ps1                             # tests (builds first unless -NoBuild)
.\scripts\run.ps1 -NoBuild                     # start the development build
.\scripts\package.ps1 -Configuration Release   # installer in artifacts\
```

The development build started by `run.ps1` uses the Windows App Runtime installed on the machine
(version 2.5). The installer contains everything instead; see [PACKAGING.md](PACKAGING.md).

## Scripts

Every script works with paths relative to the repository, stops at the first error and then exits
with a non-zero code.

| Script | What it does | Output |
|---|---|---|
| `build.ps1 [-Configuration Release\|Debug] [-NativeOnly] [-Clean]` | Fetches FFmpeg, builds the engine with CMake and Ninja, then the .NET solution | `build\native-<configuration>\bin\`, `src\Syna.App\bin\x64\<configuration>\...` |
| `test.ps1 [-NoBuild] [-Filter <texts>] [-RealClip <file>]` | Builds, generates missing test media, runs the native tests, then the managed tests | Test results on the console |
| `run.ps1 [-NoBuild] [<files>]` | Starts the development build, optionally with files | |
| `package.ps1 [-Configuration Release] [-Version <x.y.z>]` | Publishes the self-contained app, builds and checks the installer, writes its SHA-256 | `artifacts\Syna-Setup-<version>-x64.msi`, `.msi.sha256`, `artifacts\app\` |
| `test-install.ps1 -Msi <file> [-Force]` | Installs the installer silently, checks the installation, uninstalls it and checks the removal. Refuses to run where Syna is installed unless `-Force` | Logs in `artifacts\install-test\` |
| `release-notes.ps1 -Version <x.y.z> [-Commit <ref>] [-Out <file>]` | Writes the release notes from the built installer and the Git history (used by the release workflow) | Markdown |
| `build-ffmpeg.ps1` | Builds the FFmpeg DLLs from their pinned sources in a WSL distribution of its own (`scripts\ffmpeg\build.sh`, [FFMPEG.md](FFMPEG.md)), then runs `fetch-deps.ps1` | `third_party\ffmpeg-build\` (package and sources), `third_party\ffmpeg\` |
| `fetch-deps.ps1 [-Force] [-SourceTo <folder>]` | Unpacks the FFmpeg package of the current recipe into `third_party\ffmpeg\` (run by `build.ps1`); `-SourceTo` copies its corresponding source | `third_party\ffmpeg\` |
| `check-ffmpeg-license.ps1 [-Dir <folder>] [-SourceDir <folder>]` | Checks that the FFmpeg DLLs may be redistributed: LGPL, no GPL code, built by the current recipe, every license text and source archive present ([RELEASING.md](RELEASING.md#licenses)) | Problems on the console, exit code 1 |
| `make-fixtures.ps1 [-Force]` | Generates synthetic test media with an FFmpeg command-line tool (run by `test.ps1`) | `tests\fixtures\generated\` |
| `benchmark.ps1 [-Clips <files>]` | Measures open, seek, playback and trim times | `artifacts\benchmarks\<timestamp>\` |
| `make-icons.ps1` | Renders the app icon and the README icon from the app's mark | `src\Syna.App\Assets\AppIcon.ico`, `docs\assets\syna-icon.png` |
| `capture-readme-screenshots.ps1 [-NoBuild] [-Regenerate] [-Drive S:]` | Generates synthetic clips, runs the development build in a separate profile on a temporary drive letter, and captures the README screenshots. Needs an FFmpeg command-line tool and the desktop; nothing is played | `docs\assets\screenshots\` |

`build\`, `artifacts\`, `third_party\`, `tests\fixtures\generated\`, `bin\` and `obj\` are build
output and ignored by Git.

## Tests

| Suite | Count | Needs |
|---|---|---|
| Native engine (`syna_native_tests.exe`) | 80 | Most tests use generated media and a Direct3D 11 GPU with video decoding; 4 also need a real recording |
| Managed (`tests\Syna.Core.Tests`, xUnit v3) | 44 | Nothing beyond the build; one test uses generated media |

`make-fixtures.ps1` needs an FFmpeg command-line tool with libx264 or NVENC, on `PATH` or in
`SYNA_FIXTURE_FFMPEG`. The FFmpeg build the app uses has no such encoders (it leaves out libx264), so
use a separate full build; it is a test tool only and never part of Syna. Tests whose media is
missing are reported as `SKIP`, not as failures.

The 4 tests that need a real recording run only when one is given. The file is opened read-only, and
everything the tests write goes to `%TEMP%`:

```powershell
.\scripts\test.ps1 -NoBuild -RealClip "D:\Videos\recording.mp4"
```

`-Filter` runs only the native tests whose names contain one of the given texts, for example
`-Filter trim_` or `-Filter mixer_,sample_fifo`.

The GitHub Actions runners have neither a GPU nor an FFmpeg command-line tool, so CI runs the managed
tests and the native tests that need neither (`-Filter mixer_,sample_fifo`; the filter also selects
four trim tests, which report `SKIP` there for lack of media), then builds the installer and runs
`test-install.ps1` on it. The full suite runs locally;
[TEST_MATRIX.md](TEST_MATRIX.md) lists what each test covers and what is checked by hand.

## Continuous integration and releases

[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) runs on every push to `main` and every pull
request: build, tests, installer, install test. The installer is kept as a workflow artifact for 14
days. Tagged versions are built by [`.github/workflows/release.yml`](../.github/workflows/release.yml);
see [RELEASING.md](RELEASING.md).

## FFmpeg

The FFmpeg DLLs are Syna's own build of FFmpeg 8.1.3, made by `scripts\ffmpeg\build.sh` from source
archives pinned by URL and SHA-256 ([FFMPEG.md](FFMPEG.md) has the configuration, the libraries and
their licenses). It runs on Linux; on Windows, `build-ffmpeg.ps1` runs it in a WSL distribution of
its own, `syna-build`, which it creates on first use from Ubuntu's 24.04 image (pinned by SHA-256).
Other WSL distributions are not touched; `wsl --unregister syna-build` removes it again.

```powershell
.\scripts\build-ffmpeg.ps1
```

The result is `third_party\ffmpeg-build\ffmpeg-syna-<recipe>-win64.zip` with the source archives in
`third_party\ffmpeg-build\source\`, and `fetch-deps.ps1` unpacks the package into
`third_party\ffmpeg\`. `<recipe>` comes from the SHA-256 of `build.sh`, so after any change to it
`build.ps1` stops until FFmpeg is rebuilt. Instead of building, the `ffmpeg-build` artifact of a CI
run of the same commit can be unpacked into `third_party\ffmpeg-build\`.

## Debug builds

`-Configuration Debug` works for `build.ps1`, `test.ps1` and `run.ps1`. The engine is then built
without optimizations and with the debug C runtime, and the managed tests load that Debug engine.
Installers are meant to contain Release builds.
