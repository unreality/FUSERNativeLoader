<#
.SYNOPSIS
    Installs a build from out\<Configuration>\ into a FUSER install, or restores what was there before.

.DESCRIPTION
    Copies winhttp.dll to Fuser\Binaries\Win64\, each bundled mod (mod.ini and its DLL) to Fuser\Binaries\Win64\mods\<Name>\
    and the mods' paks (<Name>_P.pak / .sig) to Fuser\Content\Paks\mods\. The first install keeps the game's previous
    winhttp.dll (e.g. the stock song loader) as winhttp.dll.fnl-backup; -Restore puts it back and removes the bundled
    mods and their paks. Other mods and paks are left alone. Refuses while the game is running.

.PARAMETER GameRoot
    The folder that contains Fuser\ (default: %FUSER_GAME_ROOT%).
.PARAMETER Configuration
    Release (default) or Debug: which out\ folder to install.
.PARAMETER Restore
    Undo: restore the backed-up winhttp.dll and remove the bundled mods and their paks.

.EXAMPLE
    $env:FUSER_GAME_ROOT = 'D:\Games\FUSER'
    .\install.ps1 -Configuration Debug
    .\install.ps1 -Restore
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$GameRoot = $env:FUSER_GAME_ROOT,
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$Restore
)
$ErrorActionPreference = 'Stop'
if (-not $GameRoot) { throw 'Pass -GameRoot <folder containing Fuser\> or set FUSER_GAME_ROOT.' }
$win64 = Join-Path $GameRoot 'Fuser\Binaries\Win64'
$paks = Join-Path $GameRoot 'Fuser\Content\Paks'
if (-not (Test-Path -LiteralPath $win64) -or -not (Test-Path -LiteralPath $paks)) { throw "No FUSER install under '$GameRoot'." }
if (-not $WhatIfPreference -and (Get-Process -Name 'FuserEOS-Win64-Shipping', 'Fuser-Win64-Shipping' -ErrorAction SilentlyContinue)) { throw 'FUSER is running - close it first.' }

$bundledMods = @('FUSERBpmRange', 'FUSERFifthDeck', 'FUSERFixes', 'FUSERMultiplayer', 'FUSERQualityOfLifeImprovements')
$dll = Join-Path $win64 'winhttp.dll'
$backup = Join-Path $win64 'winhttp.dll.fnl-backup'
$modsDir = Join-Path $win64 'mods'
$modPaks = Join-Path $paks 'mods'
# Builds before the mods layout installed the multiplayer DLL beside winhttp.dll and one pak for every asset
$legacy = @((Join-Path $win64 'FUSERMultiplayer.dll'), (Join-Path $win64 'FUSERMultiplayer.pdb'), (Join-Path $paks 'FUSERNativeMods_P.pak'),
    (Join-Path $paks 'FUSERNativeMods_P.sig'))

function Remove-IfEmpty([string]$dir) {
    if ((Test-Path -LiteralPath $dir) -and -not (Get-ChildItem -LiteralPath $dir -Force) -and $PSCmdlet.ShouldProcess($dir, 'Remove (empty)')) {
        Remove-Item -LiteralPath $dir -Force
    }
}

if ($Restore) {
    if (Test-Path -LiteralPath $backup) {
        if ($PSCmdlet.ShouldProcess($dll, 'Restore from winhttp.dll.fnl-backup')) { Move-Item -LiteralPath $backup -Destination $dll -Force }
    } else {
        Write-Warning 'No winhttp.dll.fnl-backup: winhttp.dll left as it is.'
    }
    foreach ($name in $bundledMods) {
        $modDir = Join-Path $modsDir $name
        if ((Test-Path -LiteralPath $modDir) -and $PSCmdlet.ShouldProcess($modDir, 'Remove')) { Remove-Item -LiteralPath $modDir -Recurse -Force }
        foreach ($ext in 'pak', 'sig') {
            $f = Join-Path $modPaks "$($name)_P.$ext"
            if ((Test-Path -LiteralPath $f) -and $PSCmdlet.ShouldProcess($f, 'Remove')) { Remove-Item -LiteralPath $f -Force }
        }
    }
    Remove-IfEmpty $modsDir
    Remove-IfEmpty $modPaks
    foreach ($f in $legacy) { if ((Test-Path -LiteralPath $f) -and $PSCmdlet.ShouldProcess($f, 'Remove')) { Remove-Item -LiteralPath $f -Force } }
    if (-not $WhatIfPreference) { Write-Host 'Restored.' }
    return
}

$src = Join-Path $PSScriptRoot "out\$Configuration"
$inputs = @(Join-Path $src 'winhttp.dll')
foreach ($name in $bundledMods) { $inputs += @("mods\$name\mod.ini", "mods\$name\$name.dll") | ForEach-Object { Join-Path $src $_ } }
foreach ($f in $inputs) { if (-not (Test-Path -LiteralPath $f)) { throw "Missing $f - run .\build.ps1 -Configuration $Configuration first." } }
$builtPaks = @(Get-ChildItem -LiteralPath (Join-Path $src 'Paks\mods') -File -ErrorAction SilentlyContinue)

# keep the previous winhttp.dll once (a later install over our own build must not overwrite the backup)
if ((Test-Path -LiteralPath $dll) -and -not (Test-Path -LiteralPath $backup)) {
    if ($PSCmdlet.ShouldProcess($dll, 'Back up as winhttp.dll.fnl-backup')) { Copy-Item -LiteralPath $dll -Destination $backup }
}
if ($PSCmdlet.ShouldProcess($dll, "Install $Configuration winhttp.dll")) { Copy-Item -LiteralPath $inputs[0] -Destination $dll -Force }
foreach ($name in $bundledMods) {
    $modDir = Join-Path $modsDir $name
    if ($PSCmdlet.ShouldProcess($modDir, "Install $Configuration $name")) {
        New-Item -ItemType Directory -Path $modDir -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $src "mods\$name\mod.ini"), (Join-Path $src "mods\$name\$name.dll") -Destination $modDir -Force
    }
}
if ($builtPaks -and $PSCmdlet.ShouldProcess($modPaks, "Install $($builtPaks.Count) pak file(s)")) {
    New-Item -ItemType Directory -Path $modPaks -Force | Out-Null
    $builtPaks | Copy-Item -Destination $modPaks -Force
}
foreach ($f in $legacy) { if ((Test-Path -LiteralPath $f) -and $PSCmdlet.ShouldProcess($f, 'Remove')) { Remove-Item -LiteralPath $f -Force } }

if (Test-Path -LiteralPath (Join-Path $win64 'dwmapi.dll')) {
    Write-Warning 'dwmapi.dll (UE4SS) is present: do not run UE4SS alongside FUSERNativeLoader - remove or rename it.'
}
if ($WhatIfPreference) { return }
$same = (Get-FileHash -LiteralPath $inputs[0]).Hash -eq (Get-FileHash -LiteralPath $dll).Hash
Write-Host "Installed $Configuration build to $GameRoot (winhttp.dll hash match: $same)."
