param(
    [ValidateSet('configure','build','package','all')][string]$Action = 'all',
    [string]$ToolsRoot = (Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) 'build-tools'),
    [int]$Jobs = [Environment]::ProcessorCount,
    [string]$NativeDeps,
    [string]$QtVersion = '',
    [string]$GradleVersion = '',
    [int]$CompileSdkVersion = 0,
    [string]$SigningConfigFile = $env:OBS_ANDROID_SIGNING_CONFIG,
    [switch]$DebugSigning,
    [switch]$AntutuPackage,
    [switch]$SkipVerify
)
$ErrorActionPreference = 'Stop'

function Resolve-Tool([string]$Name, [string]$EnvVar, [string[]]$Commands, [string]$FallbackPath, [switch]$Optional) {
    if ($EnvVar) {
        $val = [Environment]::GetEnvironmentVariable($EnvVar)
        if ($val -and (Test-Path -LiteralPath $val -PathType Leaf)) { return [IO.Path]::GetFullPath($val) }
    }
    foreach ($cmd in $Commands) {
        $hit = Get-Command $cmd -All -ErrorAction SilentlyContinue |
            Where-Object { $_.Source -notlike '*\WindowsApps\*' } | Select-Object -First 1
        if ($hit) { return $hit.Source }
    }
    if ($FallbackPath -and (Test-Path -LiteralPath $FallbackPath -PathType Leaf)) {
        return [IO.Path]::GetFullPath($FallbackPath)
    }
    if ($Optional) { return $null }
    throw "Required tool '$Name' not found in environment/PATH or '$FallbackPath'."
}

function Resolve-Dir([string]$Name, [string[]]$EnvVars, [string]$MarkerRel, [string]$FallbackPath) {
    foreach ($v in $EnvVars) {
        $val = [Environment]::GetEnvironmentVariable($v)
        if ($val -and (Test-Path -LiteralPath (Join-Path $val $MarkerRel))) { return [IO.Path]::GetFullPath($val) }
    }
    if ($FallbackPath -and (Test-Path -LiteralPath (Join-Path $FallbackPath $MarkerRel))) {
        return [IO.Path]::GetFullPath($FallbackPath)
    }
    throw "Required directory '$Name' not found in environment or '$FallbackPath'."
}

$toolchain = @{}
foreach ($line in (Get-Content -LiteralPath "$PSScriptRoot/toolchain-versions.env")) {
    $trimmed = $line.Trim()
    if (!$trimmed -or $trimmed.StartsWith('#')) { continue }
    $pair = $trimmed -split '=', 2
    if ($pair.Count -eq 2) { $toolchain[$pair[0].Trim()] = $pair[1].Trim() }
}
if (!$QtVersion) { $QtVersion = $toolchain['QT_VERSION'] }
if (!$GradleVersion) { $GradleVersion = $toolchain['GRADLE_VERSION'] }
if ($CompileSdkVersion -le 0) { $CompileSdkVersion = [int]$toolchain['COMPILE_SDK_VERSION'] }
$NdkVersion = $toolchain['NDK_VERSION']
$BuildToolsVersion = $toolchain['BUILD_TOOLS_VERSION']
$JdkVersion = $toolchain['JDK_VERSION']
if ($QtVersion -notmatch '^\d+\.\d+\.\d+$') { throw "Invalid QtVersion: $QtVersion" }
if ($GradleVersion -notmatch '^\d+\.\d+\.\d+$') { throw "Invalid GradleVersion: $GradleVersion" }
if ($CompileSdkVersion -lt 36) { throw "Invalid CompileSdkVersion: $CompileSdkVersion" }

$repo = Split-Path $PSScriptRoot -Parent
$tools = [IO.Path]::GetFullPath($ToolsRoot)
$build = Join-Path $tools ('build-obs33-arm64-qt' + $QtVersion.Replace('.', ''))
$logs = Join-Path $tools 'build-logs'

$sdk = Resolve-Dir 'Android SDK' @('ANDROID_SDK_ROOT','ANDROID_HOME') "platforms/android-$CompileSdkVersion/android.jar" (Join-Path $tools 'android-sdk')

$ndkMajor = $NdkVersion -replace '^[rR]', ''
$sdkNdkDir = Get-ChildItem -Path (Join-Path $sdk 'ndk') -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'source.properties') } |
    Sort-Object @{ Expression = { $_.Name.StartsWith("$ndkMajor.") }; Descending = $true }, Name -Descending |
    Select-Object -First 1
$ndk = Resolve-Dir "Android NDK ($NdkVersion)" @('ANDROID_NDK_ROOT','ANDROID_NDK_HOME') 'source.properties' $(if ($sdkNdkDir) { $sdkNdkDir.FullName } else { '' })

$jdkDir = Get-ChildItem -Path $tools -Directory -Filter "jdk-$JdkVersion*" -ErrorAction SilentlyContinue |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'bin/javac.exe') } |
    Sort-Object Name -Descending | Select-Object -First 1
$javaHome = Resolve-Dir "JDK $JdkVersion" @('JAVA_HOME') 'bin/javac.exe' $(if ($jdkDir) { $jdkDir.FullName } else { '' })

$cmakeDir = Get-ChildItem -Path (Join-Path $tools 'cmake') -Directory -Filter 'cmake-*' -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
$cmakeFallback = if ($cmakeDir) { Join-Path $cmakeDir.FullName 'bin/cmake.exe' } else { '' }
$cmake = Resolve-Tool 'CMake' '' @('cmake') $cmakeFallback
$ninja = Resolve-Tool 'Ninja' '' @('ninja') (Join-Path $tools 'ninja/ninja.exe')
$gradle = Resolve-Tool 'Gradle' '' @('gradle') (Join-Path $tools "gradle/gradle-$GradleVersion/bin/gradle.bat")
$python = Resolve-Tool 'Python' 'PYTHON' @('python','py') (Join-Path $tools 'python/python.exe')
$ccache = Resolve-Tool 'ccache' '' @('ccache') (Join-Path $tools 'ccache/ccache.exe') -Optional

$qt = Join-Path $tools "Qt/$QtVersion/android_arm64_v8a"
$hostQt = Join-Path $tools "Qt/$QtVersion/msvc2022_64"
$prefix = if ($NativeDeps) { [IO.Path]::GetFullPath($NativeDeps) } else { Join-Path $tools 'android-deps-ffmpeg8/arm64-v8a' }

$env:JAVA_HOME = $javaHome
$env:ANDROID_SDK_ROOT = $sdk
$env:ANDROID_HOME = $sdk
$env:ANDROID_NDK_ROOT = $ndk
$env:ANDROID_NDK_HOME = $ndk
$env:ANDROID_USER_HOME = Join-Path $tools 'android-user-home'
$env:GRADLE_USER_HOME = Join-Path $tools 'gradle-cache'
# Qt invokes Gradle internally. A persistent daemon can retain this script's
# redirected output handle after a successful build and block the pipeline.
$env:GRADLE_OPTS = ($env:GRADLE_OPTS + ' -Dorg.gradle.daemon=false').Trim()
$env:PATH = "$(Split-Path $gradle -Parent);$javaHome/bin;$hostQt/bin;$(Split-Path $ninja -Parent);$env:PATH"
New-Item -ItemType Directory -Force -Path $build,$logs,$env:ANDROID_USER_HOME | Out-Null

$packageId = if ($AntutuPackage) { 'com.antutu.ABenchMark' } else { 'com.obsproject.studio' }
$packageSource = Join-Path $build 'android-package-source'
$sourcePackage = Join-Path $repo 'obs-studio/frontend/cmake/android'
$buildFullPath = [IO.Path]::GetFullPath($build).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$expectedPackageSource = [IO.Path]::GetFullPath((Join-Path $buildFullPath 'android-package-source'))
$packageSource = [IO.Path]::GetFullPath($packageSource)
if ($packageSource -ne $expectedPackageSource -or
    !$packageSource.StartsWith($buildFullPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace package staging outside the expected build directory: $packageSource"
}
if (Test-Path -LiteralPath $packageSource) { Remove-Item -LiteralPath $packageSource -Recurse -Force }
New-Item -ItemType Directory -Force -Path $packageSource | Out-Null
Get-ChildItem -LiteralPath $sourcePackage -Force | Where-Object Name -ne 'libs' |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $packageSource -Recurse -Force }
$manifestPath = Join-Path $packageSource 'AndroidManifest.xml'
$manifestText = [IO.File]::ReadAllText($manifestPath)
$manifestText = ([regex]'(?<=\bpackage=")[^"]+(?=")').Replace($manifestText, $packageId, 1)
[IO.File]::WriteAllText($manifestPath, $manifestText, [Text.UTF8Encoding]::new($false))
$servicePath = Join-Path $packageSource 'java/com/obsproject/studio/ObsForegroundService.java'
$serviceText = [IO.File]::ReadAllText($servicePath)
$serviceText = ([regex]'import com\.(?:obsproject\.studio|antutu\.ABenchMark)\.R;').Replace($serviceText, "import $packageId.R;", 1)
[IO.File]::WriteAllText($servicePath, $serviceText, [Text.UTF8Encoding]::new($false))

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

if ($ccache) {
    if (!$env:CCACHE_DIR) { $env:CCACHE_DIR = Join-Path $tools 'ccache-cache' }
    if (!$env:CCACHE_MAXSIZE) { $env:CCACHE_MAXSIZE = '2G' }
    if (!$env:CCACHE_SLOPPINESS) { $env:CCACHE_SLOPPINESS = 'pch_defines,time_macros,locale' }
    New-Item -ItemType Directory -Force -Path $env:CCACHE_DIR | Out-Null
}

$needConfigure = $Action -in @('configure','all') -or !(Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt'))
if ($needConfigure) {
    $argsConfigure = @('-S', "$repo/obs-studio", '-B', $build, '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake",
        '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-29', '-DANDROID_STL=c++_shared',
        '-DCMAKE_BUILD_TYPE=RelWithDebInfo', "-DCMAKE_PREFIX_PATH=$prefix;$qt", "-DQT_HOST_PATH=$hostQt",
        '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DENABLE_PLUGINS=ON', '-DENABLE_FRONTEND=ON',
        '-DENABLE_SCRIPTING=OFF', '-DENABLE_HEVC=ON', "-DANDROID_SDK_ROOT=$sdk", "-DCMAKE_INSTALL_PREFIX=$build/install")
    $argsConfigure += "-DOBS_ANDROID_COMPILE_SDK_VERSION=$CompileSdkVersion"
    $argsConfigure += "-DOBS_ANDROID_PACKAGE_NAME=$packageId", "-DOBS_ANDROID_PACKAGE_SOURCE_DIR=$($packageSource.Replace('\','/'))"
    if ($ccache) {
        $ccachePath = $ccache.Replace('\', '/')
        $argsConfigure += "-DCMAKE_C_COMPILER_LAUNCHER=$ccachePath", "-DCMAKE_CXX_COMPILER_LAUNCHER=$ccachePath"
        Write-Host "Using ccache: $ccachePath (CCACHE_DIR=$env:CCACHE_DIR)"
    }
    Invoke-BuildTool $cmake $argsConfigure 'configure.log'
}
if ($Action -in @('build','package','all')) {
    Invoke-BuildTool $cmake @('--build', $build, '--parallel', "$Jobs") 'native-build.log'
    if ($ccache -and $Action -in @('build','all')) { & $ccache -s | Write-Host }
}
if ($Action -in @('package','all')) {
    $modules = @(Get-ChildItem -Path "$build/rundir" -Recurse -File -Filter '*.so' |
        Where-Object { $_.DirectoryName -match 'obs-plugins$' })
    foreach ($required in @('android-mediacodec.so','android-browser.so')) {
        if ($required -notin $modules.Name) { throw "Built plugin missing: $required" }
    }
    # 插件走 assets：运行时由 AndroidRuntimeBootstrap 解到 $OBS_ROOT_PATH/lib。
    # （APK 的 lib/<abi>/ 只解 lib*.so，我们的插件名不带 lib 前缀，解不出来。）
    $packageAssets = Join-Path $packageSource 'assets/obs-plugins'
    if (Test-Path -LiteralPath $packageAssets) { Remove-Item -LiteralPath $packageAssets -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $packageAssets | Out-Null
    foreach ($module in $modules) { Copy-Item -LiteralPath $module.FullName -Destination $packageAssets -Force }
    $stripExe = Join-Path $ndk 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe'
    if (Test-Path -LiteralPath $stripExe) {
        foreach ($module in (Get-ChildItem -LiteralPath $packageAssets -File -Filter '*.so')) {
            & $stripExe --strip-unneeded $module.FullName 2>$null | Out-Null
        }
    }
    Invoke-BuildTool $python @("$PSScriptRoot/android-plugin-bundle.py", 'generate', $packageAssets) 'plugin-bundle.log'
    $androidBuild = Join-Path $build 'frontend/android-build'
    $deploy = Join-Path $hostQt 'bin/androiddeployqt.exe'
    $unsigned = Join-Path $androidBuild 'obs-studio-test.apk'
    Invoke-BuildTool $deploy @('--input', "$build/frontend/android-obs-studio-deployment-settings.json",
        '--output', $androidBuild, '--apk', $unsigned, '--android-platform', "android-$CompileSdkVersion", '--release', '--aux-mode') 'android-stage.log'
    Invoke-BuildTool $python @("$PSScriptRoot/stage-qt-gradle-properties.py",
        '--deployment-settings', "$build/frontend/android-obs-studio-deployment-settings.json",
        '--output', $androidBuild, '--android-platform', "android-$CompileSdkVersion",
        '--sdk', $sdk, '--build-tools', $BuildToolsVersion) 'gradle-props.log'
    # Patch the staged runtime only. A second androiddeployqt invocation would
    # overwrite it with the pristine SDK jar, so build directly with Gradle.
    $patchedQtJar = Join-Path $build 'qt-ime-patched/Qt6Android.jar'
    Invoke-BuildTool $python @("$PSScriptRoot/qt-android-ime/patch-qt-android-jar.py",
        '--qt-version', $QtVersion, '--jar', "$qt/jar/Qt6Android.jar",
        '--qtbase-src', "$build/_deps/obs_qtbase_android-src",
        '--android-jar', "$sdk/platforms/android-$CompileSdkVersion/android.jar",
        '--javac', "$javaHome/bin/javac.exe", '--output', $patchedQtJar) 'qt-ime-runtime.log'
    Copy-Item -LiteralPath $patchedQtJar -Destination "$androidBuild/libs/Qt6Android.jar" -Force
    Copy-Item -LiteralPath "$build/qt-android-platform/libplugins_platforms_qtforandroid_arm64-v8a.so" `
        -Destination "$androidBuild/libs/arm64-v8a/libplugins_platforms_qtforandroid_arm64-v8a.so" -Force
    $gradleArgs = @('--no-daemon', '-p', $androidBuild, 'assembleRelease')
    if ($SkipVerify) { $gradleArgs += @('-x', 'lintVitalRelease') }
    Invoke-BuildTool $gradle $gradleArgs 'android-package.log'
    $apk = Get-ChildItem -Path $androidBuild -Recurse -File -Filter '*unsigned.apk' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (!$apk) { $apk = Get-Item -LiteralPath $unsigned -ErrorAction Stop }
    $releases = Join-Path $repo 'releases'
    New-Item -ItemType Directory -Force -Path $releases | Out-Null
    $aligned = Join-Path $build 'obs-test-aligned.apk'
    $modeName = if ($AntutuPackage) { 'antutu' } else { 'obs' }
    $signed = Join-Path $releases ('OBS-Android-arm64-' + $modeName + '-hwcodec-browser-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.apk')
    Invoke-BuildTool "$sdk/build-tools/$BuildToolsVersion/zipalign.exe" @('-f', '-P', '16', '4', $apk.FullName, $aligned) 'zipalign.log'
    if ($DebugSigning) {
        Invoke-BuildTool "$sdk/build-tools/$BuildToolsVersion/apksigner.bat" @('sign','--ks',"$PSScriptRoot/signing/debug.keystore",
            '--ks-key-alias','androiddebugkey','--ks-pass','pass:android','--key-pass','pass:android','--out',$signed,$aligned) 'sign.log'
    } else {
        if (!$SigningConfigFile) { $SigningConfigFile = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'release key.txt' }
        if (!(Test-Path -LiteralPath $SigningConfigFile)) { throw 'Release signing configuration missing; pass -SigningConfigFile or explicitly choose -DebugSigning.' }
        Invoke-BuildTool $python @("$PSScriptRoot/sign-android-apk.py", '--config', $SigningConfigFile,
            '--input', $aligned, '--output', $signed, '--tools-root', $tools) 'sign.log'
    }
    if (!$SkipVerify) {
        Invoke-BuildTool "$sdk/build-tools/$BuildToolsVersion/apksigner.bat" @('verify','--verbose',$signed) 'verify-signature.log'
        Invoke-BuildTool "$sdk/build-tools/$BuildToolsVersion/zipalign.exe" @('-c','-P','16','-v','4',$signed) 'verify-alignment.log'
        Invoke-BuildTool $python @("$PSScriptRoot/verify-android-apk.py", $signed, '--tools-root', $tools,
            '--native-dir', $build, '--expected-package', $packageId) 'strict-apk-check.log'
        Invoke-BuildTool $python @("$PSScriptRoot/qt-android-ime/verify-runtime-apk.py", $signed) 'qt-runtime-apk-check.log'
        Invoke-BuildTool $python @("$PSScriptRoot/android-plugin-bundle.py", 'verify', $signed) 'plugin-bundle-apk.log'
    }
    Get-FileHash -Algorithm SHA256 -LiteralPath $signed
    Write-Host "Test APK: $signed"
}
