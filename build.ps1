#!/usr/bin/env pwsh
<#
.SYNOPSIS
    AffineUI task runner (Windows / PowerShell). Unix/macOS: use ./build.sh

.DESCRIPTION
    Project-level commands with a single entry point. The MSVC environment
    (vcvars64) is set up automatically for the steps that need cl/cmake/ninja.

    .\build.ps1                  compile-check the prebuilt dist/ codefiles
                                 (uses the committed dist/; does NOT regenerate)
    .\build.ps1 codefiles        (re)generate dist/affineui.{h,cpp}  (REQUIRES clang)
    .\build.ps1 examples         build every example app (MSVC / D3D11)
    .\build.ps1 run [name]       build + run one example  (default: hello)
    .\build.ps1 test             build + run the unit tests (ctest)
    .\build.ps1 configure        cmake configure into build\ninja
    .\build.ps1 clean            remove build\
    .\build.ps1 sync-nanovg      vendor the affineui_nanovg fork  (needs bash+rsync)
    .\build.ps1 sync-lexbor      vendor the affineui_lexbor fork  (needs bash+rsync)
    .\build.ps1 sync-yoga        vendor the affineui_yoga fork  (needs bash+rsync)
    .\build.ps1 help             this help

    'codefiles' (the amalgamator) stages Lexbor as C++ and depends on clang's
    diagnostics; MSVC is NOT supported for that step, so it fails fast if clang
    is absent. The no-arg default instead compile-checks the prebuilt dist/ with
    the *platform* compiler (MSVC / D3D11 here). The MSVC/D3D11 path is also the
    modular build behind examples / run / test.
#>
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Argv
)

$ErrorActionPreference = 'Stop'

# ── argument parsing: GNU-style --flag=value, plus positionals ───────────────
# Verb + name are positional; flags (e.g. --toolchain=msvc) may appear anywhere.
$Toolchain  = 'auto'        # auto | msvc | clang | gcc
$Positional = @()
foreach ($a in $Argv) {
    if ($a -match '^--toolchain=(.+)$') { $Toolchain = $matches[1].ToLower() }
    elseif ($a -like '--*')             { Write-Host "warning: unknown option '$a'" -ForegroundColor Yellow }
    else                                { $Positional += $a }
}
$Verb = if ($Positional.Count -ge 1) { $Positional[0] } else { '' }
$Name = if ($Positional.Count -ge 2) { $Positional[1] } else { '' }
$Root  = $PSScriptRoot
$Build = Join-Path $Root 'build\ninja'
$Dist  = Join-Path $Root 'dist'
$Smoke = Join-Path $Root 'build\smoke'

# Primary example (what `run` launches with no name). The full set is NOT
# hardcoded — it's read from the manifest CMake writes at configure time
# (examples/CMakeLists.txt), so it can't drift out of date as examples are
# added, and optional ones (hello_sdl, embed_d3d11) only appear when their
# deps were actually found.
$Primary = 'hello'

# ── MSVC environment ─────────────────────────────────────────────────────────
function Initialize-Msvc {
    if ($env:_AFFINEUI_MSVC_READY -eq '1') { return }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = $null
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
    }
    if (-not $vsPath) {
        $vsPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    }
    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) {
        throw "vcvars64.bat not found under '$vsPath'. Install Visual Studio 2022 (or Build Tools) with the 'Desktop development with C++' workload."
    }
    # Import the batch environment (PATH, INCLUDE, LIB, ...) into this session.
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($matches[1])" -Value $matches[2]
        }
    }
    $env:_AFFINEUI_MSVC_READY = '1'
}

# ── clang (required by codefiles / smoke) ────────────────────────────────────
function Get-Clang {
    foreach ($n in 'clang++', 'clang') {
        $c = Get-Command $n -ErrorAction SilentlyContinue
        if ($c) { return $c.Source }
    }
    return $null
}

function Get-ClangOrFail {
    $c = Get-Clang
    if (-not $c) {
        Write-Host "error: 'codefiles' (amalgamation) requires clang, which was not found on PATH." -ForegroundColor Red
        Write-Host "       The amalgamator stages Lexbor as C++ and depends on clang's diagnostics;"
        Write-Host "       MSVC (cl) is not supported for this step."
        Write-Host "       Install LLVM and retry, e.g.:  winget install LLVM.LLVM"
        exit 1
    }
    return $c
}

function Assert-LastExit([string]$what) {
    if ($LASTEXITCODE -ne 0) {
        Write-Host "error: $what failed (exit $LASTEXITCODE)" -ForegroundColor Red
        exit $LASTEXITCODE
    }
}

# ── verbs ────────────────────────────────────────────────────────────────────
function Invoke-Configure {
    Initialize-Msvc
    if (-not (Test-Path (Join-Path $Build 'CMakeCache.txt'))) {
        Push-Location $Root
        try { cmake --preset ninja; Assert-LastExit 'cmake configure' }
        finally { Pop-Location }
    }
}

function Invoke-Codefiles {
    # Regenerate the two-file SDK. clang is mandatory (see header note).
    $clang = Get-ClangOrFail
    Initialize-Msvc   # so a Windows clang finds the CRT / Windows SDK headers
    Push-Location $Root
    try {
        python tools/amalgamate.py --root . --out dist --cxx $clang
        Assert-LastExit 'amalgamate'
    } finally { Pop-Location }
}

function Invoke-Smoke {
    # Compile-check the *prebuilt* (committed) two-file SDK in dist/ with the
    # platform compiler (MSVC / D3D11) -- a real "does the shipped SDK build on
    # Windows" check. Does NOT regenerate dist/ (that's `codefiles`).
    $cpp = Join-Path $Dist 'affineui.cpp'
    $h   = Join-Path $Dist 'affineui.h'
    if (-not ((Test-Path $cpp) -and (Test-Path $h))) {
        Write-Host "error: no prebuilt codefiles in dist/ (expected affineui.cpp + affineui.h)." -ForegroundColor Red
        Write-Host "       generate them first:  .\build.ps1 codefiles   (requires clang)"
        exit 1
    }
    $tc = if ($Toolchain -eq 'auto') { 'msvc' } else { $Toolchain }
    Initialize-Msvc        # backend is D3D11; clang/gcc also need the Win SDK headers
    New-Item -ItemType Directory -Force $Smoke | Out-Null
    $drv = Join-Path $Smoke 'smoke_main.cpp'
    Set-Content -Encoding utf8 -Path $drv -Value @'
#include "affineui.h"
int main() { affineui::Ui ui; (void)ui; return 0; }
'@
    Write-Host "smoke: compiling the prebuilt SDK (toolchain=$tc / D3D11) ..." -ForegroundColor Cyan
    switch ($tc) {
        'msvc' {
            $d = @('/DSOKOL_D3D11', '/DAFFINEUI_BACKEND_D3D11', '/DSOKOL_NO_ENTRY')
            cl /nologo /c /EHsc /std:c++20 @d /I $Dist "/Fo$Smoke\affineui.obj" $cpp
            Assert-LastExit 'SDK smoke compile (affineui.cpp)'
            cl /nologo /c /EHsc /std:c++20 @d /I $Dist "/Fo$Smoke\smoke_main.obj" $drv
            Assert-LastExit 'SDK smoke compile (affineui.h consumer)'
        }
        { $_ -in 'clang', 'gcc' } {
            $cc = if ($tc -eq 'clang') { Get-Clang } else { (Get-Command g++ -ErrorAction SilentlyContinue).Source }
            if (-not $cc) { Write-Host "error: --toolchain=$tc compiler not found on PATH." -ForegroundColor Red; exit 1 }
            $d = @('-DSOKOL_D3D11', '-DAFFINEUI_BACKEND_D3D11', '-DSOKOL_NO_ENTRY')
            & $cc -std=c++20 -c @d -I $Dist $cpp -o (Join-Path $Smoke 'affineui.o')
            Assert-LastExit 'SDK smoke compile (affineui.cpp)'
            & $cc -std=c++20 -c @d -I $Dist $drv -o (Join-Path $Smoke 'smoke_main.o')
            Assert-LastExit 'SDK smoke compile (affineui.h consumer)'
        }
        default {
            Write-Host "error: unknown --toolchain='$tc' (use msvc | clang | gcc)" -ForegroundColor Red
            exit 1
        }
    }
    Write-Host "smoke: OK" -ForegroundColor Green
}

# Example targets that actually exist in the configured build, straight from
# the manifest CMake wrote at configure time. Optional examples (hello_sdl,
# embed_d3d11) are absent from it when their deps weren't found, so whatever
# this returns is genuinely runnable.
function Get-ExampleTargets {
    Invoke-Configure
    $manifest = Join-Path $Build 'examples\examples.txt'
    if (-not (Test-Path $manifest)) { return @() }
    return @(Get-Content $manifest | ForEach-Object { $_.Trim() } |
             Where-Object { $_ })
}

function Invoke-Examples {
    $tgts = Get-ExampleTargets
    if (-not $tgts) { Write-Host "no example targets in this build." -ForegroundColor Yellow; return }
    cmake --build $Build --target $tgts --parallel
    Assert-LastExit 'build examples'
}

function Invoke-Run([string]$name) {
    if (-not $name) { $name = $Primary }
    $targets = Get-ExampleTargets
    if ($targets -notcontains $name) {
        Write-Host "unknown example '$name'." -ForegroundColor Red
        Write-Host "available:"
        foreach ($t in $targets) { Write-Host "  $t" }
        exit 1
    }
    cmake --build $Build --target $name --parallel
    Assert-LastExit "build example '$name'"
    $exe = Get-ChildItem -Path $Build -Recurse -Filter "$name.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $exe) {
        Write-Host "error: built '$name' but no $name.exe found under $Build" -ForegroundColor Red
        exit 1
    }
    Write-Host "running $($exe.FullName)" -ForegroundColor Cyan
    Push-Location $Root            # assets/ resolve relative to repo root
    try { & $exe.FullName } finally { Pop-Location }
}

function Invoke-Test {
    Invoke-Configure
    cmake --build $Build --parallel
    Assert-LastExit 'build'
    ctest --test-dir $Build --output-on-failure
    Assert-LastExit 'tests'
}

function Invoke-Conformance {
    # Build the headless tool, ensure the browser driver's deps, then run the
    # A/B harness (all tests). For one test: python conformance/run.py --test X
    Invoke-Configure
    cmake --build $Build --target conformance_test
    Assert-LastExit 'build conformance_test'
    $browser = Join-Path $Root 'conformance\browser'
    if (-not (Test-Path (Join-Path $browser 'node_modules'))) {
        Write-Host 'conformance: installing browser deps (npm install) ...' -ForegroundColor Cyan
        Push-Location $browser
        try { npm install --no-audit --no-fund; Assert-LastExit 'npm install' } finally { Pop-Location }
    }
    python (Join-Path $Root 'conformance\run.py')
}

function Invoke-Sync([string]$which) {
    $sh = Get-Command bash -ErrorAction SilentlyContinue
    if (-not $sh) { throw "sync-$which needs bash + rsync (use git-bash or WSL)." }
    & $sh.Source (Join-Path $Root "scripts/sync_${which}_from_fork.sh")
    Assert-LastExit "sync-$which"
}

function Show-Help {
    Write-Host @'
AffineUI task runner (Windows / PowerShell). Unix/macOS: use ./build.sh

  .\build.ps1                  compile-check the prebuilt dist/ codefiles
                               (uses the committed dist/; does NOT regenerate)
  .\build.ps1 codefiles        (re)generate dist/affineui.{h,cpp}  (REQUIRES clang)
  .\build.ps1 examples         build every example app (MSVC / D3D11)
  .\build.ps1 list             list every runnable example in this build
  .\build.ps1 run [name]       build + run one example  (default: hello)
  .\build.ps1 test             build + run the unit tests (ctest)
  .\build.ps1 conformance      A/B render every test in a browser vs AffineUI,
                               pixel-diff, write conformance/out/report.html
  .\build.ps1 configure        cmake configure into build\ninja
  .\build.ps1 clean            remove build\
  .\build.ps1 sync-nanovg      vendor the affineui_nanovg fork  (needs bash+rsync)
  .\build.ps1 sync-lexbor      vendor the affineui_lexbor fork  (needs bash+rsync)
  .\build.ps1 sync-yoga        vendor the affineui_yoga fork  (needs bash+rsync)
  .\build.ps1 help             this help

Example names come from the build itself (`.\build.ps1 list`) — no hardcoded
list to go stale, and optional examples (hello_sdl, embed_d3d11) only show up
when their dependencies were found at configure time.

'codefiles' (the amalgamator) stages Lexbor as C++ and depends on clang's
diagnostics; MSVC is NOT supported for that step, so it fails fast if clang is
absent. The MSVC/D3D11 path is the modular build behind examples / run / test.
'@
}

# ── dispatch ─────────────────────────────────────────────────────────────────
switch ($Verb) {
    ''            { Invoke-Smoke }
    'codefiles'   { Invoke-Codefiles }
    'examples'    { Invoke-Examples }
    'list'        { Get-ExampleTargets | ForEach-Object { Write-Host $_ } }
    'run'         { Invoke-Run $Name }
    'test'        { Invoke-Test }
    'conformance' { Invoke-Conformance }
    'configure'   { Invoke-Configure }
    'clean'       { if (Test-Path (Join-Path $Root 'build')) { Remove-Item -Recurse -Force (Join-Path $Root 'build') } }
    'sync-nanovg' { Invoke-Sync 'nanovg' }
    'sync-lexbor' { Invoke-Sync 'lexbor' }
    'sync-yoga'   { Invoke-Sync 'yoga' }
    'help'        { Show-Help }
    default       { Write-Host "unknown command '$Verb'`n"; Show-Help; exit 1 }
}
