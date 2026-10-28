<#
.SYNOPSIS
  Makes the small "compiler" folder a player's folder carries (docs/vu-live.md), out of an llvm-mingw this PC has.

.DESCRIPTION
  The runner (ps2run.exe) compiles vector programs the build does not know while the game runs. It needs a C++ compiler for that.
  A player has none, so the folder next to the game holds a trimmed copy of llvm-mingw: clang++, the linker, the C++ headers,
  the C library headers and the few libraries a generated DLL links with. About 270 MB unpacked, 70 MB zipped.

  Where it looks for llvm-mingw: -From, then tools\portable\llvm-mingw (scripts\portable.ps1 puts it there), then the
  clang++ found on the PATH (two folders up from its bin).

  When it is done it tests the copy: it compiles and links a small DLL with a PATH of only System32, the way a
  player's PC would. Nothing is changed in the source llvm-mingw.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\pack_compiler.ps1 -Out "C:\Users\me\Desktop\PS2 Games\program\compiler"
  powershell -ExecutionPolicy Bypass -File scripts\pack_compiler.ps1 -Out build\compiler -Zip build\compiler.zip
#>
param(
    [Parameter(Mandatory = $true)] [string] $Out,   # the folder to make (it is emptied first if it exists)
    [string] $From = "",                             # the llvm-mingw folder (the one with bin, include, lib)
    [string] $Zip = "",                              # also write a zip of the folder here
    [switch] $NoTest                                 # skip the compile test at the end
)
$ErrorActionPreference = 'Stop'

function Find-Source {
    if ($From) { return (Resolve-Path -LiteralPath $From).Path }
    $repo = Split-Path -Parent $PSScriptRoot
    $portable = Join-Path $repo 'tools\portable\llvm-mingw'
    if (Test-Path -LiteralPath (Join-Path $portable 'bin\clang++.exe')) { return $portable }
    $cmd = Get-Command clang++.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        $root = Split-Path -Parent (Split-Path -Parent $cmd.Source)
        if (Test-Path -LiteralPath (Join-Path $root 'lib\clang')) { return $root }
    }
    throw "No llvm-mingw found. Run scripts\portable.ps1, or pass -From <llvm-mingw folder>."
}

$src = Find-Source
if (-not (Test-Path -LiteralPath (Join-Path $src 'bin\clang++.exe'))) { throw "$src has no bin\clang++.exe" }
$triple = 'x86_64-w64-mingw32'
Write-Host "from $src"

$Out = [IO.Path]::GetFullPath($Out)
if ($Out.TrimEnd('\').Length -le 3) { throw "Refusing to use $Out" }
if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Recurse -Force }
New-Item -ItemType Directory -Path $Out | Out-Null

function Copy-Files([string] $fromDir, [string] $toDir, [string[]] $patterns) {
    New-Item -ItemType Directory -Force -Path $toDir | Out-Null
    foreach ($p in $patterns) {
        $found = @(Get-ChildItem -LiteralPath $fromDir -File -Filter $p -ErrorAction SilentlyContinue)
        if (-not $found) { Write-Host "  (nothing matches $p in $fromDir)"; continue }
        foreach ($f in $found) { Copy-Item -LiteralPath $f.FullName -Destination $toDir }
    }
}
function Copy-Tree([string] $fromDir, [string] $toDir) {
    robocopy $fromDir $toDir /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE) for $fromDir" }
    $global:LASTEXITCODE = 0
}

# bin: the driver (clang++.exe is a small launcher), the compiler, the linker, their DLLs and the .cfg files that make clang
# pick libc++ and the mingw target (without them it looks for libstdc++ headers and fails).
Copy-Files "$src\bin" "$Out\bin" @(
    'clang++.exe', 'clang-*.exe', 'ld.lld.exe', 'libclang-cpp.dll', 'libLLVM-*.dll', 'libc++.dll', 'libunwind.dll',
    'libffi-*.dll', 'libwinpthread-1.dll', 'x86_64-pc-windows-gnu.cfg', 'x86_64-w64-windows-gnu.cfg', 'mingw32-common.cfg')
# clang-format, clang-tidy and the scan-deps tools match 'clang-*.exe' too and are not needed.
foreach ($extra in 'clang-format.exe', 'clang-tidy.exe', 'clang-scan-deps.exe', 'clang-scan-deps-real.exe', 'clang-scan-deps-wrapper.exe', 'clang-target-wrapper.exe') {
    Remove-Item -LiteralPath "$Out\bin\$extra" -Force -ErrorAction SilentlyContinue
}

# include: the libc++ headers and the mingw-w64 headers.
Copy-Tree "$src\include" "$Out\include"

# lib\clang\<version>: clang's own headers (immintrin.h and friends) and the compiler runtime for Windows.
$clangVersion = (Get-ChildItem -LiteralPath "$src\lib\clang" -Directory | Sort-Object Name | Select-Object -Last 1).Name
Copy-Tree "$src\lib\clang\$clangVersion\include" "$Out\lib\clang\$clangVersion\include"
Copy-Files "$src\lib\clang\$clangVersion\lib\windows" "$Out\lib\clang\$clangVersion\lib\windows" @('libclang_rt.builtins-x86_64.a')

# <triple>\lib: the start-up files and the libraries a DLL links with; the rest (a thousand-odd import libraries for Windows
# APIs and old C runtimes) is dropped.
Copy-Files "$src\$triple\lib" "$Out\$triple\lib" @(
    '*.o', 'libmingw32.a', 'libmingwex.a', 'libmoldname.a', 'libucrt.a', 'libmsvcrt.a', 'libkernel32.a', 'libuser32.a',
    'libadvapi32.a', 'libshell32.a', 'libntdll.a', 'libgdi32.a', 'libws2_32.a', 'libcrypt32.a', 'libbcrypt.a',
    'libc++*.a', 'libunwind*.a', 'libwinpthread*.a', 'libpthread*.a')
# The headers of the triple folder (usually empty: include\ above holds them).
if (Test-Path -LiteralPath "$src\$triple\include") { Copy-Tree "$src\$triple\include" "$Out\$triple\include" }

foreach ($license in 'LICENSE.TXT', 'LICENSE', 'README.md') {
    if (Test-Path -LiteralPath "$src\$license") { Copy-Item -LiteralPath "$src\$license" -Destination $Out }
}

$files = @(Get-ChildItem -LiteralPath $Out -Recurse -File)
$mb = ($files | Measure-Object Length -Sum).Sum / 1MB
Write-Host ('made {0}: {1} files, {2:N0} MB' -f $Out, $files.Count, $mb)

if (-not $NoTest) {
    # The way a player's PC runs it: a PATH of System32 only, the compiler called by its full path.
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('pack_compiler_' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        @'
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <immintrin.h>
extern "C" __declspec(dllexport) float pack_compiler_test(const float *a, int n) {
    std::vector<float> v(a, a + n);
    __m128 s = _mm_setzero_ps();
    for (int i = 0; i + 4 <= n; i += 4) s = _mm_add_ps(s, _mm_loadu_ps(&v[i]));
    float t[4];
    _mm_storeu_ps(t, s);
    return std::sqrt(t[0] + t[1] + t[2] + t[3]);
}
'@ | Set-Content -LiteralPath "$tmp\t.cpp" -Encoding ASCII
        $saved = $env:Path
        $env:Path = [Environment]::GetFolderPath('System')
        try {
            $log = & "$Out\bin\clang++.exe" -std=c++20 -O3 -mavx2 -mfma -shared -o "$tmp\t.dll" "$tmp\t.cpp" 2>&1
            $code = $LASTEXITCODE
        } finally { $env:Path = $saved }
        if ($code -ne 0 -or -not (Test-Path -LiteralPath "$tmp\t.dll")) {
            Write-Host ($log | Out-String)
            throw "The test compile with the packed compiler failed (code $code). A file the compiler needs is missing from the copy."
        }
        Write-Host ('test: compiled and linked a DLL with a PATH of System32 only ({0:N0} KB)' -f ((Get-Item "$tmp\t.dll").Length / 1KB))
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if ($Zip) {
    $Zip = [IO.Path]::GetFullPath($Zip)
    if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force }
    # tar.exe is on every Windows 10/11 and writes zip files; the folder is stored as "compiler\...".
    $parent = Split-Path -Parent $Out
    $leaf = Split-Path -Leaf $Out
    Push-Location -LiteralPath $parent
    try { & tar.exe -a -c -f $Zip $leaf } finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { throw "tar.exe failed ($LASTEXITCODE)" }
    Write-Host ('zip: {0} ({1:N0} MB)' -f $Zip, ((Get-Item -LiteralPath $Zip).Length / 1MB))
}
