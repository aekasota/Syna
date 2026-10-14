# Generates synthetic test media in tests/fixtures/generated. Uses an FFmpeg CLI with NVENC and
# libx264/libx265 (set SYNA_FIXTURE_FFMPEG to choose one). The CLI is a test tool only; the app never runs it.
param([switch]$Force)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'tests\fixtures\generated'
New-Item -ItemType Directory -Force $out | Out-Null
$ffmpeg = if ($env:SYNA_FIXTURE_FFMPEG) { $env:SYNA_FIXTURE_FFMPEG } else { (Get-Command ffmpeg -ErrorAction Stop).Source }
$encoders = & $ffmpeg -hide_banner -encoders 2>$null | Out-String
$hasNvenc = $encoders -match 'h264_nvenc'
$hasX264 = $encoders -match 'libx264'

$font = 'C\:/Windows/Fonts/consola.ttf'
function Video([string]$size = '1920x1080', [int]$rate = 60) {
    "testsrc2=size=${size}:rate=${rate},drawtext=fontfile='$font':text='%{frame_num}':fontsize=110:fontcolor=white:box=1:boxcolor=black@0.7:boxborderw=12:x=48:y=48,setparams=range=tv:color_primaries=bt709:color_trc=bt709:colorspace=bt709"
}
$tone = 'sine=frequency=440:sample_rate=48000'
$beeps = "aevalsrc='0.4*sin(2*PI*1000*t)*lt(mod(t\,1)\,0.5)':s=48000"

function Make([string]$name, [string[]]$arguments) {
    $dest = Join-Path $out $name
    if ((Test-Path $dest) -and -not $Force) { Write-Host "exists  $name"; return }
    Write-Host "create  $name"
    & $ffmpeg -hide_banner -loglevel error -y @arguments $dest
    if ($LASTEXITCODE -ne 0) {
        Remove-Item $dest -ErrorAction SilentlyContinue
        throw "ffmpeg failed for $name"
    }
}

$h264 = if ($hasNvenc) { @('-c:v', 'h264_nvenc', '-profile:v', 'high', '-preset', 'p4', '-rc', 'vbr', '-b:v', '30M', '-bf', '0') }
        else { @('-c:v', 'libx264', '-profile:v', 'high', '-preset', 'veryfast', '-crf', '18', '-bf', '0') }
$bt709 = @('-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709', '-color_range', 'tv')
$aac2 = @('-c:a', 'aac', '-b:a', '192k', '-ac', '2')
$names2 = @('-metadata:s:a:0', 'title=System sounds', '-metadata:s:a:1', 'title=Microphone')

# A: ShadowPlay-like 1080p60 H.264, IDR every 0.5 s, two named AAC tracks.
Make 'A_h264_1080p60_2trk.mp4' (@('-f', 'lavfi', '-i', (Video), '-f', 'lavfi', '-i', $tone, '-f', 'lavfi', '-i', $beeps,
    '-t', '10', '-map', '0:v', '-map', '1:a', '-map', '2:a') + $h264 + @('-g', '30', '-pix_fmt', 'yuv420p') + $bt709 + $aac2 + $names2)

# B: HEVC with two tracks.
if ($hasNvenc) {
    Make 'B_hevc_1080p60_2trk.mp4' (@('-f', 'lavfi', '-i', (Video), '-f', 'lavfi', '-i', $tone, '-f', 'lavfi', '-i', $beeps,
        '-t', '8', '-map', '0:v', '-map', '1:a', '-map', '2:a', '-c:v', 'hevc_nvenc', '-preset', 'p4', '-b:v', '20M', '-bf', '0',
        '-g', '30', '-tag:v', 'hvc1', '-pix_fmt', 'yuv420p') + $bt709 + $aac2 + $names2)
}

# C: single audio track.
Make 'C_h264_720p30_1trk.mp4' (@('-f', 'lavfi', '-i', (Video '1280x720' 30), '-f', 'lavfi', '-i', $tone,
    '-t', '6', '-map', '0:v', '-map', '1:a') + $h264 + @('-g', '30', '-pix_fmt', 'yuv420p') + $aac2)

# D: four audio tracks.
Make 'D_h264_4trk.mp4' (@('-f', 'lavfi', '-i', (Video '1280x720' 60), '-f', 'lavfi', '-i', $tone, '-f', 'lavfi', '-i', $beeps,
    '-f', 'lavfi', '-i', 'sine=frequency=220:sample_rate=48000', '-f', 'lavfi', '-i', 'anoisesrc=color=pink:amplitude=0.1:sample_rate=48000',
    '-t', '6', '-map', '0:v', '-map', '1:a', '-map', '2:a', '-map', '3:a', '-map', '4:a') + $h264 + @('-g', '60', '-pix_fmt', 'yuv420p') + $aac2)

# E: tracks with different sample rates (48 kHz and 44.1 kHz).
Make 'E_h264_mixed_rates.mp4' (@('-f', 'lavfi', '-i', (Video '1280x720' 60), '-f', 'lavfi', '-i', $tone,
    '-f', 'lavfi', '-i', 'sine=frequency=1000:sample_rate=44100', '-t', '6', '-map', '0:v', '-map', '1:a', '-map', '2:a') + $h264 +
    @('-g', '60', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '160k', '-ac', '2', '-ar:a:0', '48000', '-ar:a:1', '44100'))

# F: second track starts 0.5 s late.
Make 'F_h264_offset_track.mp4' (@('-f', 'lavfi', '-i', (Video '1280x720' 60), '-f', 'lavfi', '-i', $tone,
    '-itsoffset', '0.5', '-f', 'lavfi', '-i', $beeps, '-t', '6', '-map', '0:v', '-map', '1:a', '-map', '2:a') + $h264 +
    @('-g', '60', '-pix_fmt', 'yuv420p') + $aac2)

# G: short clip, every frame a keyframe.
$intra = if ($hasX264) { @('-c:v', 'libx264', '-preset', 'veryfast', '-crf', '18', '-g', '1') } else { $h264 + @('-g', '2') }
Make 'G_h264_allintra_short.mp4' (@('-f', 'lavfi', '-i', (Video '1280x720' 60), '-f', 'lavfi', '-i', $tone,
    '-t', '2', '-map', '0:v', '-map', '1:a') + $intra + @('-pix_fmt', 'yuv420p') + $aac2)

# R: NVENC H.264 with B-frames that are also references: it decodes three frames ahead while the
# stream declares a reorder depth of two. IDR every 2 s.
if ($hasNvenc) {
    Make 'R_h264_nvenc_bref.mp4' @('-f', 'lavfi', '-i', (Video '1280x720' 60), '-t', '8', '-c:v', 'h264_nvenc', '-preset', 'p5',
        '-bf', '3', '-b_ref_mode', 'middle', '-g', '120', '-pix_fmt', 'yuv420p')
}

# H: long GOP (5 s) with B-frames, to stress trimming.
if ($hasX264) {
    Make 'H_h264_longgop_bframes.mp4' (@('-f', 'lavfi', '-i', (Video '1920x1080' 60), '-f', 'lavfi', '-i', $tone, '-f', 'lavfi', '-i', $beeps,
        '-t', '20', '-map', '0:v', '-map', '1:a', '-map', '2:a', '-c:v', 'libx264', '-preset', 'veryfast', '-crf', '20', '-bf', '3',
        '-g', '300', '-keyint_min', '300', '-sc_threshold', '0', '-pix_fmt', 'yuv420p') + $aac2 + $names2)
}

# I: variable frame timing (+-4 ms jitter around 60 fps), timestamps preserved.
Make 'I_h264_vfr.mp4' (@('-f', 'lavfi', '-i', (Video '1280x720' 60), '-f', 'lavfi', '-i', $tone, '-t', '6',
    '-map', '0:v', '-map', '1:a', '-vf', 'settb=1/90000,setpts=(N/60+0.004*sin(N))/TB', '-fps_mode', 'passthrough', '-enc_time_base:v', '1:90000', '-video_track_timescale', '90000') +
    $h264 + @('-g', '30', '-pix_fmt', 'yuv420p') + $aac2)

# J: 10-bit HEVC.
if ($hasNvenc) {
    Make 'J_hevc_main10.mp4' (@('-f', 'lavfi', '-i', (Video), '-f', 'lavfi', '-i', $tone, '-t', '5', '-map', '0:v', '-map', '1:a',
        '-c:v', 'hevc_nvenc', '-profile:v', 'main10', '-preset', 'p4', '-b:v', '20M', '-g', '30', '-bf', '0', '-tag:v', 'hvc1', '-pix_fmt', 'p010le') +
        $bt709 + $aac2)
    Make 'L_av1_1080p60_2trk.mp4' (@('-f', 'lavfi', '-i', (Video), '-f', 'lavfi', '-i', $tone, '-f', 'lavfi', '-i', $beeps,
        '-t', '5', '-map', '0:v', '-map', '1:a', '-map', '2:a', '-c:v', 'av1_nvenc', '-preset', 'p4', '-b:v', '15M', '-g', '30',
        '-pix_fmt', 'yuv420p') + $bt709 + $aac2 + $names2)
}

# Containers other than MP4.
Make 'M_h264_2trk.mkv' (@('-f', 'lavfi', '-i', (Video '1280x720' 60), '-f', 'lavfi', '-i', $tone, '-f', 'lavfi', '-i', $beeps,
    '-t', '6', '-map', '0:v', '-map', '1:a', '-map', '2:a') + $h264 + @('-g', '30', '-pix_fmt', 'yuv420p') + $aac2 + $names2)
# Q: Matroska with an AAC stereo tone and a mono Opus 1 kHz tone that ends a second earlier.
Make 'Q_h264_aac_opus_mono.mkv' (@('-f', 'lavfi', '-i', (Video '1280x720' 30), '-f', 'lavfi', '-i', $tone,
    '-f', 'lavfi', '-i', 'sine=frequency=1000:sample_rate=48000:duration=2', '-t', '3', '-map', '0:v', '-map', '1:a', '-map', '2:a') + $h264 +
    @('-g', '30', '-pix_fmt', 'yuv420p', '-c:a:0', 'aac', '-b:a:0', '192k', '-ac:a:0', '2', '-c:a:1', 'libopus', '-b:a:1', '64k', '-ac:a:1', '1'))
Make 'P_vp9_2trk.webm' (@('-f', 'lavfi', '-i', (Video '1280x720' 30), '-f', 'lavfi', '-i', $tone, '-f', 'lavfi', '-i', $beeps, '-t', '4',
    '-map', '0:v', '-map', '1:a', '-map', '2:a', '-c:v', 'libvpx-vp9', '-b:v', '4M', '-deadline', 'realtime', '-cpu-used', '8', '-g', '30',
    '-c:a', 'libopus', '-b:a', '128k', '-ac', '2'))
Make 'N_vp9_opus.webm' (@('-f', 'lavfi', '-i', (Video '1280x720' 30), '-f', 'lavfi', '-i', $tone, '-t', '4', '-map', '0:v', '-map', '1:a',
    '-c:v', 'libvpx-vp9', '-b:v', '4M', '-deadline', 'realtime', '-cpu-used', '8', '-g', '30', '-c:a', 'libopus', '-b:a', '128k'))

# O: two AAC tracks (440 Hz and 1 kHz tones) whose packet timestamps wobble around the sample count
# like ShadowPlay's capture-clock stamps: +-30 samples per packet, one packet 4 ms early every 23
# (track 1) or 4 ms late every 31 (track 2), and 1000 ppm of slow clock drift on track 2.
$jitter = "[0:a]asetpts='(N+20*sin(0.37*N/1024)+10*sin(1.91*N/1024)-200*eq(mod(N/1024,23),11))/SR/TB'[a0];" +
          "[1:a]asetpts='(N+0.001*N+15*sin(0.53*N/1024)+8*sin(2.3*N/1024)+180*eq(mod(N/1024,31),17))/SR/TB'[a1]"
Make 'O_aac_2trk_jitter.mp4' (@('-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:samples_per_frame=1024',
    '-f', 'lavfi', '-i', 'sine=frequency=1000:sample_rate=48000:samples_per_frame=1024', '-t', '20', '-filter_complex', $jitter,
    '-map', '[a0]', '-map', '[a1]', '-c:a', 'aac', '-b:a', '192k', '-ac', '2'))

# K: malformed media. K1 is a recording cut off before the moov atom was written; K2 has a zeroed region.
$a = Join-Path $out 'A_h264_1080p60_2trk.mp4'
$k1 = Join-Path $out 'K1_truncated.mp4'
if (-not (Test-Path $k1) -or $Force) {
    $bytes = [IO.File]::ReadAllBytes($a)
    [IO.File]::WriteAllBytes($k1, $bytes[0..([int]($bytes.Length * 0.5))])
    Write-Host 'create  K1_truncated.mp4'
}
$k2 = Join-Path $out 'K2_corrupt_middle.mp4'
if (-not (Test-Path $k2) -or $Force) {
    $bytes = [IO.File]::ReadAllBytes($a)
    $start = [int]($bytes.Length * 0.4)
    for ($i = $start; $i -lt $start + 262144; $i++) { $bytes[$i] = 0 }
    [IO.File]::WriteAllBytes($k2, $bytes)
    Write-Host 'create  K2_corrupt_middle.mp4'
}
Write-Host "Fixtures in $out"
