param(
    [string]$Compiler = 'C:/msys64/ucrt64/bin/gcc.exe',
    [string]$ToolsRoot = (Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) 'build-tools')
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$src = Join-Path $repo 'obs-studio/plugins/android-mediacodec'
$out = Join-Path $ToolsRoot 'checks/mediacodec'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:PATH = (Split-Path $Compiler -Parent) + ';' + $env:PATH

# The shedding policy itself lives in shed-policy.h (no NDK headers needed), so
# the host test compiles the real decision logic rather than a copy of it.
$arguments = @('-std=c11','-Wall','-Wextra','-Werror',
    "$src/tests/stability-gate-test.c",'-o',"$out/stability-gate-test.exe")
& $Compiler @arguments
if ($LASTEXITCODE -ne 0) { throw 'stability-gate-test compilation failed' }
& "$out/stability-gate-test.exe"
if ($LASTEXITCODE -ne 0) { throw 'stability-gate regression failed' }

$plugin = Join-Path $src 'android-mediacodec.c'
$text = [IO.File]::ReadAllText($plugin, [Text.Encoding]::UTF8)

function Assert-Order([string]$name, [string[]]$needles) {
    $position = -1
    foreach ($needle in $needles) {
        $found = $text.IndexOf($needle)
        if ($found -lt 0) { throw "android-mediacodec.c: $name - missing '$needle'" }
        if ($found -lt $position) { throw "android-mediacodec.c: $name - '$needle' is out of order" }
        $position = $found
    }
}

# The gate must run before the submit, otherwise it can never prevent the
# blocking eglSwapBuffers it exists for.
Assert-Order 'shed gate precedes the submit' @(
    'mc_shed_decide(pending_now, e->degraded, soft_limit)',
    'gs_android_encoder_surface_submit(e->surface, t->tex[0], ns)')

# While shedding the encoder must still drain: without take_packet() the packets
# pile up until the output queue aborts, and the stall watchdog goes unreachable.
Assert-Order 'shed path still drains and still watches' @(
    'if (shed) {',
    'mc_flow_stalled(&e->flow, shed_now, NO_OUTPUT_NS)',
    'take_packet(e, packet, received, false)',
    'return shed_result;')

# The hard limit must stay within the output queue's capacity, and the soft
# limit must be a small absolute depth (the device proved a fraction of the hard
# limit is unreachable).
if ($text -notmatch 'if \(e->max_pending > MAX_QUEUED\)') {
    throw 'android-mediacodec.c: max_pending must be capped at MAX_QUEUED'
}
if ($text -match 'max_pending - STABILITY_HEADROOM') {
    throw 'android-mediacodec.c: the soft limit must not be derived from max_pending'
}

# Policy constants must come from the shared header, not be duplicated here.
foreach ($macro in @('MC_SHED_SOFT_PENDING', 'MC_SHED_MIN_PENDING')) {
    if ($text.IndexOf("#include `"shed-policy.h`"") -lt 0) { throw 'android-mediacodec.c: shed-policy.h not included' }
}

Write-Host 'PASS: mediacodec stability gate (policy, ordering, drain path).'
