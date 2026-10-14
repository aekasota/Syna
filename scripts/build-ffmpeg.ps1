# Builds the FFmpeg package (scripts\ffmpeg\build.sh, docs/FFMPEG.md) on this machine, in a WSL
# distribution of its own, into third_party\ffmpeg-build\, and then prepares third_party\ffmpeg from
# it. The first run creates that distribution from Ubuntu's 24.04 WSL image (pinned below, about
# 360 MB) in %USERPROFILE%\.wsl\<name>; every run installs the build tools there
# (scripts\ffmpeg\setup-ubuntu.sh). Other WSL distributions are not touched. To remove it:
# wsl --unregister syna-build
param([string]$Distro = 'syna-build')
. "$PSScriptRoot\common.ps1"

$image = @{
    Url    = 'https://cloud-images.ubuntu.com/wsl/releases/24.04/20240423/ubuntu-noble-wsl-amd64-wsl.rootfs.tar.gz'
    Sha256 = '8251E27FFFF381A4AF5F41DCB94D867DE3E0D9774A9241908AB34555D99315EA'
}

function ConvertTo-WslPath([string]$path) { '/mnt/' + $path.Substring(0, 1).ToLowerInvariant() + $path.Substring(2).Replace('\', '/') }
function Invoke-Wsl([string]$what, [string[]]$arguments) {
    & wsl.exe -d $Distro @arguments
    if ($LASTEXITCODE -ne 0) { throw "$what failed with exit code $LASTEXITCODE" }
}

$distros = @(wsl.exe --list --quiet) -replace "`0", '' | ForEach-Object Trim | Where-Object { $_ }
if ($distros -notcontains $Distro) {
    $downloads = Join-Path $RepoRoot 'third_party\_downloads'
    New-Item -ItemType Directory -Force $downloads | Out-Null
    $rootfs = Join-Path $downloads (Split-Path $image.Url -Leaf)
    if (-not (Test-Path $rootfs) -or (Get-FileHash $rootfs -Algorithm SHA256).Hash -ne $image.Sha256) {
        Write-Host "Downloading $(Split-Path $image.Url -Leaf)"
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $image.Url -OutFile $rootfs -UseBasicParsing
        if ((Get-FileHash $rootfs -Algorithm SHA256).Hash -ne $image.Sha256) { throw "SHA-256 mismatch for $rootfs" }
    }
    Invoke-Checked "wsl --import $Distro" { wsl.exe --import $Distro (Join-Path $env:USERPROFILE ".wsl\$Distro") $rootfs --version 2 }
    # Builds see only the distribution's own tools, not the Windows PATH.
    Invoke-Wsl 'writing /etc/wsl.conf' @('-u', 'root', '--', 'sh', '-c', 'printf "[interop]\nappendWindowsPath=false\n" > /etc/wsl.conf')
    wsl.exe --terminate $Distro | Out-Null
}

Invoke-Wsl 'setup-ubuntu.sh' @('-u', 'root', '--', 'bash', (ConvertTo-WslPath (Join-Path $PSScriptRoot 'ffmpeg\setup-ubuntu.sh')))
Invoke-Wsl 'build.sh' @('--', 'bash', (ConvertTo-WslPath (Join-Path $PSScriptRoot 'ffmpeg\build.sh')), (ConvertTo-WslPath (Join-Path $RepoRoot 'third_party\ffmpeg-build')))
& "$PSScriptRoot\fetch-deps.ps1" -Force
