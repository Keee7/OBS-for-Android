<#
.SYNOPSIS
    Host-side regression entry point for the Android capture stack.

.DESCRIPTION
    Runs every host-executable test script in this directory and reports a single
    pass/fail. Device-dependent behaviour (HAL callback timing, MediaCodec,
    MediaProjection tokens, real USB throughput) still requires hardware runs --
    this only covers what can be verified on a build host.

    Requires Windows PowerShell for -File invocation of the child scripts and a
    gcc on PATH (pass -Compiler to override).
#>
param(
    [string]$Compiler = 'C:/msys64/ucrt64/bin/gcc.exe'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$failed = @()
$scripts = @(
    'test-android-reader-ctx.ps1',
    'test-android-uvc.ps1',
    'test-android-screen.ps1',
    'test-android-mediacodec.ps1'
)

foreach ($name in $scripts) {
    $path = Join-Path $PSScriptRoot $name
    if (-not (Test-Path $path)) {
        Write-Host "SKIP $name (missing)"
        continue
    }
    Write-Host ""
    Write-Host "===== $name ====="
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $path)
    if ($name -ne 'test-android-screen.ps1') { $argList += @('-Compiler', $Compiler) }
    & powershell @argList
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL $name (exit $LASTEXITCODE)"
        $failed += $name
    } else {
        Write-Host "OK   $name"
    }
}

Write-Host ""
if ($failed.Count -gt 0) {
    throw ("host test failures: {0}" -f ($failed -join ', '))
}
Write-Host 'PASS: all host-side Android capture regressions.'