# Captures the README screenshots into docs\assets\screenshots from the development build. The clips
# are synthetic (generated here), the app runs in a separate profile (SYNA_DATA_DIR) and the clips sit
# on a temporary drive letter, so no personal file, path or setting can appear in an image.
# Needs an FFmpeg command-line tool with NVENC or libx264 (SYNA_FIXTURE_FFMPEG or PATH, as for
# make-fixtures.ps1) and an interactive desktop: the window is shown without taking the focus while
# it is captured, and minimized in between. Nothing is played, so there is no sound.
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Drive = 'S:',
    [switch]$NoBuild,
    [switch]$Regenerate
)
. "$PSScriptRoot\common.ps1"
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing

$work = Join-Path $RepoRoot 'artifacts\screenshots'
$mediaRoot = Join-Path $work 'media'
$profileDir = Join-Path $work 'profile'
$outDir = Join-Path $RepoRoot 'docs\assets\screenshots'
$exe = Join-Path (Get-AppOutputDir 'Release') 'Syna.exe'

if (-not $NoBuild) { & "$PSScriptRoot\build.ps1" -Configuration Release }
if (-not (Test-Path $exe)) { throw "The development build is missing: $exe" }
if (Get-Process Syna -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) { throw 'The development build is running; close it first.' }

# ---- Synthetic clips: 1080p60 H.264, keyframes every 2 s, named audio tracks like a game recorder's.
$ffmpeg = if ($env:SYNA_FIXTURE_FFMPEG) { $env:SYNA_FIXTURE_FFMPEG } else { (Get-Command ffmpeg -ErrorAction Stop).Source }
$encoders = & $ffmpeg -hide_banner -encoders 2>$null | Out-String
$video = if ($encoders -match 'h264_nvenc') { @('-c:v', 'h264_nvenc', '-preset', 'p5', '-rc', 'vbr', '-cq', '21', '-b:v', '0') }
         else { @('-c:v', 'libx264', '-preset', 'medium', '-crf', '20') }

function Tone([double]$root, [double]$swell) {
    "aevalsrc='0.15*(sin(2*PI*$root*t)+sin(2*PI*$($root * 1.26)*t)+sin(2*PI*$($root * 1.5)*t))*(0.55+0.45*sin(2*PI*$swell*t))':s=48000"
}
function Voice([double]$period, [double]$talk) {
    "anoisesrc=color=pink:amplitude=0.7:sample_rate=48000,highpass=f=200,lowpass=f=3200,volume='if(lt(mod(t\,$period)\,$talk)\,0.9*(0.6+0.4*sin(2*PI*5*t))\,0.02)':eval=frame"
}
$clips = @(
    @{ Name = 'Fractal Dive 2026.10.11 - 21.04.17.mp4'; Seconds = 20
       Video = 'mandelbrot=size=960x540:rate=60:maxiter=600:start_scale=2.6:end_scale=0.02:end_pts=1200:start_x=-0.743643887037151:start_y=0.131825904205330,format=gray,format=yuv444p,pseudocolor=preset=magma,scale=1920:1080:flags=lanczos'
       Audio = @(@{ Title = 'Desktop audio'; Source = (Tone 196 0.3) }, @{ Title = 'Microphone'; Source = (Voice 3.4 1.6) }) },
    @{ Name = 'Cell Colony 2026.10.11 - 21.32.40.mp4'; Seconds = 18
       Video = 'life=size=192x108:rate=30:ratio=0.33:mold=0:life_color=#E89B3A:death_color=#141519,scale=1920:1080:flags=neighbor'
       Audio = @(@{ Title = 'Desktop audio'; Source = (Tone 220 0.5) }, @{ Title = 'Microphone'; Source = (Voice 2.6 1.1) }) },
    @{ Name = 'Aurora 2026.10.12 - 19.15.02.mp4'; Seconds = 24
       Video = 'gradients=size=1920x1080:rate=60:c0=0x0B3D5B:c1=0x1F8A70:c2=0x101A2E:c3=0x6A3D9A:c4=0x0E6E73:nb_colors=5:speed=0.012:type=spiral'
       Audio = @(@{ Title = 'Desktop audio'; Source = (Tone 174.6 0.2) }, @{ Title = 'Microphone'; Source = (Voice 4.1 1.9) },
                 @{ Title = 'Voice chat'; Source = (Voice 5.3 1.2) }, @{ Title = 'Music'; Source = (Tone 261.6 0.9) }) },
    @{ Name = 'Sierpinski Run 2026.10.12 - 19.48.55.mp4'; Seconds = 16
       Video = 'sierpinski=size=1920x1080:rate=60:type=triangle:jump=40,format=gray,format=yuv444p,pseudocolor=preset=cividis'
       Audio = @(@{ Title = 'Desktop audio'; Source = (Tone 246.9 0.7) }, @{ Title = 'Microphone'; Source = (Voice 3.0 0.8) }) },
    @{ Name = 'Plasma Drift 2026.10.13 - 08.02.31.mp4'; Seconds = 12
       Video = "color=size=384x216:rate=60,format=gray,geq=lum='128+62*sin(hypot(X-192\,Y-108)/13-T*2.1)+48*sin(X/19+T*1.3)',format=yuv444p,pseudocolor=preset=viridis,scale=1920:1080:flags=bicubic"
       Audio = @(@{ Title = 'Desktop audio'; Source = (Tone 293.7 0.4) }, @{ Title = 'Microphone'; Source = (Voice 2.2 0.7) }) }
)
$recordings = Join-Path $mediaRoot 'Recordings'
New-Item -ItemType Directory -Force $recordings, (Join-Path $mediaRoot 'Trims') | Out-Null
foreach ($c in $clips) {
    $file = Join-Path $recordings $c.Name
    if ((Test-Path $file) -and -not $Regenerate) { continue }
    Write-Host "Generating $($c.Name)"
    $inputs = @('-f', 'lavfi', '-i', $c.Video)
    $maps = @('-map', '0:v')
    for ($i = 0; $i -lt $c.Audio.Count; $i++) {
        $inputs += '-f', 'lavfi', '-i', $c.Audio[$i].Source
        $maps += '-map', "$($i + 1):a", "-metadata:s:a:$i", "title=$($c.Audio[$i].Title)"
    }
    & $ffmpeg -hide_banner -loglevel error -y @inputs -t $c.Seconds @maps @video -g 120 -pix_fmt yuv420p `
        -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -c:a aac -b:a 160k -ac 2 $file
    if ($LASTEXITCODE -ne 0) { Remove-Item $file -ErrorAction SilentlyContinue; throw "ffmpeg failed for $($c.Name)" }
}

# ---- Window capture without activation, cropped to the visible frame.
if (-not ('ShotWin' -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Runtime.InteropServices; using System.Drawing; using System.Drawing.Imaging;
public static class ShotWin {
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
  [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct STARTUPINFO { public int cb; public string lpReserved, lpDesktop, lpTitle; public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags; public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError; }
  [StructLayout(LayoutKind.Sequential)] struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  // Starts the app without handing it this console, with this process's environment.
  public static int Start(string exe, string args) {
    var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(si);
    PROCESS_INFORMATION pi;
    if (!CreateProcess(exe, "\"" + exe + "\" " + args, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, System.IO.Path.GetDirectoryName(exe), ref si, out pi))
      throw new System.ComponentModel.Win32Exception();
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return pi.dwProcessId;
  }
  // PrintWindow with PW_RENDERFULLCONTENT also captures the DirectComposition video surface.
  public static Bitmap Capture(IntPtr h) {
    RECT w, f; GetWindowRect(h, out w);
    if (DwmGetWindowAttribute(h, 9, out f, Marshal.SizeOf(typeof(RECT))) != 0) f = w;
    using (var full = new Bitmap(w.R - w.L, w.B - w.T)) {
      using (var g = Graphics.FromImage(full)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); }
      return full.Clone(new Rectangle(f.L - w.L, f.T - w.T, f.R - f.L, f.B - f.T), PixelFormat.Format32bppArgb);
    }
  }
}
"@
}

$A = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$CT = [System.Windows.Automation.ControlType]

function Wait-Until([scriptblock]$Condition, [int]$TimeoutMs = 15000) {
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    do { $r = & $Condition; if ($r) { return $r }; Start-Sleep -Milliseconds 100 } while ((Get-Date) -lt $deadline)
    return $null
}
function Find($Root, [string]$Name, $Type, [int]$TimeoutMs = 8000) {
    $c = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition($A::NameProperty, $Name)),
        (New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty, $Type)))
    $el = Wait-Until { $Root.FindFirst($TS::Descendants, $c) } $TimeoutMs
    if (-not $el) { throw "'$Name' not found" }
    return $el
}
function Find-All($Root, [string]$Name, $Type) {
    $c = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition($A::NameProperty, $Name)),
        (New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty, $Type)))
    return @($Root.FindAll($TS::Descendants, $c))
}
# Presses a button, selects an item or flips a toggle, whichever the element supports.
function Use($El) {
    $p = $null
    foreach ($pattern in [System.Windows.Automation.InvokePattern]::Pattern, [System.Windows.Automation.SelectionItemPattern]::Pattern, [System.Windows.Automation.TogglePattern]::Pattern) {
        if ($El.TryGetCurrentPattern($pattern, [ref]$p)) {
            if ($p -is [System.Windows.Automation.InvokePattern]) { $p.Invoke() } elseif ($p -is [System.Windows.Automation.SelectionItemPattern]) { $p.Select() } else { $p.Toggle() }
            Start-Sleep -Milliseconds 250
            return
        }
    }
    throw "'$($El.Current.Name)' cannot be used"
}
function Press([string]$Name, [int]$Times = 1) { for ($i = 0; $i -lt $Times; $i++) { Use (Find $script:root $Name $CT::Button) } }
function Set-Range($El, [double]$Value) {
    $p = $null
    if (-not $El.TryGetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern, [ref]$p)) { throw "'$($El.Current.Name)' has no range" }
    $p.SetValue($Value)
}

# Shows the window without activating it, lets it render, captures it and minimizes it again.
function Shot([string]$Name, $CropTo = $null) {
    [ShotWin]::ShowWindow($script:hwnd, 4) | Out-Null
    Start-Sleep -Milliseconds 900
    $bmp = [ShotWin]::Capture($script:hwnd)
    if ($CropTo) {
        $r = $CropTo.Current.BoundingRectangle
        $win = $script:root.Current.BoundingRectangle
        $pad = 24
        $x = [Math]::Max(0, [int]($r.X - $win.X) - $pad); $y = [Math]::Max(0, [int]($r.Y - $win.Y) - $pad)
        $w = [Math]::Min($bmp.Width - $x, [int]$r.Width + 2 * $pad); $h = [Math]::Min($bmp.Height - $y, [int]$r.Height + 2 * $pad)
        $crop = $bmp.Clone((New-Object System.Drawing.Rectangle($x, $y, $w, $h)), $bmp.PixelFormat)
        $bmp.Dispose(); $bmp = $crop
    }
    $path = Join-Path $outDir "$Name.png"
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host ("Saved {0} ({1}x{2})" -f $path, $bmp.Width, $bmp.Height)
    $bmp.Dispose()
    [ShotWin]::ShowWindow($script:hwnd, 7) | Out-Null
}

# ---- A fresh profile: dark theme, explorer in the demo folder, trims saved to the demo drive.
if (Test-Path "$Drive\") { throw "Drive $Drive is in use; choose another letter with -Drive." }
Remove-Item -Recurse -Force $profileDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $profileDir, $outDir | Out-Null
$settings = [ordered]@{
    autoplay = $false; restoreSessions = $false; rememberPosition = $false; masterGain = 0.8; theme = 'Dark'
    explorerVisible = $true; railVisible = $true; audioPanelVisible = $true; showDiagnostics = $false
    windowBounds = @(153, 62, 1614, 907); windowMaximized = $false  # a 1600 x 900 visible frame
    lastFolder = "$Drive\Recordings"; trimFolder = "$Drive\Trims"; trimMode = 'Smart'; trimProfile = 'Original'; trimAudio = 'Mix'
}
[IO.File]::WriteAllText((Join-Path $profileDir 'settings.json'), ($settings | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
Get-ChildItem (Join-Path $mediaRoot 'Trims') -File | Remove-Item

subst $Drive $mediaRoot
if ($LASTEXITCODE -ne 0) { throw "subst $Drive failed" }
$process = $null
try {
    $files = $clips | ForEach-Object { "`"$Drive\Recordings\$($_.Name)`"" }
    $env:SYNA_DATA_DIR = $profileDir
    $process = Get-Process -Id ([ShotWin]::Start($exe, ($files -join ' ')))
    Remove-Item Env:\SYNA_DATA_DIR
    $script:hwnd = Wait-Until { $process.Refresh(); if ($process.MainWindowHandle -ne 0) { $process.MainWindowHandle } } 20000
    if (-not $script:hwnd) { throw 'The window did not appear.' }
    $null = Wait-Until { [ShotWin]::IsWindowVisible($script:hwnd) } 5000
    [ShotWin]::ShowWindow($script:hwnd, 7) | Out-Null
    $script:root = $A::FromHandle($script:hwnd)
    $rail = Find $script:root 'Open clips' $CT::List 20000
    $null = Find $rail $clips[-1].Name $CT::ListItem 20000
    Start-Sleep -Seconds 6  # thumbnails, waveforms and the explorer's metadata

    # Main view: the first clip at a moment with detail in it.
    Use (Find $rail $clips[0].Name $CT::ListItem)
    Start-Sleep -Seconds 2
    Press 'Skip forward' 16
    Start-Sleep -Seconds 1
    Shot 'main-player'

    # Mixer: the four-track clip with the tracks set differently.
    Use (Find $rail $clips[2].Name $CT::ListItem)
    Start-Sleep -Seconds 3
    Press 'Skip forward' 6
    Set-Range (Find $script:root 'Microphone' $CT::Slider) 1.35
    Set-Range (Find $script:root 'Voice chat' $CT::Slider) 0.55
    Use (Find-All $script:root 'Mute' $CT::Button)[3]
    Start-Sleep -Seconds 1
    Shot 'audio-mixer'

    # Trim: a selection on the first clip, the sheet before and after saving.
    Use (Find $rail $clips[0].Name $CT::ListItem)
    Start-Sleep -Seconds 2
    Press 'Skip back' 11
    Press 'Set IN'
    Press 'Skip forward' 8
    Press 'Set OUT'
    Press 'Trim'
    $sheet = Find $script:root 'Trim clip' $CT::Text
    Start-Sleep -Seconds 1
    Shot 'trim-workflow'
    Press 'Save clip'
    $done = Find $script:root 'Done' $CT::Button 60000
    Start-Sleep -Seconds 1
    Shot 'trim-output'
    Use $done
    Start-Sleep -Seconds 6  # the saved-clip notification leaves

    # Settings: the audio section.
    Press 'Settings'
    Use (Find $script:root 'Audio' $CT::RadioButton)
    Start-Sleep -Seconds 1
    Shot 'settings'

    # The light theme, with the explorer filtered and another clip open.
    Use (Find $script:root 'Light' $CT::RadioButton)
    Press 'Close settings'
    $filter = Find $script:root 'Filter files' $CT::Edit
    $value = $null
    if ($filter.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$value)) { $value.SetValue('2026.10.12') }
    Use (Find $rail $clips[1].Name $CT::ListItem)
    Start-Sleep -Seconds 2
    Press 'Skip forward' 7
    Start-Sleep -Seconds 1
    Shot 'library-explorer'
}
finally {
    if ($process -and -not $process.HasExited) { $process.CloseMainWindow() | Out-Null; if (-not $process.WaitForExit(15000)) { $process.Kill() } }
    subst $Drive /d
}
Write-Host "Screenshots in $outDir"
