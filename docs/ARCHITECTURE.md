# Architecture

Syna is three layers with one direction of dependency:

```
Syna.App   (WinUI 3, C#)      windows, views, view models, shell integration
        │  P/Invoke, LibraryImport only
Syna.Core  (.NET 10, C#)      typed wrapper of the C ABI, settings, SQLite state
        │  C ABI: opaque handles, plain structs, UTF-8 JSON for rich data
Syna.Native (C++20 DLL)       FFmpeg, D3D11, WASAPI: playback, probing, waveforms, trims
```

The UI never sees FFmpeg, Direct3D or WASAPI types. Nothing crosses the boundary per frame or per
audio sample: the UI sends commands, polls one state struct per UI tick, and drains an event queue.
There are no native-to-managed callbacks, so no callback can outlive its owner or run on a thread
the UI does not expect.

## Native engine (`src/Syna.Native`)

`include/syna_api.h` is the whole contract. Every export is wrapped in a guard that converts C++
exceptions into `syna_result` codes plus a thread-local message (`syna_last_error`).

| Area | Entry points | Threads |
|---|---|---|
| Engine | `syna_initialize`, `syna_query_system_json`, `syna_sample_process_stats` | caller |
| Inspection | `syna_probe_json` (fast/full), `syna_make_thumbnail` | caller (blocking; the app calls them from workers) |
| Playback | `syna_player_*` | control, render, demux, video decode, audio decode, audio output |
| Jobs | `syna_waveform_start`, `syna_trim_start`, `syna_job_*` | one worker per job, background priority |

A player owns a D3D11 device, a composition swap chain attached to the app's `SwapChainPanel`,
and a WASAPI output. Each open file gets a `Pipeline` (demuxer, decoders, queues, mixer); only the
active session has one. Details: [MEDIA_ENGINE.md](MEDIA_ENGINE.md). Trimming: [TRIM.md](TRIM.md).

## Core (`src/Syna.Core`)

- `Engine/Native.cs`: `LibraryImport` declarations (source-generated marshalling, AOT compatible).
- `Engine/EnginePlayer.cs`, `EngineJobs.cs`, `MediaEngine.cs`: `IDisposable` wrappers, typed
  records for the JSON payloads (`System.Text.Json` source generation), `EngineException` with the
  native message.
- `State/AppDatabase.cs`: SQLite (WAL, `user_version` migrations) for recents, favorites, last
  positions, per-file mixer state and track labels, watch folders, the media metadata cache and the
  session list.
- `State/AppSettings.cs`: `settings.json`, written atomically (temp file, then replace).
- `State/MixerState.cs`: a clip's mixer (per track gain, mute, solo, offset), saved per file and
  copied into a trim request; `EffectiveGain` is the player's mute/solo rule.
- `Engine/TrimPlanner.cs`: turns the trim sheet's mode, output profile, audio arrangement and Custom
  settings into the engine request, and describes for the clip at hand what will be copied and what
  re-encoded, with which encoder. It follows the engine's rules, so the sheet never promises a copy
  that will not happen, and it blocks combinations a container cannot hold.
- `Services/Notifications.cs`: `NotificationCenter`, the lifecycle of transient notifications
  (shown → dismissing → removed). Each notification holds its current timer until it fires or is
  replaced, every step runs on the UI thread, and a timer that fires after the notification moved on
  is ignored, so a timeout and a click resolve to one dismissal. Tested with a manual clock.

## App (`src/Syna.App`)

- `Program.cs`: the entry point for every launch (Start menu, Explorer, *Open with*, scripts), and
  single instance.
  - A second launch redirects its activation to the running instance through `AppInstance` and
    exits. This covers another clip and several clips opened together.
  - Redirections that arrive while the first instance is still starting are kept until its window
    exists.
  - Before WinUI loads, it writes the startup log line and checks that the installation is
    complete. When the app cannot start, `Services/StartupFailure.cs` shows a message and logs the
    details, so the window never silently fails to appear.
  - Switches that never open a window: `--register-file-types` / `--unregister-file-types` serve
    the per-user installer, and `--self-check` serves the packaging and install scripts.
- `MainWindow`: custom title bar, keyboard routing, drag and drop, fullscreen, theme, and the
  toast views (text, optional action, close button) that `NotificationCenter` adds, fades and removes.
  Implements `IShell` so view models can ask for pickers, dialogs and toasts without referencing
  views.
- `ViewModels/MainViewModel*.cs`: sessions, transport, mixer state, commands (also feeding the
  command palette), persistence timers. `TrimViewModel`, `ExplorerViewModel`, `SessionViewModel`.
- `Services/PlaybackController.cs`: owns the `EnginePlayer`, recreates it after device loss.
- `Services/BackgroundServices.cs`: a low-priority worker for thumbnails and metadata, and the
  waveform service (one engine job at a time, results cached on disk).
- `Services/ShellIntegration.cs`: "Open with" registration (run by the installer and from
  Settings) and the Default apps deep link.
- `Views/*`: explorer, session rail, editor strip (timeline, audio lanes, transport), trim sheet,
  command palette, settings, media info, dialogs. Custom controls in `Controls/` draw the timeline
  and waveforms directly (no per-sample XAML elements).

### UI update loop

While playing, `CompositionTarget.Rendering` drives `MainViewModel.Tick`: one
`syna_player_get_state` call per UI frame updates position, meters and diagnostics, then events
are drained. When paused, an 80 ms timer does the same at low cost. Video presentation does not
depend on the UI thread: the engine's render thread presents to the swap chain directly.

### Sessions

Each open clip is a `SessionViewModel` (path, position, IN/OUT, mixer state, waveform, thumbnail).
Switching sessions saves the outgoing position and mixer state, closes the engine pipeline and
opens the next file at its saved position; there is one engine player for the whole window. The
player's mixer settings are therefore shared and re-applied on every switch, so a trim never reads
them: it takes a copy of its own clip's `SessionViewModel.Mixer` when Save is pressed, keyed by
stream index (`TrimTrackMix`), and the engine job keeps its own copy of that.
The session list is persisted and optionally restored at startup (missing files stay listed and
marked).

*Close all clips* (rail header, rail context menu, command palette, Ctrl+Shift+W; one confirmation
for more than one clip) closes every session in one step: the active position is saved, the player
closes its pipeline (demux and decode threads end, the audio output is detached onto its null sink;
the D3D device and the output object stay for the next clip), waveform work is cancelled, sessions are marked closed so their queued thumbnail and duration work is
skipped and late results are dropped, and the rail is cleared with a single collection reset rather
than one removal per clip. Files, stars, history and caches are untouched.

## Data on disk

| What | Where |
|---|---|
| Settings | `%LOCALAPPDATA%\Syna\settings.json` |
| State database (history, mixer state, sessions, watch folders, clip metadata) | `%LOCALAPPDATA%\Syna\state.db` |
| Thumbnails, waveforms | `%LOCALAPPDATA%\Syna\Cache\` (keyed by path + size + modification time) |
| Logs | `%LOCALAPPDATA%\Syna\Logs\` (`app-*.log`, `native-*.log`, last 10 kept; `SYNA_VERBOSE=1` adds the engine's verbose output) |

The environment variable `SYNA_DATA_DIR` puts all of this in another folder: a separate profile for
tests and screenshots that leaves the user's data alone (and skips the import below).

On the first run, when `%LOCALAPPDATA%\Syna` does not exist yet, settings and history (recents,
positions, mixer state, sessions, metadata cache) are imported from `%LOCALAPPDATA%\HyakClipPlayer`,
the folder the app used under its previous name (`Core/State/LegacyImport.cs`). That folder is never
written: a cleanly closed database is copied as a file, and one with a write-ahead log (the previous
version still running) is read through SQLite's backup API, which includes the log. Thumbnails and
waveforms are rebuilt on demand.

Source media is never written. Trim output goes to a `.partial` file next to the destination and is
moved into place only after validation. "Delete" in the explorer moves files to the Recycle Bin.

## Activation and file associations

The app is offered for .mp4 .m4v .mov .mkv .webm through
`ActivationRegistrationManager.RegisterForFileTypeActivation`, called by the installer
(`Syna.exe --register-file-types`) or from Settings. That only adds the app to "Open with" and to
Settings > Default apps. Choosing the default player is left to Windows: the app's "Open Default
apps" button deep-links to `ms-settings:defaultapps` (on the app's own page when Windows supports
it). No registry writes try to change user choices. Details, including how activations reach the
single running instance: [PACKAGING.md](PACKAGING.md#file-types-and-activation).
