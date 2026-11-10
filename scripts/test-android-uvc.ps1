param(
    [string]$Compiler = 'C:/msys64/ucrt64/bin/gcc.exe',
    [string]$ToolsRoot = (Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) 'build-tools')
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$src = Join-Path $repo 'obs-studio/plugins/android-capture'
$out = Join-Path $ToolsRoot 'checks/uvc-regression'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:PATH = (Split-Path $Compiler -Parent) + ';' + $env:PATH
$env:TEMP = $out
$env:TMP = $out
foreach ($case in @('uvc-bulk-pump','uvc-raw-payload','uvc-cadence-stats','uvc-stream-buffer','uvc-recovery')) {
    $arguments = @('-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror',"-I$src")
    if ($case -eq 'uvc-bulk-pump') {
        $arguments += @('-pthread',"-I$src/tests/mock","$src/uvc-bulk-pump.c")
    }
    $arguments += @("$src/tests/$case-test.c",'-o',"$out/$case-test.exe")
    & $Compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw "$case compilation failed" }
    & "$out/$case-test.exe"
    if ($LASTEXITCODE -ne 0) { throw "$case regression failed" }
}
Push-Location $repo
try {
    python scripts/test-uvc-policy.py --compiler $Compiler
    if ($LASTEXITCODE -ne 0) { throw 'UVC policy regression failed' }
    python obs-studio/plugins/android-capture/tests/test-uvc-negotiation.py --compiler $Compiler
    if ($LASTEXITCODE -ne 0) { throw 'UVC negotiation regression failed' }
} finally { Pop-Location }
Write-Host 'PASS: UVC host regressions; USB host/device throughput and latency require hardware tests.'
