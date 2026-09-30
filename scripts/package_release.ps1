param(
    [string]$Version = "0.1.6"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$wheelName = "project_oreo_runtime-$Version-py3-none-win_amd64.whl"
$wheel = Join-Path $projectRoot "dist\$wheelName"
$releaseRoot = Join-Path $projectRoot "release"
$bundleName = "ProjectOreoRuntime-$Version"
$bundle = Join-Path $releaseRoot $bundleName
$archive = Join-Path $releaseRoot "$bundleName.zip"

if (-not (Test-Path -LiteralPath $wheel)) {
    throw "Runtime wheel is missing: $wheel"
}

New-Item -ItemType Directory -Force -Path $bundle | Out-Null
Copy-Item -LiteralPath $wheel -Destination (Join-Path $bundle $wheelName) -Force
# No uninstaller ships with v1. Removing the Runtime is Project Oreo Launcher's job, and
# shipping a second way to do it means one of them never gets tested.
#
# `Install Oreo Runtime.bat` is out of the repository for now and out of the bundle with it.
# The manual install was always meant to be the second way in, next to the Launcher - it is
# not a leftover - but as written it cannot find the Launcher's private Python. See the v2
# item about the manual batch installer in docs/TODO.md.
Copy-Item -LiteralPath (Join-Path $projectRoot "README.md") -Destination $bundle -Force

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $wheel).Hash.ToLowerInvariant()
"$hash  $wheelName" | Set-Content -LiteralPath (Join-Path $bundle "SHA256SUMS.txt") -Encoding ascii

if (Test-Path -LiteralPath $archive) {
    Remove-Item -LiteralPath $archive -Force
}
Compress-Archive -LiteralPath $bundle -DestinationPath $archive -CompressionLevel Optimal
Write-Host "Release bundle created: $archive"
