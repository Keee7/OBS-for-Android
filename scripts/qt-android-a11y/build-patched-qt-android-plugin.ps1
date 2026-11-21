# Builds the Qt 6 Android platform plugin with the accessibility deadlock fix applied.
#
# The patch only touches two files of qtbase's Android platform plugin, so only the
# plugin target (libplugins_platforms_qtforandroid_<abi>.so) has to be compiled; the
# rest of the Qt Android libraries shipped inside the APK are reused unchanged.
#
# Usage (Windows, PowerShell 7):
#   pwsh -File scripts/qt-android-a11y/build-patched-qt-android-plugin.ps1 `
#     -QtSrc D:\qtbase-everywhere-src-6.11.1 `
#     -BuildDir D:\qtbase-build `
#     -QtHostPath D:\Qt\6.11.1\msvc2022_64 `
#     -Ndk D:\android-ndk-r30 `
#     -AndroidSdk D:\Android\SDK `
#     -OutSo D:\plugin-patched\libplugins_platforms_qtforandroid_arm64-v8a.so
#
# Requirements: CMake >= 3.24, Ninja, NDK r27+ (r30 tested), a host Qt of the exact
# same version for QT_HOST_PATH, and the qtbase sources of the Qt version pinned in
# BUILDING.md (6.11.1).

[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $QtSrc,
    [Parameter(Mandatory)] [string] $BuildDir,
    [Parameter(Mandatory)] [string] $QtHostPath,
    [Parameter(Mandatory)] [string] $Ndk,
    [string] $AndroidSdk = $env:ANDROID_SDK_ROOT,
    [ValidateSet('arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86')] [string] $Abi = 'arm64-v8a',
    [string] $AndroidPlatform = 'android-29',
    [string] $Patch = (Join-Path $PSScriptRoot 'qt-6.11.1-a11y-deadlock.patch'),
    [Parameter(Mandatory)] [string] $OutSo,
    [string] $CMake,
    [string] $Ninja,
    [switch] $SkipPatch,
    [int] $Jobs = 8
)

$ErrorActionPreference = 'Stop'

function Find-Tool([string] $Name, [string[]] $Hints) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($hint in $Hints) {
        if (Test-Path -LiteralPath $hint) { return $hint }
    }
    throw "Cannot locate $Name. Pass its path explicitly or add it to PATH."
}

$cmake = if ($CMake) { $CMake } else { Find-Tool 'cmake' @() }
$ninja = if ($Ninja) { $Ninja } else { Find-Tool 'ninja' @() }

# 1. Apply the patch (idempotent: skip when already applied).
$accessibilityCpp = Join-Path $QtSrc 'src\plugins\platforms\android\androidjniaccessibility.cpp'
if (-not (Test-Path -LiteralPath $accessibilityCpp)) {
    throw "Qt sources not found under $QtSrc (expected src/plugins/platforms/android/)."
}

if (-not $SkipPatch) {
    $alreadyApplied = (Select-String -LiteralPath $accessibilityCpp -Pattern 'Accessibility call to the main thread timed out' -SimpleMatch -Quiet)
    if ($alreadyApplied) {
        Write-Host "[patch] already applied to $QtSrc"
    } else {
        $git = Find-Tool 'git' @()
        Write-Host "[patch] applying $Patch"
        & $git -C $QtSrc apply --whitespace=nowarn $Patch
        if ($LASTEXITCODE -ne 0) { throw "git apply failed ($LASTEXITCODE)" }
    }
}

# 2. Configure once per build directory.
$cache = Join-Path $BuildDir 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cache)) {
    $toolchain = Join-Path $Ndk 'build\cmake\android.toolchain.cmake'
    if (-not (Test-Path -LiteralPath $toolchain)) { throw "NDK toolchain file missing: $toolchain" }
    Write-Host "[configure] $BuildDir ($Abi / $AndroidPlatform)"
    & $cmake -S $QtSrc -B $BuildDir -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
        "-DANDROID_ABI=$Abi" `
        "-DANDROID_PLATFORM=$AndroidPlatform" `
        '-DANDROID_STL=c++_shared' `
        "-DANDROID_SDK_ROOT=$AndroidSdk" `
        "-DQT_HOST_PATH=$QtHostPath" `
        '-DQT_BUILD_TESTS=OFF' `
        '-DQT_BUILD_EXAMPLES=OFF' `
        '-DQT_BUILD_BENCHMARKS=OFF' `
        '-DFEATURE_accessibility=ON'
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
}

# 3. Build the platform plugin only.
$target = "libplugins_platforms_qtforandroid_$Abi.so"
Write-Host "[build] $target"
& $ninja -C $BuildDir -j $Jobs $target
if ($LASTEXITCODE -ne 0) { throw "ninja failed ($LASTEXITCODE)" }

$builtSo = Join-Path $BuildDir "plugins\platforms\libplugins_platforms_qtforandroid_$Abi.so"
if (-not (Test-Path -LiteralPath $builtSo)) { throw "Build output missing: $builtSo" }

# 4. Strip with the NDK's llvm-strip, matching what the APK expects.
$strip = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutSo) | Out-Null
if (Test-Path -LiteralPath $strip) {
    & $strip --strip-unneeded $builtSo -o $OutSo
} else {
    Copy-Item -LiteralPath $builtSo -Destination $OutSo -Force
}

$size = (Get-Item -LiteralPath $OutSo).Length
Write-Host "[ok] $OutSo ($size bytes)"
Write-Host "[next] replace lib/$Abi/libplugins_platforms_qtforandroid_$Abi.so inside the APK,"
Write-Host "       keep resources.arsc uncompressed, then zipalign + apksigner."
