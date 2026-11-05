param(
    [string]$Clang = 'C:\msys64\clang64\bin\clang.exe',
    [string]$DepsRoot = (Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) 'build-tools')
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$sysroot = Join-Path $DepsRoot 'ndk-headers'
$simde = Join-Path $DepsRoot 'simde'
$uthash = Join-Path $DepsRoot 'uthash\src'
$jansson = Join-Path $DepsRoot 'jansson-2.14\src'
$mbedtls = Join-Path $DepsRoot 'mbedtls\include'
$config = Join-Path $DepsRoot 'checks\include'
$out = Join-Path $DepsRoot 'checks\objects'
$obs = Join-Path $repo 'obs-studio\libobs'
$caption = Join-Path $repo 'obs-studio\deps\libcaption'
$gles = Join-Path $repo 'obs-studio\deps\glad-gles\include'
# CMake normally generates this header. These values model this Android check,
# rather than the complete APK build configuration.
New-Item -ItemType Directory -Force -Path $config | Out-Null
$configHeader = Join-Path $config 'obsconfig.h'
if (!(Test-Path -LiteralPath $configHeader)) {
    @(
        '#pragma once',
        '#define OBS_DATA_PATH "share/obs"',
        '#define OBS_PLUGIN_PATH "lib/obs-plugins"',
        '#define OBS_PLUGIN_DESTINATION "lib/obs-plugins"',
        '#define OBS_RELEASE_CANDIDATE 0',
        '#define OBS_BETA 0'
    ) | Set-Content -Encoding ascii -LiteralPath $configHeader
}
foreach ($required in @($Clang, (Join-Path $sysroot 'usr\include\media\NdkMediaCodec.h'),
        (Join-Path $simde 'simde\x86\sse2.h'), (Join-Path $uthash 'uthash.h'),
        (Join-Path $config 'obsconfig.h'), (Join-Path $caption 'caption\caption.h'),
        (Join-Path $jansson 'jansson_config.h'), (Join-Path $mbedtls 'mbedtls\ssl.h'))) {
    if (!(Test-Path -LiteralPath $required)) { throw "Missing required dependency: $required" }
}
New-Item -ItemType Directory -Force -Path $out | Out-Null
$common = @('--target=aarch64-linux-android29', "--sysroot=$sysroot", '-std=gnu11', '-Wall', '-Wextra', '-Werror',
    '-D__ANDROID__', '-DOS_ANDROID', "-I$simde", "-I$uthash", "-I$config", "-I$obs", "-I$caption", "-I$gles")
$common += @("-I$DepsRoot/jansson-2.14/src", "-I$DepsRoot/mbedtls/include",
    "-I$repo/obs-studio/shared/happy-eyeballs", "-I$repo/obs-studio/shared/opts-parser", "-I$repo/obs-studio/shared/bpm")
$files = @(
    @{ Source = 'obs-studio\libobs\obs-video-gpu-encode.c'; Object = 'obs-video-gpu-encode.o' },
    @{ Source = 'obs-studio\libobs\obs-video.c'; Object = 'obs-video.o' },
    @{ Source = 'obs-studio\libobs\obs.c'; Object = 'obs.o' },
    @{ Source = 'obs-studio\libobs\obs-encoder.c'; Object = 'obs-encoder.o' },
    @{ Source = 'obs-studio\libobs\obs-output.c'; Object = 'obs-output.o'; ExistingMissingFieldsWarning = $true },
    @{ Source = 'obs-studio\libobs\obs-android.c'; Object = 'obs-android.o' },
    @{ Source = 'obs-studio\libobs\graphics\graphics.c'; Object = 'graphics.o'; ExistingUnusedWarning = $true },
    @{ Source = 'obs-studio\libobs\graphics\graphics-imports.c'; Object = 'graphics-imports.o' },
    @{ Source = 'obs-studio\libobs-opengl\gl-android-interop.c'; Object = 'gl-android-interop.o' },
    @{ Source = 'obs-studio\plugins\android-mediacodec\android-mediacodec.c'; Object = 'android-mediacodec.o' },
    @{ Source = 'obs-studio\plugins\android-browser\browser-source.c'; Object = 'android-browser.o' },
    @{ Source = 'obs-studio\plugins\obs-outputs\mp4-output.c'; Object = 'mp4-output.o' },
    @{ Source = 'obs-studio\plugins\obs-outputs\flv-output.c'; Object = 'flv-output.o' },
    @{ Source = 'obs-studio\plugins\obs-outputs\null-output.c'; Object = 'null-output.o' },
    @{ Source = 'obs-studio\plugins\obs-outputs\rtmp-stream.c'; Object = 'rtmp-stream.o'; Crypto = $true },
    @{ Source = 'obs-studio\plugins\obs-encode-sink\encode-sink.c'; Object = 'encode-sink.o' }
)
foreach ($item in $files) {
    Write-Host "Compiling $($item.Source)"
    $warnings = @()
    if ($item.Crypto) { $warnings += @('-DUSE_MBEDTLS', '-DCRYPTO') }
    # The unmodified upstream file already contains unused build_sprite_norm.
    # Keep the warning visible while treating all other warnings as errors.
    if ($item.ExistingUnusedWarning) { $warnings += '-Wno-error=unused-function' }
    # Existing upstream audio_convert_info positional initializer omits optional fields.
    if ($item.ExistingMissingFieldsWarning) { $warnings += '-Wno-error=missing-field-initializers' }
    & $Clang @common @warnings -c (Join-Path $repo $item.Source) -o (Join-Path $out $item.Object)
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $($item.Source)" }
}
Write-Host "Android AArch64 object compilation passed. Objects: $out"
Write-Host 'This does not link an APK or replace Android device tests.'
