param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$nativeOutput = Join-Path $projectRoot "build\runtime\$Configuration"
$packageBin = Join-Path $projectRoot "src\oreo_runtime\bin"
$packageLicenses = Join-Path $projectRoot "src\oreo_runtime\licenses"

$required = @(
    "oreo_overlay.dll",
    "oreo_controller_support.exe",
    "SDL2.dll"
)

New-Item -ItemType Directory -Force -Path $packageBin | Out-Null
foreach ($name in $required) {
    $source = Join-Path $nativeOutput $name
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Required native artifact is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $packageBin $name) -Force
}

$licenseSources = @{
    "DearImGui.txt" = "native\overlay\third_party\imgui\LICENSE.txt"
    "MinHook.txt" = "native\overlay\third_party\minhook\LICENSE.txt"
    "SDL2.txt" = "native\controller_support\third_party\SDL2\LICENSE.txt"
    "Vulkan-Headers.md" = "native\overlay\third_party\Vulkan-Headers\LICENSE.md"
    "Apache-2.0.txt" = "native\overlay\third_party\Vulkan-Headers\LICENSES\Apache-2.0.txt"
    "Vulkan-Headers-MIT.txt" = "native\overlay\third_party\Vulkan-Headers\LICENSES\MIT.txt"
}

New-Item -ItemType Directory -Force -Path $packageLicenses | Out-Null
foreach ($entry in $licenseSources.GetEnumerator()) {
    $source = Join-Path $projectRoot $entry.Value
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Required license is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $packageLicenses $entry.Key) -Force
}

Write-Host "Runtime binaries and licenses staged in src\oreo_runtime"
