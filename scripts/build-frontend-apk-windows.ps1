param(
    [ValidateSet('configure','build','package','all')][string]$Action = 'all',
    [string]$ToolsRoot = (Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) 'build-tools'),
    [int]$Jobs = 8,
    [string]$SigningConfigFile = $env:OBS_ANDROID_SIGNING_CONFIG,
    [switch]$DebugSigning
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$tools = [IO.Path]::GetFullPath($ToolsRoot)
$build = Join-Path $tools 'build-obs-arm64'
$logs = Join-Path $tools 'build-logs'
$cmake = Join-Path $tools 'cmake/cmake-3.31.6-windows-x86_64/bin/cmake.exe'
$ninja = Join-Path $tools 'ninja/ninja.exe'
$ndk = Join-Path $tools 'ndk/android-ndk-r30'
$qt = Join-Path $tools 'Qt/6.9.3/android_arm64_v8a'
$hostQt = Join-Path $tools 'Qt/6.9.3/msvc2022_64'
$prefix = Join-Path $tools 'android-deps/arm64-v8a'
$sdk = Join-Path $tools 'android-sdk'
$env:JAVA_HOME = Join-Path $tools 'jdk-17.0.12'
$env:ANDROID_SDK_ROOT = $sdk
$env:ANDROID_HOME = $sdk
$env:ANDROID_NDK_ROOT = $ndk
$env:ANDROID_NDK_HOME = $ndk
$env:ANDROID_USER_HOME = Join-Path $tools 'android-user-home'
$env:GRADLE_USER_HOME = Join-Path $tools 'gradle-cache'
# Qt invokes Gradle internally. A persistent daemon can retain this script's
# redirected output handle after a successful build and block the pipeline.
$env:GRADLE_OPTS = ($env:GRADLE_OPTS + ' -Dorg.gradle.daemon=false').Trim()
$env:PATH = "$tools/gradle/gradle-8.14.3/bin;$env:JAVA_HOME/bin;$hostQt/bin;$tools/ninja;$env:PATH"
New-Item -ItemType Directory -Force -Path $build,$logs,$env:ANDROID_USER_HOME | Out-Null
$revision = ((Get-Content -LiteralPath "$ndk/source.properties" | Where-Object { $_ -match '^Pkg.Revision\s*=' }) -split '=',2)[1].Trim()
$sdkNdk = Join-Path $sdk "ndk/$revision"
if (!(Test-Path -LiteralPath $sdkNdk)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $sdkNdk -Parent) | Out-Null
    New-Item -ItemType Junction -Path $sdkNdk -Target $ndk | Out-Null
}
function Invoke-BuildTool([string]$Program, [string[]]$Arguments, [string]$LogName) {
    # Gradle daemons can retain inherited log handles after a successful build.
    $log = Join-Path $logs ([IO.Path]::GetFileNameWithoutExtension($LogName) + '-' +
        (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + $PID + '.log')
    Write-Host "Running $LogName; log: $log"
    # Windows PowerShell treats native stderr warnings as PowerShell errors.
    # The tool's exit status, rather than the presence of stderr, decides failure.
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Program @Arguments 2>&1 | Out-File -Encoding utf8 -LiteralPath $log
        $toolExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    if ($toolExit -ne 0) {
        Get-Content -LiteralPath $log -Tail 65 | Write-Host
        throw "$LogName failed ($toolExit)"
    }
    Get-Content -LiteralPath $log -Tail 8 | Write-Host
}
if ($Action -in @('configure','all')) {
    $argsConfigure = @('-S', "$repo/obs-studio", '-B', $build, '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake",
        '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-29', '-DANDROID_STL=c++_shared',
        '-DCMAKE_BUILD_TYPE=RelWithDebInfo', "-DCMAKE_PREFIX_PATH=$prefix;$qt", "-DQT_HOST_PATH=$hostQt",
        '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DENABLE_PLUGINS=ON', '-DENABLE_FRONTEND=ON',
        '-DENABLE_SCRIPTING=OFF', '-DENABLE_HEVC=ON', "-DANDROID_SDK_ROOT=$sdk", "-DCMAKE_INSTALL_PREFIX=$build/install")
    Invoke-BuildTool $cmake $argsConfigure 'configure.log'
}
if ($Action -in @('build','package','all')) {
    Invoke-BuildTool $cmake @('--build', $build, '--parallel', "$Jobs") 'native-build.log'
}
if ($Action -in @('package','all')) {
    $modules = @(Get-ChildItem -Path "$build/rundir" -Recurse -File -Filter '*.so' |
        Where-Object { $_.DirectoryName -match 'obs-plugins$' })
    foreach ($required in @('android-mediacodec.so','android-browser.so')) {
        if ($required -notin $modules.Name) { throw "Built plugin missing: $required" }
    }
    $packageSources = Join-Path $repo 'obs-studio/frontend/cmake/android/libs/arm64-v8a'
    New-Item -ItemType Directory -Force -Path $packageSources | Out-Null
    foreach ($module in $modules) { Copy-Item -LiteralPath $module.FullName -Destination $packageSources -Force }
    $androidBuild = Join-Path $build 'frontend/android-build'
    $deploy = Join-Path $hostQt 'bin/androiddeployqt.exe'
    $unsigned = Join-Path $androidBuild 'obs-studio-test.apk'
    Invoke-BuildTool $deploy @('--input', "$build/frontend/android-obs-studio-deployment-settings.json",
        '--output', $androidBuild, '--apk', $unsigned, '--android-platform', 'android-35', '--release') 'android-package.log'
    $apk = Get-ChildItem -Path $androidBuild -Recurse -File -Filter '*unsigned.apk' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (!$apk) { $apk = Get-Item -LiteralPath $unsigned -ErrorAction Stop }
    $releases = Join-Path $repo 'releases'
    New-Item -ItemType Directory -Force -Path $releases | Out-Null
    $aligned = Join-Path $build 'obs-test-aligned.apk'
    $signed = Join-Path $releases ('OBS-Android-arm64-hwcodec-browser-' + (Get-Date -Format 'yyyyMMdd-HHmm') + '.apk')
    Invoke-BuildTool "$sdk/build-tools/36.0.0/zipalign.exe" @('-f', '-P', '16', '4', $apk.FullName, $aligned) 'zipalign.log'
    if ($DebugSigning) {
        Invoke-BuildTool "$sdk/build-tools/36.0.0/apksigner.bat" @('sign','--ks',"$repo/android-shell/debug.keystore",
            '--ks-key-alias','androiddebugkey','--ks-pass','pass:android','--key-pass','pass:android','--out',$signed,$aligned) 'sign.log'
    } else {
        if (!$SigningConfigFile) { $SigningConfigFile = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'release key.txt' }
        if (!(Test-Path -LiteralPath $SigningConfigFile)) { throw 'Release signing configuration missing; pass -SigningConfigFile or explicitly choose -DebugSigning.' }
        Invoke-BuildTool 'python' @("$PSScriptRoot/sign-android-apk.py", '--config', $SigningConfigFile,
            '--input', $aligned, '--output', $signed, '--tools-root', $tools) 'sign.log'
    }
    Invoke-BuildTool "$sdk/build-tools/36.0.0/apksigner.bat" @('verify','--verbose',$signed) 'verify-signature.log'
    Invoke-BuildTool "$sdk/build-tools/36.0.0/zipalign.exe" @('-c','-P','16','-v','4',$signed) 'verify-alignment.log'
    Get-FileHash -Algorithm SHA256 -LiteralPath $signed
    Write-Host "Test APK: $signed"
}
