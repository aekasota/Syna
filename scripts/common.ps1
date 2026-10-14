# Shared helpers for the build/test/run/package scripts.
$ErrorActionPreference = 'Stop'

$script:RepoRoot = Split-Path -Parent $PSScriptRoot
$script:BuildRoot = Join-Path $RepoRoot 'build'

function Get-VsInstallPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found; install Visual Studio 2026 with the C++ desktop workload.' }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $path) { throw 'No Visual Studio installation with the MSVC x64 toolset was found.' }
    return $path
}

# Imports the x64 developer environment (cl, link, cmake, ninja, fxc) into this PowerShell session.
function Enter-DevEnvironment {
    if ($env:SYNA_DEVENV -eq '1') { return }
    $vs = Get-VsInstallPath
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $env:PATH = (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer') + ";$env:PATH"
    $lines = cmd /c "`"$vcvars`" >nul 2>nul && set"
    foreach ($line in $lines) {
        $i = $line.IndexOf('=')
        if ($i -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0, $i), $line.Substring($i + 1)) }
    }
    $cmakeDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
    $env:PATH = "$cmakeDir\CMake\bin;$cmakeDir\Ninja;$env:PATH"
    $env:SYNA_DEVENV = '1'
}

function Invoke-Checked([string]$what, [scriptblock]$block) {
    & $block
    if ($LASTEXITCODE -ne 0) { throw "$what failed with exit code $LASTEXITCODE" }
}

function Get-AppOutputDir([string]$Configuration) {
    return Join-Path $RepoRoot "src\Syna.App\bin\x64\$Configuration\net10.0-windows10.0.26100.0\win-x64"
}
