param([string]$Compiler = 'C:/msys64/ucrt64/bin/gcc.exe')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$out = Join-Path (Split-Path $repo -Parent) 'build-tools/checks/recovery'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:PATH = (Split-Path $Compiler -Parent) + ';' + $env:PATH
$env:TEMP = $out
$env:TMP = $out
& $Compiler '-std=c11' '-Wall' '-Wextra' '-Werror' "$repo/obs-studio/libobs-opengl/tests/android-egl-retry-test.c" '-o' "$out/egl-retry.exe"
if ($LASTEXITCODE -ne 0) { throw 'EGL retry compilation failed' }
& "$out/egl-retry.exe"
if ($LASTEXITCODE -ne 0) { throw 'EGL retry test failed' }
