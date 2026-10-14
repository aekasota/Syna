# Renders the app icon (.ico) and the README icon (.png) from the app mark used in the title bar
# (Controls/Icon.cs, IconKind.AppMark). Output is committed; re-run only when the mark changes.
# Uses WPF from Windows PowerShell, so no imaging dependency is needed.
param(
    [string]$OutDir = (Join-Path $PSScriptRoot '..\src\Syna.App\Assets'),
    [string]$DocsDir = (Join-Path $PSScriptRoot '..\docs\assets')
)

Add-Type -AssemblyName PresentationCore, PresentationFramework, WindowsBase
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir, $DocsDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$DocsDir = (Resolve-Path $DocsDir).Path

# Same 20-unit path data as IconKind.AppMark: two signals easing into one line.
$mark = [System.Windows.Media.Geometry]::Parse('M3.5 5 H6 C8.75 5 8.75 10 11.5 10 H16.5 M4.5 15 H6 C8.75 15 8.75 10 11.5 10')

function Color([string]$hex) { [System.Windows.Media.ColorConverter]::ConvertFromString($hex) }
function Brush([string]$hex) { $b = New-Object System.Windows.Media.SolidColorBrush (Color $hex); $b.Freeze(); $b }

$plateFill = Brush '#17181B'
$plateEdge = Brush '#2C2E33'
$accent = Brush '#E89B3A'

# The mark in the accent color on a dark plate.
function Render([int]$size, [double]$glyphFraction = 0.56) {
    $dv = New-Object System.Windows.Media.DrawingVisual
    $dc = $dv.RenderOpen()
    $s = $size
    $cx = $size / 2.0; $cy = $size / 2.0

    # Plate: a rounded square, inset slightly so the edge is not clipped at small sizes.
    $inset = [Math]::Max(0.5, $s * 0.04)
    $side = $s - 2 * $inset
    $radius = $side * 0.22
    $edge = New-Object System.Windows.Media.Pen($plateEdge, [Math]::Max(1.0, $s / 128.0))
    $rect = New-Object System.Windows.Rect(($cx - $side / 2), ($cy - $side / 2), $side, $side)
    $dc.DrawRoundedRectangle($plateFill, $edge, $rect, $radius, $radius)

    # Glyph: the 14.5-unit stroked extent maps to $glyphFraction of the plate.
    $unit = $s * $glyphFraction / 14.5
    $group = New-Object System.Windows.Media.TransformGroup
    $group.Children.Add((New-Object System.Windows.Media.TranslateTransform(-10, -10)))
    $group.Children.Add((New-Object System.Windows.Media.ScaleTransform($unit, $unit)))
    $group.Children.Add((New-Object System.Windows.Media.TranslateTransform($cx, $cy)))
    $dc.PushTransform($group)
    # Thicker strokes where pixels are scarce, so the two lines stay distinct at 16 px.
    $stroke = if ($s * $glyphFraction -lt 14) { 2.0 } else { 1.6 }
    $pen = New-Object System.Windows.Media.Pen($accent, $stroke)
    $pen.StartLineCap = 'Round'; $pen.EndLineCap = 'Round'; $pen.LineJoin = 'Round'
    $dc.DrawGeometry($null, $pen, $mark)
    $dc.Pop()
    $dc.Close()

    $bmp = New-Object System.Windows.Media.Imaging.RenderTargetBitmap($size, $size, 96, 96, [System.Windows.Media.PixelFormats]::Pbgra32)
    $bmp.Render($dv)
    $encoder = New-Object System.Windows.Media.Imaging.PngBitmapEncoder
    $encoder.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($bmp))
    $ms = New-Object System.IO.MemoryStream
    $encoder.Save($ms)
    return , $ms.ToArray()
}

# Executable / window icon: PNG-compressed frames in one .ico.
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$frames = foreach ($size in $sizes) { , (Render $size) }
$ico = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($ico)
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$frames[$i].Length); $w.Write([uint32]$offset)
    $offset += $frames[$i].Length
}
foreach ($f in $frames) { $w.Write($f) }
$w.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $OutDir 'AppIcon.ico'), $ico.ToArray())

[System.IO.File]::WriteAllBytes((Join-Path $DocsDir 'syna-icon.png'), $frames[-1])
Write-Host "Icons written to $OutDir and $DocsDir"
