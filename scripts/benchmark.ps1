# Runs the engine benchmark (syna_bench) over the synthetic fixtures plus any real clips given with
# -Clips, and writes artifacts\benchmarks\<timestamp>\ with one JSON report per file and summary.md.
# Source files are only read; trim outputs go to %TEMP%\syna-bench and are deleted.
param(
    [string[]]$Clips = @(),
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [double]$PlaySeconds = 8,
    [switch]$NoTrim,
    [switch]$NoBuild
)
. "$PSScriptRoot\common.ps1"

if (-not $NoBuild) { & "$PSScriptRoot\build.ps1" -Configuration $Configuration -NativeOnly }
$bench = Join-Path $BuildRoot "native-$Configuration\bin\syna_bench.exe"
if (-not (Test-Path $bench)) { throw "syna_bench.exe not found; run scripts\build.ps1 first." }

$fixtureNames = 'A_h264_1080p60_2trk.mp4', 'B_hevc_1080p60_2trk.mp4', 'C_h264_720p30_1trk.mp4', 'H_h264_longgop_bframes.mp4',
    'I_h264_vfr.mp4', 'J_hevc_main10.mp4', 'L_av1_1080p60_2trk.mp4', 'M_h264_2trk.mkv', 'N_vp9_opus.webm'
$files = @()
foreach ($name in $fixtureNames) {
    $path = Join-Path $RepoRoot "tests\fixtures\generated\$name"
    if (Test-Path $path) { $files += $path } else { Write-Warning "Missing fixture $name (run scripts\make-fixtures.ps1)." }
}
foreach ($clip in $Clips) { $files += (Resolve-Path $clip).Path }

$outDir = Join-Path $RepoRoot ("artifacts\benchmarks\" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $outDir | Out-Null

function Ms($v) { if ($null -eq $v) { '' } else { '{0:N0}' -f [double]$v } }
function TrimCell($trims, [string]$mode) {
    $t = $trims | Where-Object { $_.mode -eq $mode } | Select-Object -First 1
    if (-not $t) { return '' }
    if ($t.error) { return 'error' }
    $how = if ($t.report.reencodedSeconds -gt 0) { $t.report.encoder } else { 'copy' }
    $ok = if ($t.report.validation.passed) { '' } else { ' (validation failed)' }
    return ('{0:N0} ms, {1}{2}' -f [double]$t.ms, $how, $ok)
}

$rows = foreach ($file in $files) {
    $name = Split-Path $file -Leaf
    Write-Host "Benchmarking $name"
    $json = Join-Path $outDir "$name.json"
    $benchArgs = @($file, '--out', $json, '--play-seconds', $PlaySeconds)
    if ($NoTrim) { $benchArgs += '--no-trim' }
    # Encoder libraries print banners to stderr while being probed; that is not a failure.
    $ErrorActionPreference = 'Continue'
    & $bench @benchArgs 2>$null | Out-Null
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0) { Write-Warning "$name reported errors (see $json)." }
    $r = Get-Content $json -Raw | ConvertFrom-Json
    [pscustomobject]@{
        File            = $name
        Decoder         = $r.decoder
        'Open ms'       = Ms $r.openToFirstFrameMs
        'Seek p50/p95'  = '{0}/{1}' -f (Ms $r.seekExact.medianMs), (Ms $r.seekExact.p95Ms)
        'Step back p95' = Ms $r.stepBackward.p95Ms
        'FPS'           = '{0:N1}' -f [double]$r.playback.presentedFps
        'Dropped'       = $r.playback.framesDropped
        'Drift max ms'  = '{0:N1}' -f [double]$r.playback.avDriftMaxMs
        'CPU %'         = '{0:N2}' -f [double]$r.playback.cpuPercentAllCores
        'GPU dec %'     = '{0:N1}' -f [double]$r.playback.gpuDecodePercent
        'Private MB'    = '{0:N0}' -f [double]$r.memory.privateAfterReopensMB
        'Quick'         = TrimCell $r.trims 'quick'
        'Smart'         = TrimCell $r.trims 'smart'
        'Exact'         = TrimCell $r.trims 'exact'
    }
}

$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
$gpu = Get-CimInstance Win32_VideoController | Where-Object { $_.AdapterRAM -gt 0 -or $_.Name -match 'NVIDIA|AMD|Radeon|Intel' } | Select-Object -First 1
$ram = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
$os = Get-CimInstance Win32_OperatingSystem

$md = New-Object System.Text.StringBuilder
[void]$md.AppendLine("# Engine benchmark $(Get-Date -Format 'yyyy-MM-dd HH:mm')")
[void]$md.AppendLine()
[void]$md.AppendLine("- CPU: $cpu, $ram GB RAM")
[void]$md.AppendLine("- GPU: $($gpu.Name), driver $($gpu.DriverVersion)")
[void]$md.AppendLine("- OS: $($os.Caption) build $($os.BuildNumber)")
[void]$md.AppendLine("- Build: $Configuration, playback window $PlaySeconds s, audio muted at the master (mixer and clock active)")
[void]$md.AppendLine()
$cols = $rows[0].PSObject.Properties.Name
[void]$md.AppendLine('| ' + ($cols -join ' | ') + ' |')
[void]$md.AppendLine('|' + (($cols | ForEach-Object { '---' }) -join '|') + '|')
foreach ($row in $rows) { [void]$md.AppendLine('| ' + (($cols | ForEach-Object { $row.$_ }) -join ' | ') + ' |') }
Set-Content -Path (Join-Path $outDir 'summary.md') -Value $md.ToString() -Encoding utf8

$rows | Format-Table -AutoSize | Out-String -Width 400 | Write-Host
Write-Host "Reports: $outDir"
