# Build the single Synchronus.exe (Release, x64, static CRT).
#
#   .\build.ps1            -> dist\Synchronus.exe
#
# Requires Visual Studio 2019+ or the C++ Build Tools (MSVC + Windows SDK).
# No vcpkg / NuGet dependencies: the tool is Win32 only.
# The PlatformToolset is auto-detected so the same script works on VS2019
# (v142) and VS2022 (v143) machines.

[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$solution = Join-Path $root 'Synchronus\Synchronus.sln'

# --- locate MSBuild -------------------------------------------------------
$msbuild = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    $found = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null | Select-Object -First 1
    if ($found) { $msbuild = $found }
}
if (-not $msbuild) {
    foreach ($year in 2022, 2019) {
        foreach ($edition in 'BuildTools', 'Community', 'Professional', 'Enterprise') {
            $candidate = "C:\Program Files (x86)\Microsoft Visual Studio\$year\$edition\MSBuild\Current\Bin\MSBuild.exe"
            if ($year -eq 2022) { $candidate = "C:\Program Files\Microsoft Visual Studio\2022\$edition\MSBuild\Current\Bin\MSBuild.exe" }
            if (Test-Path $candidate) { $msbuild = $candidate; break }
        }
        if ($msbuild) { break }
    }
}
if (-not $msbuild) {
    throw "MSBuild with the C++ toolset was not found. Install 'Visual Studio Build Tools' with the 'Desktop development with C++' workload from https://visualstudio.microsoft.com/downloads/ and re-run."
}

# --- pick the toolset that matches the installed VS release ----------------
$toolset = $null
if (Test-Path $vswhere) {
    $major = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion 2>$null
    if ($major) {
        switch -Regex ($major) {
            '^17\.' { $toolset = 'v143' }
            '^16\.' { $toolset = 'v142' }
            '^15\.' { $toolset = 'v141' }
        }
    }
}
if (-not $toolset) {
    # vswhere has no registered instance on some BuildTools installs; fall
    # back to the VS year embedded in the MSBuild path we already found.
    switch -Regex ($msbuild) {
        '\\2019\\' { $toolset = 'v142' }
        '\\2022\\' { $toolset = 'v143' }
        '\\2017\\' { $toolset = 'v141' }
        default    { $toolset = 'v143' }  # project default
    }
}

Write-Host "MSBuild : $msbuild"
Write-Host "Toolset : $toolset"
Write-Host "Config  : $Configuration x64"

# Locate the newest installed Windows 10/11 SDK. Some BuildTools installs
# lack the "Installed Roots" registry registration MSBuild normally relies
# on, so pass the SDK location explicitly when we can find one.
$sdkArgs = @()
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkVersion = Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName 'um\Windows.h') } |
    Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
if ($sdkVersion) {
    # MSBuild concatenates these as "$(WindowsSdkDir)Include\...", i.e. it
    # expects a trailing separator. A trailing backslash would corrupt the
    # quoted native command line, so use forward slashes (accepted by both
    # MSBuild's Exists() and cl.exe).
    $sdkRootFwd = ($sdkRoot -replace '\\', '/') + '/'
    $sdkArgs = @("/p:WindowsTargetPlatformVersion=$sdkVersion", "/p:WindowsSDKDir=$sdkRootFwd",
                 "/p:WindowsSdkDir_10=$sdkRootFwd", "/p:UniversalCRTSdkDir_10=$sdkRootFwd",
                 "/p:UCRTContentRoot=$sdkRootFwd")
    Write-Host "SDK     : $sdkVersion ($sdkRoot)"
}

& $msbuild $solution /p:Configuration=$Configuration /p:Platform=x64 /p:PlatformToolset=$toolset @sdkArgs /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

$built = Join-Path $root "Synchronus\x64\$Configuration\Synchronus.exe"
if (-not (Test-Path $built)) { throw "Build reported success but $built was not produced." }

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$target = Join-Path $dist 'Synchronus.exe'
Copy-Item $built $target -Force

$size = [math]::Round((Get-Item $target).Length / 1KB)
Write-Host ""
Write-Host "Built: $target ($size KB)"
Write-Host "Run it (it will ask for admin), or test without changing anything:"
Write-Host "  $target --benchmark"
