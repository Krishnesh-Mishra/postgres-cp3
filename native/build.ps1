# Build the pointer-free libpq shim static library.
#
# The package ships a prebuilt native/lib/libpostgres_shim.a, so this script is
# only needed after editing native/src/postgres_shim.cpp or the public header.
#
# Requirements:
#   * a MinGW-w64 g++/ar toolchain (WinLibs, MSYS2, or similar)
#   * the vendored libpq headers in native/include/postgres
#
# Usage:
#   pwsh -File native/build.ps1
#   pwsh -File native/build.ps1 -MinGW "C:\mingw64\bin"
#   pwsh -File native/build.ps1 -Regenerate
#
# -Regenerate also runs `cp3 native-install` and rewrites the generated
# binding metadata to repository-relative paths.

param(
    [string]$MinGW = "",
    [string]$Cp3 = "",
    [switch]$Regenerate,
    [switch]$RebuildImportLib
)

$ErrorActionPreference = "Stop"

$native = $PSScriptRoot
$root = Split-Path -Parent $native

function Resolve-MinGW([string]$explicit) {
    if ($explicit) {
        if (Test-Path $explicit -PathType Leaf) {
            return (Split-Path -Parent (Resolve-Path $explicit).Path)
        }
        if (Test-Path $explicit -PathType Container) {
            return (Resolve-Path $explicit).Path
        }
        throw "MinGW path not found: $explicit"
    }

    if ($env:CP3_MINGW_BIN -and (Test-Path (Join-Path $env:CP3_MINGW_BIN "g++.exe"))) {
        return $env:CP3_MINGW_BIN
    }

    $gxx = Get-Command g++ -ErrorAction SilentlyContinue
    if ($gxx) {
        return (Split-Path -Parent $gxx.Source)
    }

    $wingetRoot = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
    if (Test-Path $wingetRoot) {
        $candidate = Get-ChildItem $wingetRoot -Directory -Filter "BrechtSanders.WinLibs*" -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName "mingw64\bin" } |
            Where-Object { Test-Path (Join-Path $_ "g++.exe") } |
            Select-Object -First 1
        if ($candidate) { return $candidate }
    }

    throw "Could not find g++. Pass -MinGW <path-to-mingw64-bin> or set CP3_MINGW_BIN."
}

function Resolve-Cp3([string]$explicit) {
    if ($explicit) {
        if (Test-Path $explicit -PathType Leaf) {
            return (Resolve-Path $explicit).Path
        }
        $candidate = Join-Path $explicit "cp3.cmd"
        if (Test-Path $candidate) { return $candidate }
        throw "cp3 executable not found: $explicit"
    }

    $cmd = Get-Command cp3 -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $known = "C:\Users\Krishnesh\AppData\Local\Programs\cp3\bin\cp3.cmd"
    if (Test-Path $known) { return $known }

    throw "Could not find cp3. Pass -Cp3 <path-to-cp3.cmd> or add it to PATH."
}

$mingwBin = Resolve-MinGW $MinGW
$gxx = Join-Path $mingwBin "g++.exe"
$ar = Join-Path $mingwBin "ar.exe"

if (-not (Test-Path $gxx)) { throw "g++ not found in $mingwBin" }
if (-not (Test-Path $ar)) { throw "ar not found in $mingwBin" }

New-Item -ItemType Directory -Force (Join-Path $native "obj") | Out-Null
New-Item -ItemType Directory -Force (Join-Path $native "lib") | Out-Null

# Clean stale objects so removed TUs are not archived.
Get-ChildItem (Join-Path $native "obj") -Filter "*.o" | Remove-Item -Force

foreach ($source in (Get-ChildItem (Join-Path $native "src") -Filter "*.cpp" | Sort-Object Name)) {
    $object = Join-Path (Join-Path $native "obj") ($source.BaseName + ".o")
    Write-Host "compiling native/src/$($source.Name)"
    & $gxx -std=c++17 -O2 -Wall -Wextra `
        -c $source.FullName `
        -o $object `
        -I (Join-Path $native "include") `
        -I (Join-Path $native "include\postgres") `
        -I (Join-Path $native "src")
    if ($LASTEXITCODE -ne 0) { throw "g++ failed with exit code $LASTEXITCODE" }
}

Write-Host "archiving native/lib/libpostgres_shim.a"
# Remove the old archive first: `ar rcs` only appends/replaces members, so
# stale objects from before a TU split would linger and duplicate symbols.
Remove-Item (Join-Path $native "lib\libpostgres_shim.a") -Force -ErrorAction SilentlyContinue
$objects = Get-ChildItem (Join-Path $native "obj") -Filter "*.o" | ForEach-Object { $_.FullName }
& $ar rcs (Join-Path $native "lib\libpostgres_shim.a") @objects
if ($LASTEXITCODE -ne 0) { throw "ar failed with exit code $LASTEXITCODE" }

if ($RebuildImportLib) {
    $gendef = Join-Path $mingwBin "gendef.exe"
    $dlltool = Join-Path $mingwBin "dlltool.exe"
    if (-not (Test-Path $gendef) -or -not (Test-Path $dlltool)) {
        throw "-RebuildImportLib needs gendef.exe and dlltool.exe in $mingwBin"
    }
    $libDir = Join-Path $native "lib"
    $dll = Join-Path $native "runtime\libpq.dll"
    if (-not (Test-Path $dll)) { throw "missing $dll" }

    Push-Location $libDir
    try {
        & $gendef $dll
        if ($LASTEXITCODE -ne 0) { throw "gendef failed with exit code $LASTEXITCODE" }
        & $dlltool -d "libpq.def" -l "libpq.a" -D $dll
        if ($LASTEXITCODE -ne 0) { throw "dlltool failed with exit code $LASTEXITCODE" }
    } finally {
        Pop-Location
    }
    Copy-Item (Join-Path $libDir "libpq.a") (Join-Path $libDir "libpq.dll.a") -Force
    Write-Host "rebuilt native/lib/libpq.a and native/lib/libpq.dll.a"
}

if ($Regenerate) {
    $cp3Exe = Resolve-Cp3 $Cp3
    Push-Location $root
    try {
        Write-Host "regenerating .cp3/native/postgres"
        & $cp3Exe native-install "native\include\postgres_shim.h" `
            --name postgres --lib-dir "native\lib" `
            --link postgres_shim --link pq --force --json
        if ($LASTEXITCODE -ne 0) { throw "cp3 native-install failed with exit code $LASTEXITCODE" }
    } finally {
        Pop-Location
    }

    $runtimeDlls = @(
        "native/runtime/libpq.dll",
        "native/runtime/libssl-3-x64.dll",
        "native/runtime/libcrypto-3-x64.dll",
        "native/runtime/libintl-9.dll",
        "native/runtime/libiconv-2.dll",
        "native/runtime/libwinpthread-1.dll"
    )

    $utf8 = New-Object System.Text.UTF8Encoding($false)

    $packageJsonPath = Join-Path $root ".cp3\native\postgres\native.json"
    $packageJson = Get-Content $packageJsonPath -Raw | ConvertFrom-Json
    $packageJson.source = "native/include/postgres_shim.h"
    $packageJson.includeDirs = @("native/include")
    $packageJson.libDirs = @("native/lib")
    $packageJson.staticLibraries = @("native/lib/libpostgres_shim.a")
    $packageJson.sharedLibraries = @("native/lib/libpq.a")
    $packageJson.runtimeDlls = $runtimeDlls

    # The generated index records the absolute source path and the compiler
    # hashes the exact index bytes. Rewrite the comment to a repository-relative
    # path, then refresh contentHash so the binding is not reported stale.
    $packageIndexPath = Join-Path $root ".cp3\native\postgres\index.cp3"
    $packageIndex = Get-Content $packageIndexPath -Raw
    $packageIndex = $packageIndex -replace '// Source: .*postgres_shim\.h', '// Source: native/include/postgres_shim.h'
    [System.IO.File]::WriteAllText($packageIndexPath, $packageIndex, $utf8)
    $packageJson.contentHash = (Get-FileHash $packageIndexPath -Algorithm SHA256).Hash.ToLower()
    [System.IO.File]::WriteAllText($packageJsonPath, ($packageJson | ConvertTo-Json -Depth 100), $utf8)

    $testNative = Join-Path $root "tests\.cp3\native\postgres"
    New-Item -ItemType Directory -Force $testNative | Out-Null
    $testIndexPath = Join-Path $testNative "index.cp3"
    Copy-Item $packageIndexPath $testIndexPath -Force
    $testIndex = Get-Content $testIndexPath -Raw
    $testIndex = $testIndex -replace '// Source: .*postgres_shim\.h', '// Source: ../native/include/postgres_shim.h'
    [System.IO.File]::WriteAllText($testIndexPath, $testIndex, $utf8)

    $testJson = Get-Content $packageJsonPath -Raw | ConvertFrom-Json
    $testJson.source = "../native/include/postgres_shim.h"
    $testJson.includeDirs = @("../native/include")
    $testJson.libDirs = @("../native/lib")
    $testJson.staticLibraries = @("../native/lib/libpostgres_shim.a")
    $testJson.sharedLibraries = @("../native/lib/libpq.a")
    $testJson.runtimeDlls = @($runtimeDlls | ForEach-Object { "../$_" })
    $testJson.contentHash = (Get-FileHash $testIndexPath -Algorithm SHA256).Hash.ToLower()
    [System.IO.File]::WriteAllText((Join-Path $testNative "native.json"), ($testJson | ConvertTo-Json -Depth 100), $utf8)

    Write-Host "binding regenerated; test binding updated"
}

Write-Host "done"
