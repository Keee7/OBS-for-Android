param(
    [string]$Compiler = 'C:/msys64/ucrt64/bin/gcc.exe',
    [string]$ToolsRoot = (Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) 'build-tools')
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$common = Join-Path $repo 'obs-studio/plugins/android-common'
$out = Join-Path $ToolsRoot 'checks/reader-ctx'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:PATH = (Split-Path $Compiler -Parent) + ';' + $env:PATH
$env:TEMP = $out
$env:TMP = $out

# 1) host-side state machine regression (tests/mock replaces libobs)
$arguments = @('-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-pthread',
    "-I$common/tests/mock","-I$common",
    "$common/tests/reader-ctx-test.c",'-o',"$out/reader-ctx-test.exe")
& $Compiler @arguments
if ($LASTEXITCODE -ne 0) { throw 'reader-ctx-test compilation failed' }
& "$out/reader-ctx-test.exe"
if ($LASTEXITCODE -ne 0) { throw 'reader-ctx regression failed' }

# 2) source-order invariants that a unit test cannot catch
function Assert-SourceOrder {
    param([string]$Path, [string]$First, [string]$Second, [string]$Why)
    $text = [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
    $a = $text.IndexOf($First)
    $b = $text.IndexOf($Second)
    if ($a -lt 0) { throw ("{0}: anchor not found: {1}" -f $Path, $First) }
    if ($b -lt 0) { throw ("{0}: anchor not found: {1}" -f $Path, $Second) }
    if ($a -gt $b) { throw ("{0}: wrong order -- {1}" -f $Path, $Why) }
}

$camera = Join-Path $repo 'obs-studio/plugins/android-camera/camera2-input.c'
$screen = Join-Path $repo 'obs-studio/plugins/android-screen/screen-capture.c'

# ctx must be created after AImageReader_new: otherwise early failures before the
# reader exists make close_capture bail out, leaving the fresh ctx un-retired, and
# the next open's wait_idle() then passes unconditionally (gate disabled).
Assert-SourceOrder -Path $camera `
    -First 'AImageReader_new' `
    -Second 'struct reader_ctx *fresh = reader_ctx_create(c->source);' `
    -Why 'ctx must be created after the reader exists (camera)'
Assert-SourceOrder -Path $screen `
    -First 'AImageReader_new' `
    -Second 'struct reader_ctx *fresh = reader_ctx_create(c->source);' `
    -Why 'ctx must be created after the reader exists (screen)'

Write-Host 'PASS: reader-ctx host regression and source-order invariants.'

# ---------------------------------------------------------------------------
# 3) ctx 生命周期契约：不允许存在 reader_ctx_destroy()。
#
# reader-ctx.h 顶部写明 ctx「不随 close 释放」，因为框架可能"已决定派发、尚未进入
# 函数体"，那一段不在 users 计数里。一旦有了 destroy()，destroy 路径上
# wait_idle()==true 就会被误当成"可以安全释放"，把 UAF 从 close 挪到 destroy。
# 这条断言防止它被加回来。
# ---------------------------------------------------------------------------
$rcHeader = Join-Path $common 'reader-ctx.h'
$rcText = [IO.File]::ReadAllText($rcHeader, [Text.Encoding]::UTF8)
if ($rcText -match 'reader_ctx_destroy') {
    throw 'reader-ctx.h must not define reader_ctx_destroy(): ctx has to outlive the capture struct (see header comment lines 25-27)'
}
foreach ($plugin in @($camera, $screen)) {
    $pText = [IO.File]::ReadAllText($plugin, [Text.Encoding]::UTF8)
    if ($pText -match 'reader_ctx_destroy') {
        throw ("{0}: must not call reader_ctx_destroy()" -f $plugin)
    }
}

Write-Host 'PASS: ctx lifetime contract (no reader_ctx_destroy).'
