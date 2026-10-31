<#
.SYNOPSIS
    Builds FUSERNativeLoader: winhttp.dll, its bundled mods and their paks, and optionally the player package.

.DESCRIPTION
    Output goes to out\<Configuration>\, laid out as it goes into a FUSER install:
      winhttp.dll, winhttp.pdb       the mod loader (CMake, MSVC x64)
      mods\<Name>\                   each bundled mod: mod.ini (from its source folder), <Name>.dll, .pdb
      Paks\mods\<Name>_P.pak, .sig   the cooked assets of the mods that have any (assets\<Name>\Content), packed by
                                     tools\fuser_pak_create.py; they go to Fuser\Content\Paks\mods\
      lib\winhttp.lib                the import library C++ tier mods link (sdk\README.md)
    -Configuration Debug builds the diagnostic DLLs (FNL_DEBUG: a console with the loader's tracing, the F6-F10 debug
    keys and the probes); the paks are the same in both configurations.
    -Package also writes out\FUSERNativeLoader-<version>[-debug].zip, laid out as it goes into a FUSER install, with
    package\README.md and the licence; its symbols go into a separate -symbols.zip, and the SDK for mod authors (sdk\
    and lib\winhttp.lib) into -sdk.zip.

.PARAMETER Configuration
    Release (default) or Debug.
.PARAMETER Package
    Also build the player zip.
.PARAMETER Python
    Python 3 to run the pak tool. Default: py -3, python on PATH, or the newest %ProgramFiles%\Python3*\python.exe.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Configuration Debug
    .\build.ps1 -Package
#>
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$Package,
    [string]$Python
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -TotalCount 1).Trim()
$buildDir = Join-Path $root "build\$Configuration"
$outDir = Join-Path $root "out\$Configuration"

# The bundled mods: each one's source folder (holding its mod.ini); assets\<Name>\Content, when it exists, is its pak
$bundledMods = @(
    @{ Name = 'FUSERBpmRange'; Source = 'src\bpm_range' },
    @{ Name = 'FUSERFifthDeck'; Source = 'src\fifth_deck' },
    @{ Name = 'FUSERFixes'; Source = 'src\fixes' },
    @{ Name = 'FUSERMultiplayer'; Source = 'src\multiplayer' },
    @{ Name = 'FUSERQualityOfLifeImprovements'; Source = 'src\qol' }
)

function Find-Python {
    if ($Python) { return , @($Python) }
    if (Get-Command py -ErrorAction SilentlyContinue) { return , @('py', '-3') }
    if (Get-Command python -ErrorAction SilentlyContinue) { return , @('python') }
    $installed = Get-ChildItem -Path (Join-Path $env:ProgramFiles 'Python3*\python.exe') -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
    if ($installed) { return , @($installed.FullName) }
    throw 'Python 3 not found: pass -Python <path to python.exe>.'
}

function Invoke-Checked([string]$what, [scriptblock]$cmd) {
    & $cmd
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'CMake not found on PATH (Visual Studio ships one: use a Developer PowerShell).' }
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
# outputs of builds before the mods layout (one pak, the multiplayer DLL beside winhttp.dll)
foreach ($old in 'FUSERNativeMods_P.pak', 'FUSERNativeMods_P.sig', 'FUSERMultiplayer.dll', 'FUSERMultiplayer.pdb') {
    $path = Join-Path $outDir $old
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
}

# ---- the DLLs: the diagnostic build differs only by FNL_DEBUG; both are optimised (MSVC Release config)
$debugFlag = if ($Configuration -eq 'Debug') { 'ON' } else { 'OFF' }
Invoke-Checked 'CMake configure' { cmake -S $root -B $buildDir -A x64 "-DFNL_DEBUG=$debugFlag" | Out-Host }
Invoke-Checked 'CMake build' { cmake --build $buildDir --config Release | Out-Host }
foreach ($f in 'winhttp.dll', 'winhttp.pdb') { Copy-Item -LiteralPath (Join-Path $buildDir "Release\$f") -Destination $outDir -Force }
# the import library C++ tier mods link (its FNL_API exports), for the SDK
New-Item -ItemType Directory -Path (Join-Path $outDir 'lib') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildDir 'Release\winhttp.lib') -Destination (Join-Path $outDir 'lib') -Force

# ---- bundled mods: mods\<Name>\ with the DLL, its PDB and mod.ini (the repository's VERSION filled in), and their paks
$py = Find-Python
$paksOut = Join-Path $outDir 'Paks\mods'
New-Item -ItemType Directory -Path $paksOut -Force | Out-Null
foreach ($m in $bundledMods) {
    $modOut = Join-Path $outDir "mods\$($m.Name)"
    New-Item -ItemType Directory -Path $modOut -Force | Out-Null
    foreach ($ext in 'dll', 'pdb') { Copy-Item -LiteralPath (Join-Path $buildDir "Release\$($m.Name).$ext") -Destination $modOut -Force }
    (Get-Content -LiteralPath (Join-Path $root "$($m.Source)\mod.ini") -Raw).Replace('{{VERSION}}', $version) |
        Set-Content -LiteralPath (Join-Path $modOut 'mod.ini') -Encoding ascii -NoNewline
    # every cooked file under assets\<Name>\Content, mounted at ../../../Fuser/Content/
    $content = Join-Path $root "assets\$($m.Name)\Content"
    if (-not (Test-Path -LiteralPath $content)) { continue }
    $files = Get-ChildItem -LiteralPath $content -Recurse -File | Sort-Object FullName | ForEach-Object { $_.FullName } # sorted: a reproducible pak
    if (-not $files) { throw "No cooked assets under $content" }
    $pak = Join-Path $paksOut "$($m.Name)_P.pak"
    $pyArgs = @($py | Select-Object -Skip 1) + @((Join-Path $root 'tools\fuser_pak_create.py'), $pak, '--mount', '../../../Fuser/Content/', '--root', $content) + $files
    Invoke-Checked 'fuser_pak_create.py' { & $py[0] @pyArgs | Out-Host }
}

# ---- player package
if ($Package) {
    $suffix = if ($Configuration -eq 'Debug') { '-debug' } else { '' }
    $name = "FUSERNativeLoader-$version$suffix"
    $stage = Join-Path $root "out\$name"
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    $win64 = Join-Path $stage 'Fuser\Binaries\Win64'
    $paks = Join-Path $stage 'Fuser\Content\Paks\mods'
    New-Item -ItemType Directory -Path $win64, $paks -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $outDir 'winhttp.dll') -Destination $win64
    foreach ($m in $bundledMods) {
        $modStage = Join-Path $win64 "mods\$($m.Name)"
        New-Item -ItemType Directory -Path $modStage -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $outDir "mods\$($m.Name)\$($m.Name).dll"), (Join-Path $outDir "mods\$($m.Name)\mod.ini") -Destination $modStage
    }
    Get-ChildItem -LiteralPath $paksOut -File | Copy-Item -Destination $paks
    Copy-Item -LiteralPath (Join-Path $root 'LICENSE.FuserSongLoader') -Destination $stage
    # FUSERMultiplayer.dll links libarchive (BSD-2) and zlib statically: their notices go with it
    Copy-Item -LiteralPath (Join-Path $root 'lib\libarchive\LICENSE.libarchive'), (Join-Path $root 'lib\libarchive\LICENSE.zlib') -Destination $stage
    (Get-Content -LiteralPath (Join-Path $root 'package\README.md') -Raw).Replace('{{VERSION}}', $version) |
        Set-Content -LiteralPath (Join-Path $stage 'README.md') -Encoding utf8
    $zip = Join-Path $root "out\$name.zip"
    $symZip = Join-Path $root "out\$name-symbols.zip"
    foreach ($z in $zip, $symZip) { if (Test-Path -LiteralPath $z) { Remove-Item -LiteralPath $z -Force } }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
    $pdbs = @(Join-Path $outDir 'winhttp.pdb') + @($bundledMods | ForEach-Object { Join-Path $outDir "mods\$($_.Name)\$($_.Name).pdb" })
    Compress-Archive -LiteralPath $pdbs -DestinationPath $symZip
    # the SDK for mod authors: sdk\ (headers, README, template) and lib\winhttp.lib
    $sdkStage = Join-Path $root "out\$name-sdk"
    $sdkZip = Join-Path $root "out\$name-sdk.zip"
    foreach ($p in $sdkStage, $sdkZip) { if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force } }
    Copy-Item -LiteralPath (Join-Path $root 'sdk') -Destination $sdkStage -Recurse
    Copy-Item -LiteralPath (Join-Path $outDir 'lib') -Destination $sdkStage -Recurse
    Compress-Archive -Path (Join-Path $sdkStage '*') -DestinationPath $sdkZip
    Write-Host "package: $zip (SDK: $sdkZip)"
}

Get-ChildItem -LiteralPath $outDir -File -Recurse | ForEach-Object {
    '{0,-58} {1,10}  {2}' -f $_.FullName.Substring($outDir.Length + 1), $_.Length, (Get-FileHash -LiteralPath $_.FullName).Hash.Substring(0, 12)
}
Write-Host "FUSERNativeLoader $version ($Configuration) -> $outDir"
