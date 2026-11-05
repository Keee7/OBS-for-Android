param(
    [string]$Clang = 'C:\msys64\clang64\bin\clang++.exe',
    [string]$DepsRoot = (Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) 'build-tools')
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$out = Join-Path $DepsRoot 'checks/objects'
$qt = Join-Path $DepsRoot 'qtbase-android-arm64/include'
$sysroot = Join-Path $DepsRoot 'ndk-headers'
New-Item -ItemType Directory -Force -Path $out | Out-Null
# arm_acle supplies the __yield declaration needed by these Qt headers with
# host MSYS Clang. This check uses real Android headers, not Qt/JNI API stubs.
$argsCommon = @('--target=aarch64-linux-android29', "--sysroot=$sysroot", '-std=c++17',
    '-Wall', '-Wextra', '-Werror', '-include', 'arm_acle.h', "-I$qt", "-I$qt/QtCore",
    "-I$sysroot/usr/include/c++/v1", "-I$DepsRoot/simde", "-I$DepsRoot/checks/include", "-I$repo/obs-studio/libobs")
foreach ($source in @('obs-studio/frontend/widgets/OBSAndroidBrowser.cpp', 'obs-studio/frontend/utility/AndroidCodecCapabilities.cpp')) {
    $object = Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($source) + '.o')
    & $Clang @argsCommon -c (Join-Path $repo $source) -o $object
    if ($LASTEXITCODE -ne 0) { throw "Android Qt JNI compilation failed: $source" }
}
$javac = Join-Path $DepsRoot 'jdk-17.0.12/bin/javac.exe'
& $javac -source 17 -target 17 -Xlint:all -cp (Join-Path $DepsRoot 'android-35/android.jar') `
    -d (Join-Path $DepsRoot 'java-classes') `
    (Join-Path $repo 'obs-studio/frontend/cmake/android/java/com/obsproject/studio/ObsCodecCapabilities.java') `
    (Join-Path $repo 'obs-studio/frontend/cmake/android/java/com/obsproject/studio/ObsBrowserHost.java')
if ($LASTEXITCODE -ne 0) { throw 'Android Java compilation failed' }
Write-Host 'Two AArch64 Qt JNI objects and both Java classes compiled. No APK link or device test.'
