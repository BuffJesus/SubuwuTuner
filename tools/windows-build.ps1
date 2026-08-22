# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The SubuwuTuner Authors

<#
.SYNOPSIS
Configure, build, and test SubuwuTuner with one coherent Visual Studio toolset.

.DESCRIPTION
The public CMake presets intentionally contain no machine-specific Visual
Studio paths. Running them from an ordinary PowerShell can let clang-cl, the
MSVC headers, and the linker libraries come from different installations.
This launcher discovers Visual Studio, enters one VsDevCmd environment, and
runs every requested phase inside that same environment.
#>

[CmdletBinding()]
param(
    [ValidateSet('msvc', 'clang')]
    [string]$Compiler = 'msvc',

    [ValidateSet('configure', 'build', 'test', 'all')]
    [string]$Action = 'all',

    # MSVC major.minor toolset, for example 14.44. Empty selects VS's default.
    [string]$Toolset = '',

    [string]$BuildDirectory = ''
)

$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'vswhere.exe was not found. Install Visual Studio 2022 Build Tools with C++ support.'
}

$requirements = @('Microsoft.VisualStudio.Component.VC.Tools.x86.x64')
if ($Compiler -eq 'clang') {
    $requirements += 'Microsoft.VisualStudio.Component.VC.Llvm.Clang'
}
$installation = (& $vswhere -latest -products * -requires $requirements -property installationPath).Trim()
if ([string]::IsNullOrWhiteSpace($installation)) {
    throw "No Visual Studio installation with the x64 $Compiler toolchain was found."
}

$vsDevCmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path -LiteralPath $vsDevCmd)) {
    throw "VsDevCmd.bat was not found under $installation."
}

if ([string]::IsNullOrWhiteSpace($Toolset)) {
    $versionFile = Join-Path $installation 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
    $fullVersion = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    if ($fullVersion -notmatch '^(\d+\.\d+)') {
        throw "Could not parse the default MSVC toolset version '$fullVersion'."
    }
    $Toolset = $Matches[1]
}

$preset = if ($Compiler -eq 'clang') { 'win-clang' } else { 'win-msvc' }
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path 'build' "$preset-coherent-$Toolset"
}
$absoluteBuild = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\$BuildDirectory"))

$phases = [System.Collections.Generic.List[string]]::new()
if ($Action -in @('configure', 'all')) {
    $phases.Add("cmake --preset $preset -B `"$absoluteBuild`"")
}
if ($Action -in @('build', 'all')) {
    $phases.Add("cmake --build `"$absoluteBuild`" --parallel")
}
if ($Action -in @('test', 'all')) {
    $phases.Add("ctest --test-dir `"$absoluteBuild`" --output-on-failure")
}

$joinedPhases = $phases -join ' && '
$command = "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 -vcvars_ver=$Toolset >nul && $joinedPhases"
Write-Host "Visual Studio: $installation"
Write-Host "Toolset:      $Toolset"
Write-Host "Compiler:     $Compiler"
Write-Host "Build dir:    $absoluteBuild"

& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Windows $Compiler $Action failed with exit code $LASTEXITCODE."
}
