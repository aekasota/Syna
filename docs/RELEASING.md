# Releasing

Releases are built by GitHub Actions from a version tag. The workflow creates a **draft** release;
nothing is public until the maintainer publishes it.

## Steps

1. Set the version in `src\Syna.App\Syna.App.csproj` (`<Version>1.2.0</Version>`). Versions are
   `MAJOR.MINOR.PATCH`, at most `255.255.65535` (a Windows Installer limit).
2. Run the full test suite locally. CI cannot: its runners have no GPU and no test media.

   ```powershell
   .\scripts\test.ps1
   .\scripts\test.ps1 -NoBuild -RealClip "<a real recording>"
   ```

3. Optionally build and try the installer locally: `.\scripts\package.ps1 -Configuration Release`.
4. Commit, then tag that commit and push the tag:

   ```powershell
   git tag v1.2.0
   git push origin v1.2.0
   ```

5. The [release workflow](../.github/workflows/release.yml) then:
   - builds the FFmpeg DLLs from their pinned sources, afresh (`scripts/ffmpeg/build.sh` in a pinned
     Ubuntu container, [FFMPEG.md](FFMPEG.md));
   - checks that the tag is `vMAJOR.MINOR.PATCH` and equals the project version, and stops otherwise;
   - builds the engine and the app with those DLLs, and runs the tests that work on the runners;
   - collects FFmpeg's corresponding source, the archives that build used;
   - checks that the FFmpeg DLLs may be redistributed and that their sources are complete and
     unchanged (`check-ffmpeg-license.ps1`, see [Licenses](#licenses)), and stops otherwise;
   - builds the installer (`package.ps1`, with its own checks), signs it if signing is configured,
     and verifies the checksum and, when signing is configured, the signature;
   - installs the installer on the runner, checks the installation, uninstalls it and checks the
     removal (`test-install.ps1`);
   - writes the release notes (`release-notes.ps1`): download, SHA-256, signing status, the commits
     since the previous tag, the commit it was built from, and the source archives;
   - creates or updates a draft release for the tag with `Syna-Setup-<version>-x64.msi`, its
     `.sha256` and the source archives.
6. Review the draft on the repository's Releases page: notes, file names, sizes. Then publish it.

A failed run can be re-run. If the draft already exists, its files and notes are replaced. A
published release is never changed by the workflow; to fix one, publish a new version.

## Permissions

The build job runs with read-only repository access. Only the job that creates the draft has
`contents: write`, through the workflow's `GITHUB_TOKEN`. No other token is used.

## Code signing

Releases are unsigned unless the repository has these secrets (Settings > Secrets and variables >
Actions):

| Secret | Value |
|---|---|
| `SYNA_SIGN_CERT_BASE64` | The code-signing certificate (`.pfx`), base64-encoded: `[Convert]::ToBase64String([IO.File]::ReadAllBytes('cert.pfx'))` |
| `SYNA_SIGN_CERT_PASSWORD` | Its password |

The certificate is written to the runner's temporary folder only for the packaging step and deleted
afterwards. Never commit a certificate; `*.pfx` is ignored by Git. What unsigned releases mean for
users is described in [PACKAGING.md](PACKAGING.md#signing).

## Licenses

The installer contains FFmpeg's DLLs, which have to be an LGPL build: no GPL or nonfree code,
including code that comes in through the libraries built into them. The release workflow builds
them from the source archives pinned in `scripts/ffmpeg/build.sh` and attaches exactly those
archives, the recipe that built the DLLs from them and their SHA-256 list ([FFMPEG.md](FFMPEG.md)).

`scripts\check-ffmpeg-license.ps1` decides whether the DLLs may be shipped; the release workflow
stops when it fails, and so does CI. Besides the license strings, the configuration and known GPL
code, it checks that the DLLs come from the current recipe, whose build found no GPL-licensed source
among the files it compiled; that every option the configuration enables is listed in the script,
with the license texts it needs in the package (installed with the app as `licenses\ffmpeg\`); and,
with `-SourceDir`, that every source archive the build used is there and unchanged.

When the FFmpeg build changes, follow [FFMPEG.md](FFMPEG.md#updating-ffmpeg-or-a-library): the pins
in `build.sh`, [ffmpeg-buildconf.txt](ffmpeg-buildconf.txt), the options listed in
`check-ffmpeg-license.ps1`, [FFMPEG.md](FFMPEG.md) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)
change together.

By distributing the installer, the publisher also takes on the obligations of Microsoft's terms for
the Windows App SDK and the Windows SDK projection, including indemnifying Microsoft as they describe
([LICENSE-AUDIT.md](LICENSE-AUDIT.md#microsoft-components)).
