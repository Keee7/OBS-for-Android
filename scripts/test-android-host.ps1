<#
.SYNOPSIS
    Run Android host regressions on Windows.

.EXAMPLE
    .\scripts\test-android-host.ps1 -Suite Uvc
#>
param(
    [ValidateSet('All', 'ReaderCtx', 'Uvc', 'Screen', 'MediaCodec', 'Egl')]
    [string]$Suite = 'All',
    [string]$Compiler = 'C:/msys64/ucrt64/bin/gcc.exe',
    [string]$Clang = 'C:/msys64/clang64/bin/clang.exe',
    [string]$ToolsRoot = (Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) 'build-tools')
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$obs = Join-Path $repo 'obs-studio'
$plugins = Join-Path $obs 'plugins'

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "$Program exited with status $LASTEXITCODE"
    }
}

function Invoke-CTest([string]$Program, [string]$Source, [string[]]$Options = @()) {
    $binary = Join-Path $output ([IO.Path]::GetFileNameWithoutExtension($Source) + '.exe')
    Invoke-Checked $Program (@('-std=c11', '-Wall', '-Wextra', '-Werror') + $Options + @($Source, '-o', $binary))
    Invoke-Checked $binary @()
}

function Assert-SourceOrder([string]$File, [string]$Text, [string[]]$Anchors) {
    $previous = -1
    foreach ($anchor in $Anchors) {
        $offset = $Text.IndexOf($anchor, [StringComparison]::Ordinal)
        if ($offset -lt $previous -or $offset -lt 0) {
            throw "$($File): missing or out-of-order source anchor '$anchor'"
        }
        $previous = $offset
    }
}

function Test-ReaderContext {
    $common = Join-Path $plugins 'android-common'
    Invoke-CTest $Compiler "$common/tests/reader-ctx-test.c" @(
        '-D_POSIX_C_SOURCE=200809L', '-pthread', "-I$common/tests/mock", "-I$common")

    # Creating the context before the reader can strand it on an early return.
    foreach ($relative in @('android-camera/camera2-input.c', 'android-screen/screen-capture.c')) {
        $file = Join-Path $plugins $relative
        $text = Get-Content -LiteralPath $file -Raw -Encoding UTF8
        Assert-SourceOrder $file $text @('AImageReader_new', 'struct reader_ctx *fresh = reader_ctx_create(c->source);')
        if ($text -match 'reader_ctx_destroy') { throw "$file must not call reader_ctx_destroy()" }
    }
    $header = Join-Path $common 'reader-ctx.h'
    if ((Get-Content -LiteralPath $header -Raw -Encoding UTF8) -match 'reader_ctx_destroy') {
        throw 'reader-ctx.h must not define reader_ctx_destroy()'
    }
}

function Test-ScreenCapture {
    $java = Join-Path $ToolsRoot 'jdk-17.0.12/bin'
    $session = Join-Path $obs 'frontend/cmake/android/java/com/obsproject/studio/ObsProjectionSession.java'
    $screen = Join-Path $plugins 'android-screen'
    Invoke-Checked "$java/javac.exe" @('-encoding', 'UTF-8', '--release', '8', '-Xlint:all', '-d', $output,
        $session, "$PSScriptRoot/android-screen/ObsProjectionSessionTest.java")
    Invoke-Checked "$java/java.exe" @('-ea', '-cp', $output, 'com.obsproject.studio.ObsProjectionSessionTest')
    Invoke-CTest $Clang "$PSScriptRoot/android-screen/capture-size-test.c" @("-I$screen")
    Invoke-CTest $Clang "$screen/tests/capture-alpha-test.c"
}

function Test-MediaCodec {
    $codec = Join-Path $plugins 'android-mediacodec'
    Invoke-CTest $Compiler "$codec/tests/stability-gate-test.c"
    $file = Join-Path $codec 'android-mediacodec.c'
    $text = Get-Content -LiteralPath $file -Raw -Encoding UTF8

    Assert-SourceOrder $file $text @(
        'mc_shed_decide(pending_now, e->degraded, soft_limit)',
        'gs_android_encoder_surface_submit(e->surface, t->tex[0], ns)')
    Assert-SourceOrder $file $text @(
        'if (shed) {', 'mc_flow_stalled(&e->flow, shed_now, NO_OUTPUT_NS)',
        'take_packet(e, packet, received, false)', 'return shed_result;')

    if ($text -notmatch 'if \(e->max_pending > MAX_QUEUED\)') {
        throw 'android-mediacodec.c: max_pending must be capped at MAX_QUEUED'
    }
    if ($text -match 'max_pending - STABILITY_HEADROOM') {
        throw 'android-mediacodec.c: the soft limit must not be derived from max_pending'
    }
    if (-not $text.Contains('#include "shed-policy.h"')) {
        throw 'android-mediacodec.c: shed-policy.h not included'
    }
}

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
$output = [IO.Path]::GetFullPath((Join-Path $tempRoot "obs-android-host-$([Guid]::NewGuid())"))
if (-not $output.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Host test directory must be inside the system temporary directory'
}
New-Item -ItemType Directory -Path $output | Out-Null
$previousPath = $env:PATH
$previousTemp = $env:TEMP
$previousTmp = $env:TMP

try {
    $env:PATH = "$(Split-Path $Compiler -Parent);$previousPath"
    $env:TEMP = $output
    $env:TMP = $output
    $suites = if ($Suite -eq 'All') { @('ReaderCtx', 'Uvc', 'Screen', 'MediaCodec', 'Egl') } else { @($Suite) }
    $failed = foreach ($name in $suites) {
        Write-Host "===== $name ====="
        try {
            switch ($name) {
                'ReaderCtx' { Test-ReaderContext }
                'Uvc' { Invoke-Checked 'python' @("$PSScriptRoot/test_android_host.py", 'uvc', '--compiler', $Compiler) }
                'Screen' { Test-ScreenCapture }
                'MediaCodec' { Test-MediaCodec }
                'Egl' { Invoke-CTest $Compiler "$obs/libobs-opengl/tests/android-egl-retry-test.c" }
            }
        } catch {
            Write-Host "FAIL $($name): $($_.Exception.Message)"
            $name
        }
    }
    if ($failed) { throw "Host regression failures: $($failed -join ', ')" }
    Write-Host "PASS: $($suites -join ', ')"
} finally {
    $env:PATH = $previousPath
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
    Remove-Item -LiteralPath $output -Recurse -Force
}
