# Prepares third_party\ffmpeg from the FFmpeg package that scripts\ffmpeg\build.sh builds from pinned
# sources (docs/FFMPEG.md). The package of the current recipe has to be in third_party\ffmpeg-build\:
# scripts\build-ffmpeg.ps1 builds it on this machine (in WSL), and CI builds it in its ffmpeg job.
# -SourceTo <folder> instead copies that build's corresponding source (every source archive, the
# recipe and their SHA-256 list) to <folder>, which releases attach.
param(
    [switch]$Force,
    [string]$SourceTo
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'third_party\ffmpeg-build'
$dest = Join-Path $root 'third_party\ffmpeg'
$recipe = (Get-FileHash (Join-Path $PSScriptRoot 'ffmpeg\build.sh') -Algorithm SHA256).Hash.Substring(0, 12).ToLowerInvariant()
$name = "ffmpeg-syna-$recipe-win64.zip"
$package = Join-Path $build $name

if (-not (Test-Path $package)) {
    throw "The FFmpeg package of the current recipe ($name) is not in $build. Build it with scripts\build-ffmpeg.ps1 (needs WSL), or put the ffmpeg-build artifact of a CI run of this commit there (docs/FFMPEG.md)."
}

if ($SourceTo) {
    $source = Join-Path $build 'source'
    $sums = Join-Path $source 'ffmpeg-sources.sha256'
    if (-not (Test-Path $sums)) { throw "$sums not found: the package was not built by scripts\ffmpeg\build.sh." }
    New-Item -ItemType Directory -Force $SourceTo | Out-Null
    foreach ($line in Get-Content $sums) {
        if ($line -match '^[0-9a-f]{64}\s+\*?(.+)$') { Copy-Item (Join-Path $source $Matches[1]) $SourceTo }
    }
    Copy-Item $sums $SourceTo
    Write-Host "Corresponding source of $name copied to $SourceTo"
    return
}

$marker = Join-Path $dest '.package'
$current = if (Test-Path $marker) { (Get-Content $marker -Raw).Trim() } else { '' }
if ($Force -or $current -ne $name) {
    Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
    Expand-Archive -Path $package -DestinationPath $dest
    Set-Content -Path $marker -Value $name -NoNewline
    Write-Host "FFmpeg ready in $dest ($name)"
} else {
    Write-Host "FFmpeg up to date ($name)"
}
