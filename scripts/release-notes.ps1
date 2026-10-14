# Writes the release notes for a version from the files in artifacts\ (the installer, its .sha256,
# and FFmpeg's corresponding source in artifacts\source) and the Git history since the previous tag.
# Used by .github/workflows/release.yml; runs locally the same way.
param(
    [Parameter(Mandatory)][string]$Version,
    [string]$Commit = 'HEAD',
    [string]$Out
)
. "$PSScriptRoot\common.ps1"

$artifacts = Join-Path $RepoRoot 'artifacts'
$msiName = "Syna-Setup-$Version-x64.msi"
$msi = Join-Path $artifacts $msiName
if (-not (Test-Path "$msi.sha256")) { throw "$msiName.sha256 not found; run scripts\package.ps1 -Version $Version first." }
$hash = (Get-Content "$msi.sha256" -Raw).Split(' ')[0].Trim()
$signed = (Get-AuthenticodeSignature $msi).Status -eq 'Valid'

$sha = (git -C $RepoRoot rev-parse $Commit).Trim()
if ($LASTEXITCODE -ne 0) { throw "Unknown commit $Commit." }
# git reports "no tag found" on stderr, which Windows PowerShell turns into an error under 'Stop'.
$ErrorActionPreference = 'Continue'
$previous = git -C $RepoRoot describe --tags --abbrev=0 --match 'v*.*.*' "$sha^" 2>$null
$ErrorActionPreference = 'Stop'
$range = if ($LASTEXITCODE -eq 0 -and $previous) { "$($previous.Trim())..$sha" } else { $sha }
$changes = @(git -C $RepoRoot log --no-merges --format='- %s (%h)' $range | Select-Object -First 100)

$lines = @(
    "Syna $Version for Windows 11, x64.",
    '',
    "**Download:** ``$msiName``. It installs for the current user into ``%LOCALAPPDATA%\Programs\Syna``, needs no administrator rights and no other downloads, and upgrades an earlier version in place. Settings and history are kept.",
    '',
    "SHA-256: ``$hash``",
    ''
)
if ($signed) {
    $lines += 'The installer and `Syna.exe` are code-signed.'
} else {
    $lines += 'The installer is not code-signed. Windows SmartScreen may show "Windows protected your PC"; after checking the SHA-256 above, choose *More info* > *Run anyway*.'
}
$lines += '', '## Changes', ''
$lines += $(if ($previous) { $changes } else { @('First release.') })
$lines += '', '## Source', '', "Built from commit ``$sha`` by GitHub Actions."

$sources = Get-ChildItem (Join-Path $artifacts 'source') -Filter *.tar.* -ErrorAction SilentlyContinue | Sort-Object Name
if ($sources) {
    $lines += '', 'Syna includes FFmpeg as DLLs under the LGPL, with the libraries listed in `docs/FFMPEG.md` built in. Their corresponding source is attached: the source archive of FFmpeg and of each library, unchanged, and the recipe (`syna-ffmpeg-recipe-*.tar.gz`) that built the DLLs from them. `ffmpeg-sources.sha256` lists their SHA-256:', ''
    foreach ($s in $sources) { $lines += "- ``$($s.Name)``" }
    $lines += '', 'Third-party licenses: `docs/THIRD-PARTY-NOTICES.md` in the repository and the `licenses` folder of the installation.'
}

$text = ($lines -join "`n") + "`n"
if ($Out) { [IO.File]::WriteAllText($Out, $text, (New-Object Text.UTF8Encoding $false)) } else { $text }
