#Requires -Version 5.1
param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",

    [ValidateSet("auto", "msvc", "ninja")]
    [string]$Generator = "auto",

    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $Root "build"
$SdkHeader = Join-Path $Root "tools\scs_sdk\include\scssdk_telemetry.h"

function Test-ScsSdk {
    if (-not (Test-Path -LiteralPath $SdkHeader)) {
        Write-Error @"
SCS Telemetry SDK not found.

Download the SDK from:
  https://modding.scssoft.com/wiki/Documentation/Engine/SDK/Telemetry

Extract it so this file exists:
  $SdkHeader
"@
    }
}

function Test-NinjaAvailable {
    return [bool](Get-Command ninja -ErrorAction SilentlyContinue)
}

function Get-VisualStudioGeneratorArgs {
    param([int]$ProductLineVersion)

    switch ($ProductLineVersion) {
        18 { return @("-G", "Visual Studio 18 2026", "-A", "x64") }
        17 { return @("-G", "Visual Studio 17 2022", "-A", "x64") }
        16 { return @("-G", "Visual Studio 16 2019", "-A", "x64") }
        default {
            if ($ProductLineVersion -ge 2022) {
                return @("-G", "Visual Studio 17 2022", "-A", "x64")
            }
            if ($ProductLineVersion -ge 2019) {
                return @("-G", "Visual Studio 16 2019", "-A", "x64")
            }
        }
    }

    return $null
}

function Get-CmakeGeneratorArgs {
    param([string]$Mode)

    if ($Mode -eq "ninja") {
        if (-not (Test-NinjaAvailable)) {
            Write-Error "Ninja was requested but is not on PATH. Install Ninja or use -Generator msvc with Visual Studio."
        }
        return @("-G", "Ninja")
    }

    $vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $installPath = $null
    $productVersion = $null

    if (Test-Path -LiteralPath $vsWhere) {
        $installPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        $productVersion = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property catalog_productLineVersion 2>$null
    }

    if ($Mode -eq "msvc" -or $Mode -eq "auto") {
        if ($installPath -and $productVersion) {
            $generatorArgs = Get-VisualStudioGeneratorArgs -ProductLineVersion ([int]$productVersion)
            if ($generatorArgs) {
                return $generatorArgs
            }
        }

        if ($Mode -eq "msvc") {
            Write-Error "Visual Studio with C++ tools was not found."
        }
    }

    if (Test-NinjaAvailable) {
        Write-Host "Visual Studio not found; using Ninja. Ensure cl.exe is on PATH (e.g. Developer PowerShell)." -ForegroundColor Yellow
        return @("-G", "Ninja")
    }

    Write-Error @"
No supported Windows build toolchain was found.

Install one of:
  - Visual Studio 2026/2022/2019 with "Desktop development with C++"
  - Visual Studio Build Tools + Ninja (https://ninja-build.org/)

Then rerun:
  .\scripts\build.ps1
  .\scripts\build.ps1 -Generator msvc
  .\scripts\build.ps1 -Generator ninja
"@
}

Test-ScsSdk

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

$generatorArgs = Get-CmakeGeneratorArgs -Mode $Generator
$isMultiConfig = ($generatorArgs -join " ") -match "Visual Studio"

$configureArgs = @(
    "-S", $Root,
    "-B", $BuildDir
) + $generatorArgs

if (-not $isMultiConfig) {
    $configureArgs += @("-DCMAKE_BUILD_TYPE=$Config")
}

Write-Host "Configuring Towhitch ($Config)..." -ForegroundColor Cyan
& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$buildArgs = @(
    "--build", $BuildDir,
    "--parallel"
)

if ($isMultiConfig) {
    $buildArgs += @("--config", $Config)
}

$buildArgs += @("--target", "Towhitch", "dist")

Write-Host "Building Towhitch..." -ForegroundColor Cyan
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$DistDir = Join-Path $Root "dist"
$artifact = Get-ChildItem -LiteralPath $DistDir -File | Select-Object -First 1
if ($artifact) {
    Write-Host ""
    Write-Host "Build complete:" -ForegroundColor Green
    Write-Host "  $($artifact.FullName)"
} else {
    Write-Warning "Build finished but no artifact was found in dist/."
}
