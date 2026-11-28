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

function Resolve-Tool([string]$Name, [string]$EnvVar, [string[]]$Commands, [string]$FallbackPath, [switch]$Optional, [switch]$PreferFallback) {
    if ($EnvVar) {
        $val = [Environment]::GetEnvironmentVariable($EnvVar)
        if ($val -and (Test-Path -LiteralPath $val -PathType Leaf)) { return [IO.Path]::GetFullPath($val) }
    }
    if ($PreferFallback -and $FallbackPath -and (Test-Path -LiteralPath $FallbackPath -PathType Leaf)) {
        return [IO.Path]::GetFullPath($FallbackPath)
    }
    foreach ($cmd in $Commands) {
        $hit = Get-Command $cmd -CommandType Application -All -ErrorAction SilentlyContinue |
            Where-Object { $_.Source -and $_.Source -notlike '*\WindowsApps\*' } | Select-Object -First 1
        if ($hit) { return $hit.Source }
    }
    if ($FallbackPath -and (Test-Path -LiteralPath $FallbackPath -PathType Leaf)) {
        return [IO.Path]::GetFullPath($FallbackPath)
    }
    if ($Optional) { return $null }
    throw "Required tool '$Name' not found in environment/PATH or '$FallbackPath'."
}

function Resolve-Dir([string]$Name, [string[]]$EnvVars, [string[]]$MarkerRels, [string]$FallbackPath) {
    $hasAll = {
        param([string]$Base)
        if (!$Base) { return $false }
        foreach ($rel in $MarkerRels) {
            if (!(Test-Path -LiteralPath (Join-Path $Base $rel))) { return $false }
        }
        return $true
    }
    foreach ($v in $EnvVars) {
        $val = [Environment]::GetEnvironmentVariable($v)
        if (& $hasAll $val) { return [IO.Path]::GetFullPath($val) }
    }
    if (& $hasAll $FallbackPath) {
        return [IO.Path]::GetFullPath($FallbackPath)
    }
    throw "Required directory '$Name' not found in environment or '$FallbackPath'."
}

function Get-VersionSortKey([string]$Text) {
    return [regex]::Replace($Text, '\d+', { param($m) $m.Value.PadLeft(10, '0') })
}

function Normalize-CachePath([string]$Value) {
    return ($Value.Trim() -replace '\\', '/').TrimEnd('/')
}

function Test-NdkMatchesMajor([string]$Dir, [string]$Major) {
    if (!$Dir) { return $false }
    $props = Join-Path $Dir 'source.properties'
    if (!(Test-Path -LiteralPath $props -PathType Leaf)) { return $false }
    if (!$Major) { return $true }
    $revLine = Get-Content -LiteralPath $props -ErrorAction SilentlyContinue |
        Where-Object { $_ -match '^Pkg\.Revision\s*=' } | Select-Object -First 1
    if (!$revLine) { return $false }
    $rev = ($revLine -split '=', 2)[1].Trim()
    return $rev.StartsWith("$Major.")
}

function Test-JavaHomeCompatible([string]$Dir, [int[]]$AllowedMajors) {
    if (!$Dir -or !(Test-Path -LiteralPath (Join-Path $Dir 'bin/javac.exe') -PathType Leaf)) { return $false }
    $releaseFile = Join-Path $Dir 'release'
    if (!(Test-Path -LiteralPath $releaseFile -PathType Leaf)) { return $true }
    $verLine = Get-Content -LiteralPath $releaseFile -ErrorAction SilentlyContinue |
        Where-Object { $_ -match '^JAVA_VERSION="(\d+)' } | Select-Object -First 1
    return ($verLine -match '^JAVA_VERSION="(\d+)' -and ([int]$Matches[1] -in $AllowedMajors))
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

$sdk = Resolve-Dir 'Android SDK' @('ANDROID_SDK_ROOT','ANDROID_HOME') @(
    "platforms/android-$CompileSdkVersion/android.jar",
    "build-tools/$BuildToolsVersion/zipalign.exe"
) (Join-Path $tools 'android-sdk')

$ndkMajorMatch = [regex]::Match($NdkVersion, '^\D*(\d+)')
$ndkMajor = if ($ndkMajorMatch.Success) { $ndkMajorMatch.Groups[1].Value } else { '' }
$ndk = $null
foreach ($envVar in @('ANDROID_NDK_ROOT','ANDROID_NDK_HOME')) {
    $candidate = [Environment]::GetEnvironmentVariable($envVar)
    if (Test-NdkMatchesMajor $candidate $ndkMajor) {
        $ndk = [IO.Path]::GetFullPath($candidate)
        break
    }
}
if (!$ndk) {
    $sdkNdkDir = Get-ChildItem -Path (Join-Path $sdk 'ndk') -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-NdkMatchesMajor $_.FullName $ndkMajor } |
        Sort-Object { Get-VersionSortKey $_.Name } -Descending | Select-Object -First 1
    if ($sdkNdkDir) { $ndk = [IO.Path]::GetFullPath($sdkNdkDir.FullName) }
}
if (!$ndk) {
    $legacyNdk = Join-Path $tools "ndk/android-ndk-$NdkVersion"
    if (Test-NdkMatchesMajor $legacyNdk $ndkMajor) { $ndk = [IO.Path]::GetFullPath($legacyNdk) }
}
if (!$ndk) {
    throw "Required directory 'Android NDK ($NdkVersion)' not found in ANDROID_NDK_ROOT/ANDROID_NDK_HOME, '$sdk/ndk/$ndkMajor.*', or '$tools/ndk/android-ndk-$NdkVersion'."
}

$allowedJdkMajors = @([int]$JdkVersion, 17)
$javaHome = $null
if (Test-JavaHomeCompatible $env:JAVA_HOME $allowedJdkMajors) {
    $javaHome = [IO.Path]::GetFullPath($env:JAVA_HOME)
}
if (!$javaHome) {
    $jdkDir = @("jdk-$JdkVersion*", 'jdk-17*') | ForEach-Object {
        Get-ChildItem -Path $tools -Directory -Filter $_ -ErrorAction SilentlyContinue |
            Where-Object { Test-JavaHomeCompatible $_.FullName $allowedJdkMajors } |
            Sort-Object { Get-VersionSortKey $_.Name } -Descending | Select-Object -First 1
    } | Where-Object { $_ } | Select-Object -First 1
    if ($jdkDir) { $javaHome = [IO.Path]::GetFullPath($jdkDir.FullName) }
}
if (!$javaHome) {
    throw "Required directory 'JDK $JdkVersion' not found in compatible JAVA_HOME or '$tools/jdk-$JdkVersion*'."
}

$cmakeDir = Get-ChildItem -Path (Join-Path $tools 'cmake') -Directory -Filter 'cmake-*' -ErrorAction SilentlyContinue |
    Sort-Object { Get-VersionSortKey $_.Name } -Descending | Select-Object -First 1
$cmakeFallback = if ($cmakeDir) { Join-Path $cmakeDir.FullName 'bin/cmake.exe' } else { '' }
$cmake = Resolve-Tool 'CMake' '' @('cmake') $cmakeFallback -PreferFallback
$ninja = Resolve-Tool 'Ninja' '' @('ninja') (Join-Path $tools 'ninja/ninja.exe')
$gradle = Resolve-Tool 'Gradle' '' @('gradle') (Join-Path $tools "gradle/gradle-$GradleVersion/bin/gradle.bat") -PreferFallback
$python = Resolve-Tool 'Python' 'PYTHON' @('python') (Join-Path $tools 'python/python.exe') -Optional
if (!$python) { $python = Resolve-Tool 'Python' '' @('py') '' }
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
if ($env:GRADLE_OPTS -notmatch '(^|\s)-Dorg\.gradle\.daemon=false(\s|$)') {
    $env:GRADLE_OPTS = ($env:GRADLE_OPTS + ' -Dorg.gradle.daemon=false').Trim()
}
$pathEntries = @((Split-Path $gradle -Parent), "$javaHome/bin", "$hostQt/bin", (Split-Path $ninja -Parent))
$existingPath = @($env:PATH -split ';' | Where-Object { $_ })
$newPath = @()
foreach ($p in ($pathEntries + $existingPath)) {
    if ($p -and ($newPath -notcontains $p)) { $newPath += $p }
}
$env:PATH = $newPath -join ';'
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
# Ensure AGP/Gradle can locate android.ndkVersion under $sdk/ndk/<revision>
# when $ndk comes from ANDROID_NDK_ROOT or legacy build-tools/ndk/android-ndk-*.
$revisionLine = Get-Content -LiteralPath "$ndk/source.properties" | Where-Object { $_ -match '^Pkg\.Revision\s*=' } | Select-Object -First 1
if ($revisionLine) {
    $revision = ($revisionLine -split '=', 2)[1].Trim()
    $sdkNdk = [IO.Path]::GetFullPath((Join-Path $sdk "ndk/$revision"))
    if ($sdkNdk -ne $ndk -and !(Test-Path -LiteralPath $sdkNdk)) {
        $staleLink = Get-Item -LiteralPath $sdkNdk -Force -ErrorAction SilentlyContinue
        if ($staleLink) { $staleLink.Delete() }
        New-Item -ItemType Directory -Force -Path (Split-Path $sdkNdk -Parent) | Out-Null
        New-Item -ItemType Junction -Path $sdkNdk -Target $ndk | Out-Null
    }
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

if ($ccache) {
    if (!$env:CCACHE_DIR) { $env:CCACHE_DIR = Join-Path $tools 'ccache-cache' }
    if (!$env:CCACHE_MAXSIZE) { $env:CCACHE_MAXSIZE = '2G' }
    if (!$env:CCACHE_SLOPPINESS) { $env:CCACHE_SLOPPINESS = 'pch_defines,time_macros,locale' }
    New-Item -ItemType Directory -Force -Path $env:CCACHE_DIR | Out-Null
}

$cacheFile = Join-Path $build 'CMakeCache.txt'
$expectedCcache = if ($ccache) { Normalize-CachePath $ccache } else { '' }
$needConfigure = $Action -in @('configure','all') -or !(Test-Path -LiteralPath $cacheFile)
if (!$needConfigure) {
    $cacheText = [IO.File]::ReadAllText($cacheFile)
    $expectedCache = @{
        'OBS_ANDROID_PACKAGE_NAME' = $packageId
        'OBS_ANDROID_COMPILE_SDK_VERSION' = "$CompileSdkVersion"
        'CMAKE_PREFIX_PATH' = "$(Normalize-CachePath $prefix);$(Normalize-CachePath $qt)"
        'CMAKE_TOOLCHAIN_FILE' = Normalize-CachePath "$ndk/build/cmake/android.toolchain.cmake"
        'ANDROID_SDK_ROOT' = Normalize-CachePath $sdk
        'CMAKE_MAKE_PROGRAM' = Normalize-CachePath $ninja
        'CMAKE_C_COMPILER_LAUNCHER' = $expectedCcache
        'CMAKE_CXX_COMPILER_LAUNCHER' = $expectedCcache
    }
    foreach ($entry in $expectedCache.GetEnumerator()) {
        $pattern = "(?m)^$([regex]::Escape($entry.Key)):[^=]*=(.*)$"
        $m = [regex]::Match($cacheText, $pattern)
        $actual = if ($m.Success) { Normalize-CachePath $m.Groups[1].Value } else { '' }
        if ($actual -ne $entry.Value) {
            $needConfigure = $true
            break
        }
    }
}
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
    } else {
        $argsConfigure += '-UCMAKE_C_COMPILER_LAUNCHER', '-UCMAKE_CXX_COMPILER_LAUNCHER'
    }
    Invoke-BuildTool $cmake $argsConfigure 'configure.log'
}
if ($Action -in @('build','package','all')) {
    Invoke-BuildTool $cmake @('--build', $build, '--parallel', "$Jobs") 'native-build.log'
    if ($ccache -and $Action -in @('build','all')) { & $ccache -s | Write-Host }
}
if ($Action -in @('package','all')) {
    $unsignedApkPathFile = Join-Path $build '.unsigned-apk-path'
    $packageArgs = @(
        "$PSScriptRoot/package-android-apk.py",
        '--build-dir', $build,
        '--package-source', $packageSource,
        '--qt-version', $QtVersion,
        '--qt-android-dir', $qt,
        '--qt-host-dir', $hostQt,
        '--sdk', $sdk,
        '--ndk', $ndk,
        '--java-home', $javaHome,
        '--gradle', $gradle,
        '--android-platform', "android-$CompileSdkVersion",
        '--build-tools', $BuildToolsVersion,
        '--unsigned-apk-out', $unsignedApkPathFile
    )
    if ($SkipVerify) { $packageArgs += '--skip-lint' }
    Invoke-BuildTool $python $packageArgs 'android-package.log'
    $unsignedApk = (Get-Content -LiteralPath $unsignedApkPathFile -Raw).Trim()
    if (!$unsignedApk -or !(Test-Path -LiteralPath $unsignedApk -PathType Leaf)) {
        throw "Unsigned APK missing: $unsignedApk"
    }
    $releases = Join-Path $repo 'releases'
    New-Item -ItemType Directory -Force -Path $releases | Out-Null
    $aligned = Join-Path $build 'obs-test-aligned.apk'
    $modeName = if ($AntutuPackage) { 'antutu' } else { 'obs' }
    $signed = Join-Path $releases ('OBS-Android-arm64-' + $modeName + '-hwcodec-browser-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.apk')
    Invoke-BuildTool "$sdk/build-tools/$BuildToolsVersion/zipalign.exe" @('-f', '-P', '16', '4', $unsignedApk, $aligned) 'zipalign.log'
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
            '--native-dir', $build, '--expected-package', $packageId,
            '--aapt', "$sdk/build-tools/$BuildToolsVersion/aapt.exe",
            '--readelf', "$ndk/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe") 'strict-apk-check.log'
        Invoke-BuildTool $python @("$PSScriptRoot/qt-android-ime/verify-runtime-apk.py", $signed) 'qt-runtime-apk-check.log'
        Invoke-BuildTool $python @("$PSScriptRoot/android-plugin-bundle.py", 'verify', $signed) 'plugin-bundle-apk.log'
    }
    Get-FileHash -Algorithm SHA256 -LiteralPath $signed
    Write-Host "Test APK: $signed"
}
