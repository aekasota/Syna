# Packaging and installation

Syna ships as one Windows Installer package per release:

```powershell
.\scripts\package.ps1 -Configuration Release            # version from src\Syna.App\Syna.App.csproj
.\scripts\package.ps1 -Configuration Release -Version 1.2.0
```

| Output in `artifacts\` | What it is |
|---|---|
| `Syna-Setup-<version>-x64.msi` | The installer |
| `Syna-Setup-<version>-x64.msi.sha256` | Its SHA-256, in `sha256sum` format |
| `app\` | The self-contained application the installer contains |

## What the installer does

| | |
|---|---|
| Scope | Per user, no administrator rights |
| Location | `%LOCALAPPDATA%\Programs\Syna` |
| Start menu | A `Syna` shortcut |
| File types | `.mp4 .m4v .mov .mkv .webm` get Syna in "Open with", and Syna is listed in Settings > Default apps |
| Apps & features | `Syna`, publisher `hyak`, with version and install location |
| License page | Syna's MIT License, a summary of third-party licenses, and Microsoft's terms for the Windows App SDK files |
| Finish page | Optionally starts Syna |

It does not:

- change the default app for any file type (Windows reserves that choice for the user);
- read or write `%LOCALAPPDATA%\Syna` (settings, history, caches, logs) or any video;
- install prerequisites, because there are none: the .NET runtime, the Windows App SDK, the engine
  (built with the static C runtime) and FFmpeg are all inside the program folder.

## Why an MSI

| | MSI (chosen) | MSIX |
|---|---|---|
| Unsigned | Installs after a SmartScreen warning | Does not install unless the user imports and trusts the signing certificate |
| Administrator rights | None for the per-user install | None |
| Upgrade, uninstall, Apps & features | Built in | Built in, with a cleaner uninstall |
| File associations | Registered by the app (see below) | Declared in the manifest |

MSIX would be the better format with a trusted code-signing certificate. Syna is an unsigned open-source
project for now, and an MSIX that cannot be installed without changing certificate trust is not a
reasonable download. The decision should be revisited once releases are signed.

The installer is built with the [WiX Toolset](https://wixtoolset.org/) v7 (SDK-style project
`installer\Syna.Installer.wixproj`, restored from NuGet). WiX v7 is distributed under the Open Source
Maintenance Fee EULA, which a project must accept to build with it. Syna's author accepted it
(`<AcceptEula>wix7</AcceptEula>` in the project). The fee applies only to organizations with more than
US$10,000 in annual revenue; see <https://docs.firegiant.com/wix/osmf/> for the current terms.
Anyone who builds the installer from a fork makes that decision for themselves.

## Install scope

The package is dual-purpose (`Scope="perUserOrMachine"`, which sets `ALLUSERS=2` and
`MSIINSTALLPERUSER=1`) and installs per user by default. For a per-user install Windows Installer
places `ProgramFiles64Folder` in `%LOCALAPPDATA%\Programs`, so no elevation is needed.

Only the per-user install is supported. A per-machine install (`ALLUSERS=1`, elevated) is possible
from the command line but untested, and the file type registration would cover only the user who
ran it.

## Upgrades

Every build has a new product code and the same upgrade code
(`{4B38677F-67FF-49EE-9D91-215972F7BB22}`, which must never change). Installing any version removes
the installed one first, then installs the new files, so there is always one Apps & features entry and
one shortcut:

- a newer or equal version upgrades (equal versions are allowed so a rebuilt installer can replace
  the installed build);
- an older version is refused with "A newer version of Syna is already installed."

The program folder stays the same across upgrades, and so does everything derived from the path of
`Syna.exe`: the file type registration and the user's default-app choice.

## Uninstall

Settings > Apps > Installed apps > Syna > Uninstall. From a script, `msiexec /x` with the installed
build's MSI (or its product code) does the same.

This removes the program folder, the shortcut, the Apps & features entry, the "Open with" and
Default apps registration, and `HKCU\Software\hyak\Syna` (the shortcut's key). Settings, history,
caches and logs in `%LOCALAPPDATA%\Syna` are kept for a later reinstall; delete that folder to remove
them. Videos are never touched.

## File types and activation

The installer does not write file type keys itself. After copying the files it runs
`Syna.exe --register-file-types`, and before removing them `Syna.exe --unregister-file-types`. The app
registers through the Windows App SDK (`ActivationRegistrationManager`), the same code path as
Settings > System in the app. That registration:

- uses the SDK's activation command, so a file opened from Explorer arrives as a `File` activation
  that the running instance can receive;
- produces a ProgID (`App.<hash>.File`) derived from the full path of `Syna.exe`, which is how a
  default-app choice made for an earlier build keeps working after an upgrade;
- describes the app for Settings > Default apps (name, description, icon).

Both switches read the registration back and print it. The installer runs them through `WixQuietExec`,
so the result appears in the installer log, for example:

```
WixQuietExec:  File types that open with C:\Users\<user>\AppData\Local\Programs\Syna\Syna.exe: .mp4 .m4v .mov .mkv .webm
```

A failure there is logged but does not stop the installation or removal.

| Launch | What Windows runs | Activation kind |
|---|---|---|
| Start menu shortcut | `Syna.exe`, working folder = program folder | `Launch` |
| Double-click in Explorer (Syna is the default) or Open with | `Syna.exe "----ms-protocol:ms-encodedlaunch:App?ContractId=Windows.File&Verb=open&File=%1"` | `File` |

The first process registers the single-instance key and opens the window. A later process redirects
its activation to that instance and exits: another clip, the Start menu entry while Syna runs, or
several clips opened together (Explorer starts one process per file). The app subscribes to
redirections before it registers the key and keeps those that arrive while it is still starting.

Each window process logs one line at startup (`start exe=... cwd=... activation=File files=1`) and one
per redirected activation, in `%LOCALAPPDATA%\Syna\Logs`.

## Startup checks

The installed app carries WinUI itself, so WinUI's own resources must be merged into `Syna.pri`
(about 2.2 MB; the app's resources alone are about 70 KB). That merge comes from the MSIX tooling in
the build (`EnableMsixTooling`), which is why the project keeps it although it is not packaged as
MSIX. `Syna.exe --self-check` opens no window and exits with 0 when the folder can start, 2 when
WinUI's resources are missing, and 3 when the engine or an FFmpeg library cannot be loaded. If either
fails at a normal start, the app shows a message and logs the details instead of disappearing.

The development build (`build.ps1`, `run.ps1`) uses the Windows App Runtime installed on the machine
instead, and `package.ps1` publishes into its own folders so it never reuses the development build's
`Syna.pri`.

## What `package.ps1` checks

1. The published app contains what it needs to start (`Syna.exe`, `Syna.pri`, WinUI, the engine,
   the six FFmpeg DLLs, the icon, the licenses), and `Syna.exe --self-check` passes.
2. The license texts of the NuGet packages it redistributes are copied into `licenses\`, at the
   restored versions.
3. The MSI has the expected product name, version, manufacturer, upgrade code and install scope, as
   many files as the app folder, a shortcut and the custom actions.
4. The MSI is unpacked as an administrative image (`msiexec /a`, no registration), the file count is
   compared again, and the unpacked `Syna.exe --self-check` passes.

WiX's validation runs on every build. Five checks are suppressed in the project file, each with the
reason: language columns of WinUI's resource DLLs that ICE03 does not know, versioned files without a
language, the shortcut in a dual-purpose package, same-version upgrades, and a shortcut that targets a
file in another component.

## Signing

Signing is optional and configured only through the environment, never through files in the
repository:

| Variable | Meaning |
|---|---|
| `SYNA_SIGN_CERT_FILE` | Path to a code-signing certificate (`.pfx`) |
| `SYNA_SIGN_CERT_PASSWORD` | Its password |
| `SYNA_SIGN_TIMESTAMP_URL` | RFC 3161 timestamp server (default `http://timestamp.digicert.com`) |

With them set, `package.ps1` signs `Syna.exe` before it is packed and then the MSI (SHA-256, with a
timestamp, using `signtool` from the Windows SDK build tools package the app already restores). The
release workflow takes the certificate from the repository secrets `SYNA_SIGN_CERT_BASE64` and
`SYNA_SIGN_CERT_PASSWORD`, writes it to a temporary file for the build and deletes it afterwards.
`*.pfx` is ignored by Git.

Without a certificate the installer and `Syna.exe` are unsigned, and the build says so. In practice:

- Microsoft Defender SmartScreen shows "Windows protected your PC" for the downloaded MSI; the
  user has to choose *More info* > *Run anyway*.
- Windows shows the publisher as unknown. The "hyak" shown in Apps & features comes from the MSI
  and is not verified by anything.
- Policies that allow only signed software (App Control for Business, AppLocker publisher rules)
  will block it, and some antivirus products treat unsigned executables with more suspicion.
- The `.sha256` file shows that a download is the file the release published. It does not say who
  built it.

A signature identifies the publisher and makes tampering detectable. SmartScreen reputation is
earned over time even for signed files.

## Moving from the script install

Development builds before the installer were installed by `scripts\install.ps1` into the same
folder. To switch to the installer, close Syna, delete `%LOCALAPPDATA%\Programs\Syna` and the Start
menu shortcut `Syna`, then run the MSI. It installs into the same folder and registers the same
ProgID, so the default-app choice and the data in `%LOCALAPPDATA%\Syna` carry over. Files left in
the folder would otherwise stay behind: Windows Installer does not replace files of equal version
that it did not install, and does not remove them on uninstall.

## Upgrading from Hyak Clip Player

Syna was called Hyak Clip Player before 3 October 2026. On its first start with no
`%LOCALAPPDATA%\Syna` folder, Syna imports the settings and history from
`%LOCALAPPDATA%\HyakClipPlayer` and leaves that folder unchanged; delete it once it is no longer
needed.

## Size

The installed app is about 220 MB and the MSI about 67 MB. The FFmpeg DLLs take 44 MiB (`avcodec`
32 MiB): Syna's build keeps FFmpeg's own codecs but only the external libraries Syna uses
([FFMPEG.md](FFMPEG.md)); BtbN's build with every library was 154 MiB. The Windows App SDK is
referenced by component (Foundation, InteractiveExperiences, WinUI, Runtime) rather than the
metapackage, which keeps about 70 MB of AI, search and widget libraries out of the installation.

## Tested

On Windows 11 Pro 26200 (x64), 13 October 2026, with `msiexec /qn` and verbose logs:

| Check | Result |
|---|---|
| Install without elevation | Per-user ("user unmanaged" context): the program folder, the shortcut and the Apps & features entry (473 files in the final 1.0.0 build) |
| File types | Registered for all five types (installer log); the existing `.mp4` default choice kept |
| Start menu entry | Starts Syna (`activation=Launch`, working folder = program folder) |
| Explorer double-click, Open with, two files opened together, while Syna runs | All arrive in the one running instance, no second process |
| Explorer double-click with Syna closed | Starts with the file (`activation=File`) |
| Folder and file names with spaces, accented Latin letters, Japanese characters and an en dash | Opened, and stored in the session list exactly |
| Upgrade 1.0.0 to 1.0.1, and a rebuilt 1.0.0 over 1.0.0 | Old version removed first; one entry, one shortcut, default choice kept |
| Downgrade 1.0.1 to 1.0.0 | Refused with exit code 1603 and the message above |
| Uninstall | Files, shortcut, entry and `HKCU\Software\hyak` removed; registration removed (installer log: `none`) |
| Reinstall | Registration restored, default choice still in place |
| `test-install.ps1` with the final installer | All nine checks passed (install, shortcut, one product, file types, `--self-check`; removal of folder, shortcut, product and file types) |

Repeated the same day with the LGPL FFmpeg build ([FFMPEG.md](FFMPEG.md); 494 files):
`test-install.ps1` passed all nine checks; installing over the earlier 1.0.0 upgraded it in place,
registered the five file types and kept the default-app choice; every installed file was identical to
`artifacts\app`; the installed app, started with a separate profile (`SYNA_DATA_DIR`), played H.264
with two audio tracks, AV1, and VP9 with Opus through D3D11VA, and its encoder probe found NVENC
(H.264, HEVC, AV1), Media Foundation, OpenH264, Kvazaar, SVT-AV1 and libvpx. AMF and Quick Sync
could not be tried: the test machine has an NVIDIA GPU only.

Not tested: the installer's dialogs on a clean machine, a per-machine install, Windows 10, and a
signed build.
