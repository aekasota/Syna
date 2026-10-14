# Installs an installer silently for the current user, checks the installation, uninstalls it and
# checks the removal. Used by CI and the release workflow. Where Syna is already installed it refuses
# to run unless -Force, because the uninstall step removes that installation too. Data in
# %LOCALAPPDATA%\Syna is never touched. Logs go to artifacts\install-test\.
param(
    [Parameter(Mandatory)][string]$Msi,
    [switch]$Force
)
. "$PSScriptRoot\common.ps1"

$Msi = (Resolve-Path $Msi).Path
$upgradeCode = '{4B38677F-67FF-49EE-9D91-215972F7BB22}'
$expectedTypes = '.mp4 .m4v .mov .mkv .webm'
$installer = New-Object -ComObject WindowsInstaller.Installer
function Get-InstalledProducts { @($installer.GetType().InvokeMember('RelatedProducts', 'GetProperty', $null, $installer, @($upgradeCode))) }

if ((Get-InstalledProducts).Count -and -not $Force) {
    throw 'Syna is installed on this machine and this test would uninstall it. Run with -Force to allow that.'
}

$logs = Join-Path $RepoRoot 'artifacts\install-test'
New-Item -ItemType Directory -Force $logs | Out-Null
$dir = Join-Path $env:LOCALAPPDATA 'Programs\Syna'
$exe = Join-Path $dir 'Syna.exe'
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'Syna.lnk'

# Runs msiexec quietly with a verbose log; returns the file types the app reported in that log.
function Invoke-Msiexec([string]$action, [string]$target, [string]$name) {
    $log = Join-Path $logs "$name.log"
    $p = Start-Process msiexec.exe -ArgumentList $action, "`"$target`"", '/qn', '/l*v', "`"$log`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "msiexec $action returned $($p.ExitCode); see $log" }
    $line = Get-Content $log -Encoding Unicode | Select-String 'File types that open with .+?: (.*)$' | Select-Object -Last 1
    if ($line) { return $line.Matches[0].Groups[1].Value.Trim() }
    return '(not reported)'
}

$results = [ordered]@{}
$types = Invoke-Msiexec '/i' $Msi 'install'
$results['installed in the program folder'] = Test-Path $exe
$results['Start menu shortcut'] = Test-Path $shortcut
$results['one installed product'] = (Get-InstalledProducts).Count -eq 1
$results["file types registered ($types)"] = $types -eq $expectedTypes
$results['installed app passes --self-check'] = (Start-Process $exe -ArgumentList '--self-check' -Wait -PassThru).ExitCode -eq 0

$product = Get-InstalledProducts | Select-Object -First 1
$types = Invoke-Msiexec '/x' $product 'uninstall'
$results['program folder removed'] = -not (Test-Path $dir)
$results['shortcut removed'] = -not (Test-Path $shortcut)
$results['no installed product'] = (Get-InstalledProducts).Count -eq 0
$results["file types unregistered ($types)"] = $types -eq 'none'

foreach ($check in $results.Keys) { Write-Host ('{0}  {1}' -f $(if ($results[$check]) { 'PASS' } else { 'FAIL' }), $check) }
$failed = @($results.Keys | Where-Object { -not $results[$_] })
if ($failed.Count) { throw "Install test failed: $($failed -join '; ')" }
Write-Host 'Install test passed.'
