param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$preset = if ($Config -eq "Debug") { "windows-mingw-debug" } else { "windows-mingw-release" }

Push-Location $root
try {
    cmake --preset $preset
    cmake --build --preset $preset
    ctest --preset $preset --output-on-failure
    $exe = Join-Path $root "build/$preset/bin/revdock.exe"
    if (Test-Path $exe) {
        & $exe @args
    }
} finally {
    Pop-Location
}
