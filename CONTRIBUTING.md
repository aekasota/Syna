# Contributing to Syna

Thanks for taking the time. Syna is developed by one person, so this page is mostly about making
reports and changes easy to act on.

## Reporting a bug

Open an issue with:

- what you did, what you expected and what happened instead;
- Syna's version (Settings > About), your Windows version and your GPU;
- for problems with a particular clip, its media info (Ctrl+I, then copy) and, if you can share it, a
  short clip that shows the problem;
- the logs from `%LOCALAPPDATA%\Syna\Logs` (`app-*.log` and `native-*.log` of the session where it
  happened).

Logs and media info contain file paths. Remove anything you do not want to publish before attaching
them.

Security problems are not reported as issues; see [SECURITY.md](SECURITY.md).

## Suggesting a feature

Open an issue that describes the problem first, not only the solution. Syna aims to stay a focused
tool for reviewing and cutting recordings, so some good ideas will not fit.

## Changing the code

[docs/BUILDING.md](docs/BUILDING.md) explains the toolchain and the scripts.
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) is the place to start reading.

- Open an issue before larger changes, so the approach can be agreed on before you spend time on it.
- Keep pull requests focused on one change, and say what it does and why.
- Run `.\scripts\test.ps1` before submitting. The full suite needs a Direct3D 11 GPU and an FFmpeg
  command-line tool for the test media; CI runs the part that needs neither. Engine and planner
  changes come with tests.
- Follow the style of the surrounding code. Comments explain why, not what. Project text is in
  English (US).
- New dependencies need a reason, and their license has to allow redistribution under Syna's terms.
  FFmpeg stays an LGPL build: no `--enable-gpl` or `--enable-nonfree` components, and no GPL code
  through the libraries built into it either. A library added to `scripts/ffmpeg/build.sh` needs its
  license text and an entry in `scripts/check-ffmpeg-license.ps1` ([FFMPEG.md](docs/FFMPEG.md)).
- For interface changes, attach before and after screenshots. Use synthetic media
  ([`scripts/capture-readme-screenshots.ps1`](scripts/capture-readme-screenshots.ps1) makes some),
  never someone else's recordings or personal files.

## License

By contributing, you agree that your contribution is released under the [MIT License](LICENSE), like
the rest of Syna's source code.
