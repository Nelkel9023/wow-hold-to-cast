# Builds dinput8.dll with the zig C compiler (zig cc, MinGW target).
#
# Requirements:
#   - zig on PATH, or pass -Zig <path-to-zig.exe>  (https://ziglang.org/download/)
#   - MinHook sources at third_party/minhook-master  (see README)
#
# Usage:
#   pwsh -File build/build.ps1
#   pwsh -File build/build.ps1 -Zig C:\zig\zig.exe
param(
    [string]$Zig = $env:ZIG
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $root 'src'
$tp   = Join-Path $root 'third_party'
$out  = Join-Path $root 'build'

if (-not $Zig) {
    $cmd = Get-Command zig -ErrorAction SilentlyContinue
    if ($cmd) { $Zig = $cmd.Source }
}
if (-not $Zig -or -not (Test-Path $Zig)) {
    throw 'zig.exe not found. Install zig, add it to PATH, or pass -Zig <path>.'
}

$mh = Join-Path $tp 'minhook-master'
foreach ($f in 'src\hook.c','src\buffer.c','src\trampoline.c','src\hde\hde32.c') {
    if (-not (Test-Path (Join-Path $mh $f))) {
        throw "MinHook source missing: $mh\$f  (see README for how to obtain MinHook)"
    }
}

if (-not (Test-Path $out)) { New-Item -ItemType Directory -Path $out | Out-Null }

$zigArgs = @(
    'cc', '-target', 'x86-windows-gnu', '-O2', '-shared',
    '-o', (Join-Path $out 'dinput8.dll'),
    (Join-Path $src 'proxy_dinput8.c'),
    (Join-Path $src 'holdtocast.c'),
    (Join-Path $mh 'src\hook.c'),
    (Join-Path $mh 'src\buffer.c'),
    (Join-Path $mh 'src\trampoline.c'),
    (Join-Path $mh 'src\hde\hde32.c'),
    '-I', (Join-Path $mh 'include'),
    '-I', (Join-Path $mh 'src')
)

Write-Host "zig: $Zig"
& $Zig @zigArgs
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "OK -> $out\dinput8.dll"
