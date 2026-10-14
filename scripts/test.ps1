# Builds (unless -NoBuild), generates missing fixtures, then runs the native engine tests and the
# managed Core tests. -RealClip additionally runs the opt-in tests against a real recording (read-only;
# outputs go to %TEMP%). -Filter limits native tests to names containing any of the given texts
# (-Filter mixer_,sample_fifo).
[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$NoBuild,
    [string]$RealClip,
    [string[]]$Filter = @()
)
. "$PSScriptRoot\common.ps1"

if (-not $NoBuild) { & "$PSScriptRoot\build.ps1" -Configuration $Configuration }

# Fixtures need an FFmpeg CLI with libx264/NVENC (see make-fixtures.ps1); tests skip missing ones.
try { & "$PSScriptRoot\make-fixtures.ps1" }
catch { Write-Warning "Fixture generation skipped: $($_.Exception.Message)" }

$failed = @()
$native = Join-Path $BuildRoot "native-$Configuration\bin\syna_native_tests.exe"
if ($RealClip) { $env:SYNA_REAL_CLIP = (Resolve-Path $RealClip).Path }
try {
    # Encoder libraries print banners to stderr while being probed; only the exit code matters.
    $ErrorActionPreference = 'Continue'
    $filters = @($Filter | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    & $native @filters 2>$null | Out-Host
    if ($LASTEXITCODE -ne 0) { $failed += 'native' }
}
finally {
    $ErrorActionPreference = 'Stop'
    Remove-Item Env:\SYNA_REAL_CLIP -ErrorAction SilentlyContinue
}

$coreTests = Join-Path $RepoRoot 'tests\Syna.Core.Tests\Syna.Core.Tests.csproj'
dotnet test --project $coreTests -c $Configuration -p:Platform=x64 --no-build | Out-Host
if ($LASTEXITCODE -ne 0) { $failed += 'core' }

if ($failed.Count) { throw "Test failures: $($failed -join ', ')" }
Write-Host 'All tests passed.'
