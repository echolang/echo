# Copy clang, lld-link and a Windows sysroot next to a released echoc so
# `echoc build` works on a machine that has never installed LLVM or VS.
#
#   tools/windows/bundle_toolchain.ps1 <dest-root>
#
# dest-root is the stage directory: bin/, lib/clang/<ver>, sysroot/{lib,include}.
# LLVM_PATH, LIB and INCLUDE come from the MSVC+LLVM environment the release
# job already has. A missing sysroot is a hard failure: that would ship a
# compiler that cannot link.

$ErrorActionPreference = "Stop"

if ($args.Count -ne 1) {
    Write-Error "usage: bundle_toolchain.ps1 <dest-root>"
    exit 2
}

$DestRoot = $args[0]
if (-not $env:LLVM_PATH) {
    Write-Error "bundle_toolchain: LLVM_PATH is not set"
    exit 2
}

$llvmBin = Join-Path $env:LLVM_PATH "bin"
$destBin = Join-Path $DestRoot "bin"
$destLib = Join-Path $DestRoot "lib"
$destSysroot = Join-Path $DestRoot "sysroot"
$destSysLib = Join-Path $destSysroot "lib"
$destSysInc = Join-Path $destSysroot "include"

New-Item -ItemType Directory -Path $destBin, $destLib, $destSysLib, $destSysInc -Force | Out-Null

$tools = @("clang.exe", "lld.exe", "lld-link.exe", "llvm-nm.exe", "llvm-ar.exe")
foreach ($name in $tools) {
    $src = Join-Path $llvmBin $name
    if (-not (Test-Path -LiteralPath $src)) {
        Write-Error "bundle_toolchain: $src is missing"
        exit 1
    }
    Copy-Item -LiteralPath $src -Destination (Join-Path $destBin $name) -Force
}

$readobj = Join-Path $llvmBin "llvm-readobj.exe"
function Get-Imports([string]$Exe) {
    if (-not (Test-Path -LiteralPath $readobj)) {
        return @()
    }
    $names = @()
    & $readobj --coff-imports $Exe | ForEach-Object {
        if ($_ -match '^\s+Name:\s+(\S+\.dll)$') {
            $names += $Matches[1]
        }
    }
    return $names
}

$pending = New-Object System.Collections.Generic.Queue[string]
Get-ChildItem -LiteralPath $destBin -Filter "*.exe" -ErrorAction SilentlyContinue | ForEach-Object {
    $pending.Enqueue($_.FullName)
}
$copiedDll = @{}
while ($pending.Count -gt 0) {
    $exe = $pending.Dequeue()
    foreach ($dll in Get-Imports $exe) {
        $key = $dll.ToLowerInvariant()
        if ($copiedDll.ContainsKey($key)) {
            continue
        }
        $src = Join-Path $llvmBin $dll
        if (-not (Test-Path -LiteralPath $src)) {
            continue
        }
        $dest = Join-Path $destBin $dll
        Copy-Item -LiteralPath $src -Destination $dest -Force
        $copiedDll[$key] = $true
        $pending.Enqueue($dest)
    }
}

$clangLib = Join-Path $env:LLVM_PATH "lib\clang"
if (-not (Test-Path -LiteralPath $clangLib)) {
    Write-Error "bundle_toolchain: no clang resource directory at $clangLib"
    exit 1
}
Copy-Item -LiteralPath $clangLib -Destination (Join-Path $destLib "clang") -Recurse -Force

if (-not $env:LIB) {
    Write-Error "bundle_toolchain: LIB is empty; msvc-dev-cmd did not run"
    exit 1
}

$libCount = 0
foreach ($dir in ($env:LIB -split ";" | Where-Object { $_ -and $_.Trim().Length -gt 0 })) {
    if (-not (Test-Path -LiteralPath $dir)) {
        continue
    }
    Get-ChildItem -LiteralPath $dir -Filter "*.lib" -ErrorAction SilentlyContinue | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $destSysLib $_.Name) -Force
        $libCount += 1
    }
}

if ($libCount -eq 0) {
    Write-Error "bundle_toolchain: copied no .lib files from LIB=$env:LIB"
    exit 1
}

foreach ($must in @("libcmt.lib", "libucrt.lib", "libvcruntime.lib", "oldnames.lib", "kernel32.lib")) {
    if (-not (Test-Path -LiteralPath (Join-Path $destSysLib $must))) {
        Write-Error "bundle_toolchain: $must is missing from $destSysLib"
        exit 1
    }
}

if (-not $env:INCLUDE) {
    Write-Error "bundle_toolchain: INCLUDE is empty; msvc-dev-cmd did not run"
    exit 1
}

function Include-Leaf([string]$Path) {
    # Split-Path -LiteralPath -Leaf is not a parameter set, in Windows PowerShell
    # or in pwsh. GetFileName does not glob and does not need a trailing-slash dance
    $leaf = [System.IO.Path]::GetFileName($Path.TrimEnd('\', '/'))
    if ($leaf -eq "winrt" -or $leaf -eq "cppwinrt" -or $leaf -eq "atl" -or $leaf -eq "mfc") {
        return $null
    }
    if ($leaf -eq "ucrt" -or $leaf -eq "um" -or $leaf -eq "shared") {
        return $leaf
    }
    if ($Path -match "MSVC") {
        return "msvc"
    }
    if ($Path -match "clang") {
        return $null
    }
    return $leaf
}

$incCount = 0
$usedLeaf = @{}
foreach ($dir in ($env:INCLUDE -split ";" | Where-Object { $_ -and $_.Trim().Length -gt 0 })) {
    if (-not (Test-Path -LiteralPath $dir)) {
        continue
    }
    $leaf = Include-Leaf $dir
    if ($null -eq $leaf) {
        continue
    }
    if ($usedLeaf.ContainsKey($leaf)) {
        $n = 2
        while ($usedLeaf.ContainsKey("$leaf-$n")) {
            $n += 1
        }
        $leaf = "$leaf-$n"
    }
    $usedLeaf[$leaf] = $true
    $dest = Join-Path $destSysInc $leaf
    Copy-Item -LiteralPath $dir -Destination $dest -Recurse -Force
    $incCount += 1
}

if ($incCount -eq 0) {
    Write-Error "bundle_toolchain: copied no include directories from INCLUDE=$env:INCLUDE"
    exit 1
}

Write-Host "bundle_toolchain: $libCount import libraries, $incCount include trees, $($copiedDll.Count) DLLs"
