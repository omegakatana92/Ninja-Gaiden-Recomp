# make_release.ps1 - build the ROM-free release package.
#
# The package holds everything a player needs to build and play the game from
# their own legally obtained ROM: the launcher, the framework's runtime
# sources, the host recompiler, the game's configuration and the build
# scripts.  It holds no ROM, no ROM-derived C source and no ROM-derived
# binary.
#
#   powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 `
#       [-Output release] [-Staging <dir>]

param(
    [string]$Output = "",
    [string]$Staging = ""
)

$ErrorActionPreference = "Stop"
$project = Split-Path -Parent $PSScriptRoot
if (-not $Output) { $Output = Join-Path $project "release" }
if (-not $Staging) { $Staging = Join-Path $project "release\staging" }

$engine = ""
foreach ($candidate in @((Join-Path $project "engine"), "C:\Users\omega\Documents\tools\nesrecomp-engine")) {
    if (Test-Path -LiteralPath (Join-Path $candidate "runner\cyc\cyc.cmake")) { $engine = $candidate; break }
}
if (-not $engine) { throw "The NESRecomp engine was not found." }

$packageName = "NinjaGaiden-Windows-ROM-Free"
$root = Join-Path $Staging $packageName
if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
New-Item -ItemType Directory -Path $root -Force | Out-Null

function Copy-Tree($from, $to, [string[]]$ExcludeDirs = @()) {
    New-Item -ItemType Directory -Path $to -Force | Out-Null
    Get-ChildItem -LiteralPath $from -Recurse -File | ForEach-Object {
        $relative = $_.FullName.Substring($from.Length).TrimStart('\')
        $skip = $false
        foreach ($part in $relative.Split('\')) { if ($ExcludeDirs -contains $part) { $skip = $true; break } }
        if ($skip) { return }
        $target = Join-Path $to $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $target -Force
    }
}

Write-Host "The engine:      $engine"
Write-Host "The package:     $root"

# The launcher and the SDL2 runtime it shares with the game.
$release = Join-Path $project "build\Release"
Copy-Item -LiteralPath (Join-Path $release "NinjaGaidenLauncher.exe") -Destination $root -Force
foreach ($dll in @("SDL2.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll",
                   "msvcp140_2.dll", "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll", "concrt140.dll")) {
    $path = Join-Path $release $dll
    if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $root -Force }
}

# The host recompiler and the game's own files.
New-Item -ItemType Directory -Path (Join-Path $root "tools") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $root "docs") -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $project "tools\NESRecomp.exe") -Destination (Join-Path $root "tools\NESRecomp.exe") -Force
Copy-Item -LiteralPath (Join-Path $project "game.toml") -Destination $root -Force
Copy-Item -LiteralPath (Join-Path $project "CMakeLists.txt") -Destination $root -Force
Copy-Item -LiteralPath (Join-Path $project "docs\route_play.txt") -Destination (Join-Path $root "docs\route_play.txt") -Force
# The seed file starts empty: the launcher fills it with the addresses the
# game reaches when it plays the recorded route.  Nothing ROM-derived is
# shipped.
New-Item -ItemType Directory -Path (Join-Path $root "seeds") -Force | Out-Null
@(
    "# Ninja Gaiden (USA) - cycle seed file.",
    "#",
    "# The addresses the program reached while the recorded route was played.",
    "# The launcher fills this file after the first build and rebuilds with it,",
    "# so that those addresses are compiled natively instead of interpreted.",
    "# Format:  4k:BB:AAAA count"
) | Set-Content -LiteralPath (Join-Path $root "seeds\cyc_seeds.txt") -Encoding ASCII
foreach ($doc in @("README.md", "LICENSE", "THIRD-PARTY-NOTICES.md")) {
    $path = Join-Path $project $doc
    if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $root -Force }
}

# The framework: the runtime the game is compiled against.
$engineTarget = Join-Path $root "engine"
Copy-Tree (Join-Path $engine "runner\cyc")   (Join-Path $engineTarget "runner\cyc")   @("oracle", "build")
Copy-Tree (Join-Path $engine "runner\include") (Join-Path $engineTarget "runner\include")
Copy-Tree (Join-Path $engine "runner\src")   (Join-Path $engineTarget "runner\src")
Copy-Tree (Join-Path $engine "common")       (Join-Path $engineTarget "common")
$sdlTarget = Join-Path $engineTarget "runner\external\SDL2"
Copy-Tree (Join-Path $engine "runner\external\SDL2\cmake")  (Join-Path $sdlTarget "cmake")
Copy-Tree (Join-Path $engine "runner\external\SDL2\include") (Join-Path $sdlTarget "include")
Copy-Tree (Join-Path $engine "runner\external\SDL2\lib")    (Join-Path $sdlTarget "lib")
foreach ($file in @("SDL2.dll", "COPYING.txt", "README-SDL.txt")) {
    $path = Join-Path $engine "runner\external\SDL2\$file"
    if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $sdlTarget -Force }
}
Copy-Item -LiteralPath (Join-Path $engine "LICENSE") -Destination (Join-Path $engineTarget "LICENSE") -Force

# The release must not carry a ROM or anything derived from one.
$romFiles = Get-ChildItem -LiteralPath $root -Recurse -File |
    Where-Object { $_.Extension -in @(".nes", ".fds", ".unf", ".unif", ".nsf") }
if ($romFiles) {
    throw "The package must not contain ROM images: $($romFiles[0].FullName)"
}
$codegen = Get-ChildItem -LiteralPath $root -Recurse -File |
    Where-Object { $_.Name -like "game_cyc*" -or $_.Name -like "*_full.c" }
if ($codegen) {
    throw "The package must not contain ROM-derived generated source: $($codegen[0].FullName)"
}

New-Item -ItemType Directory -Path $Output -Force | Out-Null
$zip = Join-Path $Output "$packageName.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path $root -DestinationPath $zip -CompressionLevel Optimal
$size = (Get-Item -LiteralPath $zip).Length
Write-Host "The package: $zip ($([math]::Round($size / 1MB, 2)) MB)"
Write-Host "Done."
