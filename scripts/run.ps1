# Builds (unless -NoBuild) and starts the app, optionally opening files:
#   scripts\run.ps1 -NoBuild C:\Videos\clip.mp4
# Paths are never taken as the configuration: that is only set by name (-Configuration Debug).
[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$NoBuild,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Files = @()
)
. "$PSScriptRoot\common.ps1"

if (-not $NoBuild) { & "$PSScriptRoot\build.ps1" -Configuration $Configuration }
$exe = Join-Path (Get-AppOutputDir $Configuration) 'Syna.exe'
if (-not (Test-Path $exe)) { throw "App not built: $exe" }
$quoted = @($Files | ForEach-Object { '"' + (Resolve-Path $_).Path + '"' })
if ($quoted.Count) { Start-Process $exe -ArgumentList $quoted } else { Start-Process $exe }
