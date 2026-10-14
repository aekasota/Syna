# Builds the native engine (CMake + Ninja) and the WinUI app (dotnet).
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$NativeOnly,
    [switch]$Clean
)
. "$PSScriptRoot\common.ps1"

& "$PSScriptRoot\fetch-deps.ps1"
Enter-DevEnvironment

$nativeDir = Join-Path $BuildRoot "native-$Configuration"
if ($Clean) { Remove-Item -Recurse -Force $nativeDir -ErrorAction SilentlyContinue }
Invoke-Checked 'cmake configure' { cmake -S $RepoRoot -B $nativeDir -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration" | Out-Host }
Invoke-Checked 'native build' { cmake --build $nativeDir | Out-Host }

if (-not $NativeOnly) {
    $sln = Join-Path $RepoRoot 'Syna.slnx'
    $cleanArg = if ($Clean) { '--no-incremental' } else { $null }
    Invoke-Checked 'dotnet build' { dotnet build $sln -c $Configuration -p:Platform=x64 $cleanArg -nologo | Out-Host }
}
Write-Host "Build complete ($Configuration)."
