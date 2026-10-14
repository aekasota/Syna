# Checks that the FFmpeg DLLs may be redistributed with Syna (docs/FFMPEG.md, docs/LICENSE-AUDIT.md).
# The DLLs are read as data, not loaded. Fails, listing every problem, unless:
#  - each DLL reports an LGPL license and the same configuration, and that configuration is the one
#    recorded in docs/ffmpeg-buildconf.txt;
#  - the configuration enables neither GPL nor nonfree code;
#  - no GPL code came in through an external library, which FFmpeg's configure cannot see: FFTW
#    (chromaprint can be built with it) and libzvbi (its teletext decoder links GPL-2.0-only files);
#  - the package was built by scripts\ffmpeg\build.sh as it is now (its BUILDINFO.txt names the recipe),
#    whose check found no GPL-licensed source among the files compiled into the DLLs;
#  - every option the configuration enables is listed in $documented below, and the package's
#    licenses folder has the texts those options and the toolchain's runtime libraries require;
#  - with -SourceDir: that folder holds every source archive the build used, unchanged (the build's
#    ffmpeg-sources.sha256), the recipe, and the archive of each listed library.
# The configuration names only what was enabled explicitly; build.sh passes --disable-autodetect so
# that nothing else gets in.
param(
    [string]$Dir,
    [string]$SourceDir
)
. "$PSScriptRoot\common.ps1"

if (-not $Dir) { $Dir = Join-Path $RepoRoot 'third_party\ffmpeg\bin' }
$root = Split-Path -Parent $Dir

# Every option the FFmpeg configuration may enable. License: the text in the package's licenses
# folder that has to ship with the DLLs. Source: the library's archive among the release's sources.
# Neither: a Windows component, or FFmpeg's own code built on a library listed here.
$documented = @{
    'w32threads'      = @{}
    'd3d11va'         = @{}
    'd3d12va'         = @{}
    'dxva2'           = @{}
    'mediafoundation' = @{}
    'schannel'        = @{}
    'cuda'            = @{}
    'cuvid'           = @{}
    'nvdec'           = @{}
    'nvenc'           = @{}
    'ffnvcodec'       = @{ License = 'nv-codec-headers-LICENSE.txt'; Source = 'nv-codec-headers-*.tar.gz' }
    'amf'             = @{ License = 'AMF-LICENSE.txt'; Source = 'AMF-headers-*.tar.gz' }
    'libvpl'          = @{ License = 'libvpl-LICENSE.txt'; Source = 'libvpl-*.tar.gz' }
    'zlib'            = @{ License = 'zlib-LICENSE.txt'; Source = 'zlib-*.tar.xz' }
    'libdav1d'        = @{ License = 'dav1d-COPYING.txt'; Source = 'dav1d-*.tar.xz' }
    'libopus'         = @{ License = 'opus-COPYING.txt'; Source = 'opus-*.tar.gz' }
    'libmp3lame'      = @{ License = 'LAME-COPYING.txt'; Source = 'lame-*.tar.gz' }
    'libopenh264'     = @{ License = 'OpenH264-LICENSE.txt'; Source = 'openh264-*.tar.gz' }
    'libkvazaar'      = @{ License = 'Kvazaar-LICENSE.txt'; Source = 'kvazaar-*.tar.xz' }
    'libsvtav1'       = @{ License = 'SVT-AV1-LICENSE.md'; Source = 'SVT-AV1-*.tar.gz' }
    'libvpx'          = @{ License = 'libvpx-LICENSE.txt'; Source = 'libvpx-*.tar.gz' }
}
# Texts for code in every DLL that no option names: FFmpeg's own license and the runtime libraries of
# the toolchain (llvm-mingw).
$always = 'FFmpeg-COPYING.LGPLv2.1.txt', 'mingw-w64-runtime-COPYING.txt', 'winpthreads-COPYING.txt'
# Options that select how FFmpeg itself is built rather than a library.
$buildOptions = 'version3', 'shared', 'static', 'autodetect', 'pic', 'lto', 'optimizations', 'stripping', 'asm', 'x86asm', 'inline-asm', 'runtime-cpudetect', 'hardcoded-tables', 'small'

$libs = 'avutil', 'avcodec', 'avformat', 'avfilter', 'swresample', 'swscale'
$problems = New-Object System.Collections.Generic.List[string]
$latin1 = [Text.Encoding]::GetEncoding(28591)
$configs = @{}
$enabled = @()

foreach ($lib in $libs) {
    $file = Get-ChildItem $Dir -Filter "$lib-*.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $file) { $problems.Add("$lib-*.dll not found in $Dir."); continue }
    $text = $latin1.GetString([IO.File]::ReadAllBytes($file.FullName))

    $licenses = @([regex]::Matches($text, '(?<![A-Za-z])L?GPL version [0-9.]+ or later|nonfree and unredistributable') |
        ForEach-Object Value | Sort-Object -Unique)
    if ($licenses.Count -eq 0) { $problems.Add("$($file.Name): no license string found.") }
    foreach ($l in $licenses) {
        if ($l -notmatch '^LGPL ') { $problems.Add("$($file.Name) reports the license '$l'.") }
    }

    $config = [regex]::Matches($text, '--[a-z][\x20-\x7E]{40,}') | ForEach-Object Value |
        Sort-Object Length -Descending | Select-Object -First 1
    if (-not $config) { $problems.Add("$($file.Name): no configuration found.") } else { $configs[$file.Name] = $config }

    if ($text -match 'fftw-[0-9]+\.[0-9]+|fftw_codelet_') {
        $problems.Add("$($file.Name) contains FFTW, which is GPL-2.0-or-later (usually built in through chromaprint).")
    }
}

$distinct = @($configs.Values | Sort-Object -Unique)
if ($distinct.Count -gt 1) { $problems.Add("The DLLs report different configurations: $($configs.Keys -join ', ').") }
$config = $distinct | Select-Object -First 1
if ($config) {
    $options = @($config.Trim() -split '\s+')

    # ffmpeg -buildconf prints one option per line, except that it keeps "--pkg-config=pkg-config"
    # and the option after it together, so both sides are compared word by word.
    $recorded = Join-Path $RepoRoot 'docs\ffmpeg-buildconf.txt'
    $words = if (Test-Path $recorded) { @(Get-Content $recorded | ForEach-Object Trim | Where-Object { $_ -like '--*' } | ForEach-Object { $_ -split '\s+' }) } else { @() }
    if (-not $words) { $problems.Add('docs\ffmpeg-buildconf.txt is missing or has no configuration.') }
    elseif (Compare-Object $words $options -SyncWindow 0 -CaseSensitive) { $problems.Add('docs\ffmpeg-buildconf.txt does not match the configuration of the DLLs (update it with ffmpeg -buildconf).') }

    foreach ($flag in '--enable-gpl', '--enable-nonfree') {
        if ($options -contains $flag) { $problems.Add("The configuration contains $flag.") }
    }
    if ($options -contains '--enable-libzvbi') {
        $problems.Add('The configuration contains --enable-libzvbi: libzvbi''s decoder links src/packet-830.c, which is GPL-2.0-only.')
    }

    $enabled = @($options | ForEach-Object { if ($_ -match '^--enable-([a-z0-9-]+)$') { $Matches[1] } } |
        Where-Object { $_ -notin $buildOptions -and $_ -notin 'gpl', 'nonfree' })
    $undocumented = @($enabled | Where-Object { -not $documented.ContainsKey($_) })
    if ($undocumented) {
        $problems.Add("$($undocumented.Count) of $($enabled.Count) enabled options are not listed in this script with their license text and source: $($undocumented -join ', ').")
    }
}

# The package: built by the current recipe, with the license texts it needs.
$recipe = (Get-FileHash (Join-Path $RepoRoot 'scripts\ffmpeg\build.sh') -Algorithm SHA256).Hash.Substring(0, 12).ToLowerInvariant()
$buildInfo = Join-Path $root 'BUILDINFO.txt'
$built = if (Test-Path $buildInfo) { [regex]::Match((Get-Content $buildInfo -TotalCount 1), 'recipe ([0-9a-f]{12})').Groups[1].Value }
if (-not $built) { $problems.Add("$buildInfo is missing or names no recipe: the DLLs were not built by scripts\ffmpeg\build.sh.") }
elseif ($built -ne $recipe) { $problems.Add("The DLLs were built by recipe $built, but scripts\ffmpeg\build.sh is now recipe ${recipe}: rebuild FFmpeg.") }
elseif (-not (Select-String -Path $buildInfo -Pattern '^GPL source check: passed' -Quiet)) { $problems.Add("$buildInfo does not record a passed GPL source check.") }

$needed = @($always) + @($enabled | Where-Object { $documented.ContainsKey($_) -and $documented[$_].License } | ForEach-Object { $documented[$_].License })
foreach ($text in $needed | Sort-Object -Unique) {
    $path = Join-Path $root "licenses\$text"
    if (-not (Test-Path $path) -or (Get-Item $path).Length -eq 0) { $problems.Add("licenses\$text is missing from the FFmpeg package.") }
}

# The corresponding source that goes with the release.
if ($SourceDir) {
    $sums = Join-Path $SourceDir 'ffmpeg-sources.sha256'
    if (-not (Test-Path $sums)) { $problems.Add("$sums is missing.") }
    else {
        foreach ($line in Get-Content $sums) {
            if ($line -notmatch '^([0-9a-f]{64})\s+\*?(.+)$') { continue }
            $file = Join-Path $SourceDir $Matches[2]
            if (-not (Test-Path $file)) { $problems.Add("Source archive $($Matches[2]) is missing from $SourceDir.") }
            elseif ((Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Matches[1]) { $problems.Add("Source archive $($Matches[2]) does not match its SHA-256.") }
        }
    }
    foreach ($pattern in @('FFmpeg-*.tar.gz', "syna-ffmpeg-recipe-$recipe.tar.gz") + @($enabled | Where-Object { $documented.ContainsKey($_) -and $documented[$_].Source } | ForEach-Object { $documented[$_].Source })) {
        if (-not (Get-ChildItem $SourceDir -Filter $pattern -ErrorAction SilentlyContinue)) { $problems.Add("No source archive $pattern in $SourceDir.") }
    }
}

if ($problems.Count) {
    foreach ($p in $problems) {
        if ($env:GITHUB_ACTIONS -eq 'true') { Write-Host "::error title=FFmpeg license check::$p" } else { Write-Host "- $p" }
    }
    Write-Host "The FFmpeg DLLs in $Dir must not be redistributed with Syna ($($problems.Count) problems; docs/FFMPEG.md)."
    exit 1
}
$sourceNote = if ($SourceDir) { ', all sources present' } else { '' }
Write-Host "FFmpeg DLLs in ${Dir}: LGPL, no GPL or nonfree code found, recipe $recipe, $($enabled.Count) options, all documented$sourceNote."
