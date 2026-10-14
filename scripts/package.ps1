# Builds the Windows installer into artifacts\:
#   app\                          the self-contained app the installer contains
#   Syna-Setup-<version>-x64.msi  the installer (per-user, no administrator rights needed)
#   Syna-Setup-<version>-x64.msi.sha256
# The version defaults to <Version> in src\Syna.App\Syna.App.csproj.
# Signing is optional: with SYNA_SIGN_CERT_FILE (a .pfx) and SYNA_SIGN_CERT_PASSWORD set, Syna.exe and
# the MSI are signed and timestamped (SYNA_SIGN_TIMESTAMP_URL, default DigiCert). See docs/PACKAGING.md.
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$Version
)
. "$PSScriptRoot\common.ps1"

$project = Join-Path $RepoRoot 'src\Syna.App\Syna.App.csproj'
$artifacts = Join-Path $RepoRoot 'artifacts'
$app = Join-Path $artifacts 'app'
$work = Join-Path $artifacts "app-build\$Configuration"
$installerWork = Join-Path $artifacts 'installer'

if (-not $Version) { $Version = ([xml](Get-Content $project)).Project.PropertyGroup.Version | Where-Object { $_ } | Select-Object -First 1 }
if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)$' -or [int]$Matches[1] -gt 255 -or [int]$Matches[2] -gt 255 -or [int]$Matches[3] -gt 65535) {
    throw "Version '$Version' cannot be used for an installer: use major.minor.patch, at most 255.255.65535."
}
$msiName = "Syna-Setup-$Version-x64.msi"
$msi = Join-Path $artifacts $msiName

function Get-SignTool {
    $packages = (dotnet nuget locals global-packages --list) -replace '^global-packages:\s*', ''
    $tool = Get-ChildItem (Join-Path $packages 'microsoft.windows.sdk.buildtools') -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq 'x64' } | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $tool) { throw 'signtool.exe not found (Microsoft.Windows.SDK.BuildTools package, restored by the app build).' }
    return $tool.FullName
}

function Invoke-Sign([string]$file) {
    if (-not $env:SYNA_SIGN_CERT_FILE) { return }
    $timestamp = if ($env:SYNA_SIGN_TIMESTAMP_URL) { $env:SYNA_SIGN_TIMESTAMP_URL } else { 'http://timestamp.digicert.com' }
    $tool = Get-SignTool
    Invoke-Checked "sign $(Split-Path $file -Leaf)" {
        & $tool sign /fd SHA256 /f $env:SYNA_SIGN_CERT_FILE /p $env:SYNA_SIGN_CERT_PASSWORD /tr $timestamp /td SHA256 $file | Out-Host
    }
}

# Plain text as RTF for the installer's license page.
function ConvertTo-Rtf([string]$text) {
    $sb = New-Object System.Text.StringBuilder
    foreach ($ch in $text.ToCharArray()) {
        $code = [int]$ch
        if ($ch -eq '\' -or $ch -eq '{' -or $ch -eq '}') { [void]$sb.Append('\').Append($ch) }
        elseif ($ch -eq "`n") { [void]$sb.Append("\par`r`n") }
        elseif ($ch -eq "`r") { }
        elseif ($code -gt 127) { [void]$sb.Append('\u').Append($(if ($code -gt 32767) { $code - 65536 } else { $code })).Append('?') }
        else { [void]$sb.Append($ch) }
    }
    return $sb.ToString()
}

& "$PSScriptRoot\build.ps1" -Configuration $Configuration -NativeOnly

# Published from scratch into its own folders. Sharing bin\ and obj\ with the development build (which
# uses the installed Windows App Runtime) can reuse its Syna.pri, which lacks WinUI's own resources,
# and the installed app would then fail inside Microsoft.UI.Xaml.dll before showing a window.
Remove-Item -Recurse -Force $app, $work, $installerWork -ErrorAction SilentlyContinue
Invoke-Checked 'publish app' {
    dotnet publish $project -c $Configuration -p:Platform=x64 -p:SelfContained=true -p:WindowsAppSDKSelfContained=true `
        -p:Version=$Version -p:DebugType=none `
        --artifacts-path "$work" -p:PublishDir="$app\" -nologo | Out-Host
}

# The installed app runs from this folder alone: check what it needs before anything else loads.
$missing = @('Syna.exe', 'Syna.pri', 'Microsoft.UI.Xaml.dll', 'Syna.Native.dll', 'Assets\AppIcon.ico', 'licenses\Syna-LICENSE.txt',
    'licenses\ffmpeg\FFmpeg-COPYING.LGPLv2.1.txt') |
    Where-Object { -not (Test-Path (Join-Path $app $_)) }
$missing += @('avcodec', 'avformat', 'avutil', 'avfilter', 'swresample', 'swscale') |
    Where-Object { -not (Get-ChildItem $app -Filter "$_-*.dll") }
if ($missing) { throw "The app build is incomplete and would not start when installed (missing: $($missing -join ', '))." }
$check = Start-Process (Join-Path $app 'Syna.exe') -ArgumentList '--self-check' -Wait -PassThru
if ($check.ExitCode -ne 0) { throw "The app build cannot start (Syna.exe --self-check returned $($check.ExitCode))." }

# License texts and notices that come with NuGet packages, at the versions this build restored. The
# Windows App SDK Runtime package only brings build targets, so it is not in Syna.deps.json; its
# NOTICE.txt covers the whole SDK (the WinUI package's is a shorter copy).
$packages = (dotnet nuget locals global-packages --list) -replace '^global-packages:\s*', ''
$deps = Get-Content (Join-Path $app 'Syna.deps.json') -Raw | ConvertFrom-Json
$sdkRuntime = ([xml](Get-Content $project)).Project.ItemGroup.PackageReference | Where-Object { $_.Include -eq 'Microsoft.WindowsAppSDK.Runtime' }
$licenseFiles = @(
    @{ Package = 'runtimepack.Microsoft.NETCore.App.Runtime.win-x64'; Folder = 'microsoft.netcore.app.runtime.win-x64'; File = 'LICENSE.TXT'; As = 'dotnet-LICENSE.txt' },
    @{ Package = 'runtimepack.Microsoft.NETCore.App.Runtime.win-x64'; Folder = 'microsoft.netcore.app.runtime.win-x64'; File = 'THIRD-PARTY-NOTICES.TXT'; As = 'dotnet-THIRD-PARTY-NOTICES.txt' },
    @{ Package = 'Microsoft.WindowsAppSDK.WinUI'; Folder = 'microsoft.windowsappsdk.winui'; File = 'license.txt'; As = 'WindowsAppSDK-LICENSE.txt' },
    @{ Package = 'Microsoft.WindowsAppSDK.Runtime'; Folder = 'microsoft.windowsappsdk.runtime'; Version = $sdkRuntime.Version; File = 'NOTICE.txt'; As = 'WindowsAppSDK-NOTICE.txt' },
    @{ Package = 'Microsoft.Web.WebView2'; Folder = 'microsoft.web.webview2'; File = 'LICENSE.txt'; As = 'WebView2-LICENSE.txt' },
    @{ Package = 'Microsoft.Web.WebView2'; Folder = 'microsoft.web.webview2'; File = 'NOTICE.txt'; As = 'WebView2-NOTICE.txt' }
)
foreach ($l in $licenseFiles) {
    # Not $version: PowerShell names ignore case, and $Version is the installer's version.
    $packageVersion = $l.Version
    if (-not $packageVersion) {
        $id = $deps.libraries.PSObject.Properties.Name | Where-Object { $_ -like "$($l.Package)/*" } | Select-Object -First 1
        if ($id) { $packageVersion = $id.Split('/')[1] }
    }
    $source = if ($packageVersion) { Join-Path $packages "$($l.Folder)\$packageVersion\$($l.File)" }
    if (-not $source -or -not (Test-Path $source)) { throw "$($l.File) of $($l.Package) not found in the NuGet cache ($source)." }
    Copy-Item $source (Join-Path $app "licenses\$($l.As)")
}

Invoke-Sign (Join-Path $app 'Syna.exe')

# The license page: Syna's MIT License, then Microsoft's terms for the Windows App SDK files and the
# Windows SDK projection, which end users have to accept (docs/THIRD-PARTY-NOTICES.md).
New-Item -ItemType Directory -Force $installerWork | Out-Null
$mit = Get-Content (Join-Path $RepoRoot 'LICENSE') -Raw -Encoding UTF8
$sdk = Get-Content (Join-Path $app 'licenses\WindowsAppSDK-LICENSE.txt') -Raw -Encoding UTF8
$thirdParty = "Syna also contains third-party components that remain under their own licenses: FFmpeg and the libraries " +
    "built into its DLLs, the .NET runtime and Microsoft.Data.Sqlite (MIT), SQLitePCLRaw (Apache-2.0), SQLite (public domain), " +
    "the Microsoft Edge WebView2 SDK, the Windows SDK projection for .NET and the Microsoft Windows App SDK. Their license texts " +
    "and notices are installed in the licenses folder next to Syna.exe (THIRD-PARTY-NOTICES.md).`n`nBy installing Syna you " +
    "also accept Microsoft's license terms for the Microsoft components it includes: the Windows SDK license terms " +
    "(https://aka.ms/WinSDKLicenseURL) for Microsoft.Windows.SDK.NET.dll and WinRT.Runtime.dll, and the following terms for " +
    "the Windows App SDK components."
$rtf = Join-Path $installerWork 'License.rtf'
$body = '{\rtf1\ansi\deff0{\fonttbl{\f0\fswiss Segoe UI;}}\viewkind4\uc1\pard\f0\fs17 ' +
    (ConvertTo-Rtf $mit) + '\par ' + (ConvertTo-Rtf $thirdParty) + '\par\par ' + (ConvertTo-Rtf $sdk) + '}'
[IO.File]::WriteAllText($rtf, $body, [Text.Encoding]::ASCII)

Invoke-Checked 'build installer' {
    dotnet build (Join-Path $RepoRoot 'installer\Syna.Installer.wixproj') -c Release `
        -p:ProductVersion=$Version -p:AppDir="$app\" -p:LicenseRtf="$rtf" -o (Join-Path $installerWork 'bin') -nologo | Out-Host
}
$built = Get-ChildItem (Join-Path $installerWork 'bin') -Recurse -Filter *.msi | Select-Object -First 1
if (-not $built) { throw 'The installer build produced no .msi.' }
Copy-Item $built.FullName $msi -Force
Invoke-Sign $msi

# What Windows Installer will see: the product's identity and scope, and every file of the app.
function Invoke-MsiQuery($database, [string]$sql) {
    $view = $database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $database, @($sql))
    [void]$view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null)
    $rows = @()
    while ($record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)) {
        $rows += $record.GetType().InvokeMember('StringData', 'GetProperty', $null, $record, 1)
    }
    [void]$view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null)
    return , $rows
}
$installer = New-Object -ComObject WindowsInstaller.Installer
$database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @([string]$msi, 0))
$expected = @{ ProductName = 'Syna'; ProductVersion = $Version; Manufacturer = 'hyak'; UpgradeCode = '{4B38677F-67FF-49EE-9D91-215972F7BB22}'; ALLUSERS = '2'; MSIINSTALLPERUSER = '1' }
foreach ($name in $expected.Keys) {
    $value = (Invoke-MsiQuery $database "SELECT Value FROM Property WHERE Property='$name'") | Select-Object -First 1
    if ($value -ne $expected[$name]) { throw "The installer's $name is '$value', expected '$($expected[$name])'." }
}
$fileCount = (Invoke-MsiQuery $database 'SELECT File FROM File').Count
$appCount = @(Get-ChildItem $app -Recurse -File).Count
if ($fileCount -ne $appCount) { throw "The installer contains $fileCount files, the app $appCount." }
foreach ($table in 'Shortcut', 'CustomAction') {
    if (-not (Invoke-MsiQuery $database "SELECT * FROM $table").Count) { throw "The installer has no $table entries." }
}
[void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($database)
[void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
[GC]::Collect()
[GC]::WaitForPendingFinalizers()

# Unpack the installer the way Windows Installer reads it (an administrative image: no registration,
# no shortcuts) and start-check the result.
$image = Join-Path $installerWork 'image'
$p = Start-Process msiexec.exe -ArgumentList '/a', "`"$msi`"", '/qn', "TARGETDIR=`"$image`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "Extracting the installer failed (msiexec exit code $($p.ExitCode))." }
$extracted = Get-ChildItem $image -Recurse -Filter Syna.exe | Select-Object -First 1
if (-not $extracted -or @(Get-ChildItem $extracted.DirectoryName -Recurse -File).Count -ne $appCount) { throw 'The extracted installer does not contain the whole app.' }
$check = Start-Process $extracted.FullName -ArgumentList '--self-check' -Wait -PassThru
if ($check.ExitCode -ne 0) { throw "The app extracted from the installer cannot start (--self-check returned $($check.ExitCode))." }

$hash = (Get-FileHash $msi -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$msi.sha256", "$hash  $msiName`n", [Text.Encoding]::ASCII)
$signed = if ($env:SYNA_SIGN_CERT_FILE) { 'signed' } else { 'unsigned' }
Write-Host ("Installer: {0} ({1:N1} MB, {2}, {3} files)" -f $msi, ((Get-Item $msi).Length / 1MB), $signed, $fileCount)
Write-Host "SHA-256:   $hash"
