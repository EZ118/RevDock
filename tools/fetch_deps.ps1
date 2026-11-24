#requires -Version 7
param(
    [switch]$Force,
    [string]$Proxy = "https://cdn.gh-proxy.org/"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$lib = Join-Path $root "lib"
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) "revdock-deps"
New-Item -ItemType Directory -Force $tmp | Out-Null

$deps = @(
    @{ Name = "SDL3";       Tag = "release-3.4.20"; Repo = "libsdl-org/SDL";       Dir = "SDL3";       Root = "SDL-release-3.4.20" },
    @{ Name = "SDL3_ttf";   Tag = "release-3.2.2";  Repo = "libsdl-org/SDL_ttf";   Dir = "SDL3_ttf";   Root = "SDL_ttf-release-3.2.2" },
    @{ Name = "SDL3_image"; Tag = "release-3.4.8";  Repo = "libsdl-org/SDL_image"; Dir = "SDL3_image"; Root = "SDL_image-release-3.4.8" },
    @{ Name = "freetype";   Tag = "VER-2-14-3";     Repo = "freetype/freetype";    Dir = "freetype";   Root = "freetype-VER-2-14-3" }
)

foreach ($dep in $deps) {
    $target = Join-Path $lib $dep.Dir
    if ((Test-Path $target) -and -not $Force) {
        Write-Host "skip $($dep.Name) (already present)"
        continue
    }
    $archive = Join-Path $tmp "$($dep.Name).tar.gz"
    $url = "${Proxy}https://github.com/$($dep.Repo)/archive/refs/tags/$($dep.Tag).tar.gz"
    Write-Host "download $($dep.Name) $($dep.Tag)"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
    if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    tar -xzf $archive -C $lib
    $extracted = Join-Path $lib $dep.Root
    if (Test-Path $extracted) {
        Move-Item -Force $extracted $target
    } else {
        throw "expected extracted directory $extracted"
    }
}

function Get-Raw($repo, $tag, $path, $out) {
    $parent = Split-Path -Parent $out
    New-Item -ItemType Directory -Force $parent | Out-Null
    Invoke-WebRequest -UseBasicParsing `
        -Uri "${Proxy}https://raw.githubusercontent.com/$repo/$tag/$path" -OutFile $out
}

$nlohmann = Join-Path $lib "nlohmann"
if ((Test-Path $nlohmann) -and -not $Force) {
    Write-Host "skip nlohmann"
} else {
    New-Item -ItemType Directory -Force "$nlohmann/single_include/nlohmann" | Out-Null
    Get-Raw "nlohmann/json" "v3.12.0" "single_include/nlohmann/json.hpp" "$nlohmann/single_include/nlohmann/json.hpp"
    Get-Raw "nlohmann/json" "v3.12.0" "LICENSE.MIT" "$nlohmann/LICENSE.MIT"
    Get-Raw "nlohmann/json" "v3.12.0" "README.md" "$nlohmann/README.md"
}

$doctest = Join-Path $lib "doctest"
if ((Test-Path $doctest) -and -not $Force) {
    Write-Host "skip doctest"
} else {
    New-Item -ItemType Directory -Force $doctest | Out-Null
    Get-Raw "doctest/doctest" "v2.5.3" "doctest/doctest.h" "$doctest/doctest.h"
    Get-Raw "doctest/doctest" "v2.5.3" "LICENSE.txt" "$doctest/LICENSE.txt"
    Get-Raw "doctest/doctest" "v2.5.3" "README.md" "$doctest/README.md"
}

$fonts = Join-Path $root "assets/fonts/Inter"
if (-not (Test-Path "$fonts/Inter-Regular.ttf") -or $Force) {
    $zip = Join-Path $tmp "Inter-4.1.zip"
    if (-not (Test-Path $zip) -or $Force) {
        Write-Host "download Inter 4.1"
        Invoke-WebRequest -UseBasicParsing `
            -Uri "${Proxy}https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip" -OutFile $zip
    }
    $extract = Join-Path $tmp "inter"
    Remove-Item -Recurse -Force $extract -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $extract | Out-Null
    tar -xzf $zip -C $extract `
        "extras/ttf/Inter-Regular.ttf" "extras/ttf/Inter-Medium.ttf" `
        "extras/ttf/Inter-SemiBold.ttf" "LICENSE.txt"
    New-Item -ItemType Directory -Force $fonts | Out-Null
    Copy-Item "$extract/extras/ttf/Inter-Regular.ttf" $fonts -Force
    Copy-Item "$extract/extras/ttf/Inter-Medium.ttf" $fonts -Force
    Copy-Item "$extract/extras/ttf/Inter-SemiBold.ttf" $fonts -Force
    Copy-Item "$extract/LICENSE.txt" "$fonts/OFL.txt" -Force
}

Write-Host "dependencies are in $lib"
Write-Host "next: python tools/trim_deps.py"
