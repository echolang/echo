# Windows release: two-stage echoc, bundled clang/lld/sysroot, epm, zip, Inno.
#
# Required env:
#   LLVM_PATH, BUILD_TYPE, VERSION, ECO_VERSION_PATCH, ECO_VERSION_SUFFIX
#   BIN_NAME, EPM_BIN_NAME
#
# ECO_STATIC_ZSTD is on and stays on. A fallback to shared zstd would ship a
# binary that only starts on the runner.

$ErrorActionPreference = "Stop"

function Invoke-Checked {
    param([scriptblock]$Block, [string]$What)
    & $Block
    if ($LASTEXITCODE -ne 0) {
        Write-Error "$What failed with exit $LASTEXITCODE"
        exit $LASTEXITCODE
    }
}

foreach ($name in @("LLVM_PATH", "BUILD_TYPE", "VERSION", "BIN_NAME", "EPM_BIN_NAME")) {
    $value = [Environment]::GetEnvironmentVariable($name)
    if ([string]::IsNullOrEmpty($value)) {
        Write-Error "build_release: $name is not set"
        exit 2
    }
}

function Unix-Path([string]$WinPath) {
    $converted = & bash -c "cygpath -u `"$WinPath`""
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($converted)) {
        Write-Error "cygpath failed for $WinPath"
        exit 1
    }
    return ([string]$converted).Trim()
}

$root = (Get-Location).Path
$llvmClang = Join-Path $env:LLVM_PATH "bin\clang.exe"
$llvmClangxx = Join-Path $env:LLVM_PATH "bin\clang++.exe"
$llvmDir = Join-Path $env:LLVM_PATH "lib\cmake\llvm"
$patch = $env:ECO_VERSION_PATCH
$suffix = $env:ECO_VERSION_SUFFIX
if ($null -eq $suffix) { $suffix = "" }

Write-Host "configure stage one"
Invoke-Checked -What "cmake stage one" -Block {
    & cmake -B build -G Ninja `
        "-DCMAKE_BUILD_TYPE=$env:BUILD_TYPE" `
        "-DCMAKE_C_COMPILER=$llvmClang" `
        "-DCMAKE_CXX_COMPILER=$llvmClangxx" `
        "-DLLVM_DIR=$llvmDir"
}

Write-Host "build stage one"
Invoke-Checked -What "build echoc" -Block {
    & cmake --build (Join-Path $root "build") --config $env:BUILD_TYPE --target echoc
}

Write-Host "regenerate embedded stdlib"
New-Item -ItemType File -Path emit.eco -Force | Out-Null
Invoke-Checked -What "emit-stdlib-header" -Block {
    & .\build\echoc.exe run --emit-stdlib-header emit.eco
}
Remove-Item emit.eco

Write-Host "configure release (static zstd required)"
Invoke-Checked -What "cmake release" -Block {
    & cmake -B build-release -G Ninja `
        "-DCMAKE_BUILD_TYPE=$env:BUILD_TYPE" `
        "-DCMAKE_C_COMPILER=$llvmClang" `
        "-DCMAKE_CXX_COMPILER=$llvmClangxx" `
        "-DLLVM_DIR=$llvmDir" `
        "-DECO_EMBED_STDLIB=ON" `
        "-DECO_STATIC_ZSTD=ON" `
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded" `
        "-DECO_VERSION_PATCH=$patch" `
        "-DECO_VERSION_SUFFIX=$suffix"
}

Write-Host "build release"
Invoke-Checked -What "build release echoc" -Block {
    & cmake --build (Join-Path $root "build-release") --config Release --target echoc
}

$stage = Join-Path $root "stage"
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path (Join-Path $stage "bin") -Force | Out-Null
Copy-Item (Join-Path $root "build-release\echoc.exe") (Join-Path $stage "bin\echoc.exe")

Write-Host "bundle clang, lld-link and the Windows sysroot"
$bundle = Join-Path $root "tools\windows\bundle_toolchain.ps1"
Invoke-Checked -What "bundle_toolchain" -Block {
    & $bundle $stage
}

foreach ($name in @("zstd.dll", "libzstd.dll", "zlib.dll", "zlib1.dll")) {
    $src = Join-Path $env:LLVM_PATH "bin\$name"
    if (Test-Path -LiteralPath $src) {
        Copy-Item $src (Join-Path $stage "bin\$name") -Force
    }
}

Copy-Item (Join-Path $root "README.md") (Join-Path $stage "README.md") -Force
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE") -Force

function Invoke-WithCleanPath {
    param([string]$BinDir, [scriptblock]$Block)
    $savedPath = $env:PATH
    $savedLlvm = $env:LLVM_PATH
    $env:PATH = "$BinDir;C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem"
    Remove-Item Env:LLVM_PATH -ErrorAction SilentlyContinue
    try {
        & $Block
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }
    finally {
        $env:PATH = $savedPath
        if ($savedLlvm) { $env:LLVM_PATH = $savedLlvm }
    }
}

$stageBin = Join-Path $stage "bin"
Write-Host "smoke test echoc with a clean PATH (including echoc build)"
Invoke-WithCleanPath -BinDir $stageBin -Block {
    $reported = ((& (Join-Path $stageBin "echoc.exe") --version) | Out-String).Trim()
    if ($reported -ne $env:VERSION) {
        Write-Error "The binary reports '$reported', expected '$env:VERSION'."
        exit 1
    }

    $scratch = Join-Path $env:RUNNER_TEMP "echo-smoke"
    New-Item -ItemType Directory -Path $scratch -Force | Out-Null
    Set-Location $scratch
    Set-Content -Path smoke.eco -Value "assert(1 == 1);`nvar `$xs = [1, 2, 3];`ndprint(`$xs[1]);`n" -NoNewline
    & (Join-Path $stageBin "echoc.exe") run smoke.eco
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $stageBin "echoc.exe") build smoke.eco -o smoke.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if (-not (Test-Path -LiteralPath "smoke.exe")) {
        Write-Error "echoc build did not write smoke.exe"
        exit 1
    }
    & .\smoke.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
Set-Location $root

Write-Host "check echoc dependencies"
$echocUnix = Unix-Path (Join-Path $stageBin "echoc.exe")
Invoke-Checked -What "check_release_deps echoc" -Block {
    bash ./tools/check_release_deps.sh $echocUnix
}

$curlPrefixWin = Join-Path $env:RUNNER_TEMP "static-curl"
$epmOutWin = Join-Path $env:RUNNER_TEMP "epm"
Write-Host "build static libcurl"
$curlPrefixUnix = Unix-Path $curlPrefixWin
Invoke-Checked -What "build_static_curl" -Block {
    bash ./tools/build_static_curl.sh $curlPrefixUnix
}

Write-Host "build epm"
$epmOutUnix = Unix-Path $epmOutWin
Invoke-Checked -What "build_release_epm" -Block {
    bash ./tools/build_release_epm.sh build-release/echoc.exe $epmOutUnix $curlPrefixUnix $env:VERSION
}

$epmExe = Join-Path $env:RUNNER_TEMP "epm.exe"
if (-not (Test-Path -LiteralPath $epmExe)) {
    $epmExe = Join-Path $env:RUNNER_TEMP "epm"
}
Copy-Item $epmExe (Join-Path $stageBin "epm.exe") -Force

Write-Host "smoke test epm"
$reportedEpm = ((& $epmExe --version) | Out-String).Trim()
if ($reportedEpm -ne $env:VERSION) {
    Write-Error "epm reports '$reportedEpm', expected '$env:VERSION'."
    exit 1
}

Write-Host "check epm dependencies"
$epmCheck = Unix-Path $epmExe
Invoke-Checked -What "check_release_deps epm" -Block {
    bash ./tools/check_release_deps.sh $epmCheck
}

$epmStage = Join-Path $root "epm-stage"
if (Test-Path -LiteralPath $epmStage) {
    Remove-Item -LiteralPath $epmStage -Recurse -Force
}
New-Item -ItemType Directory -Path $epmStage -Force | Out-Null
Copy-Item (Join-Path $stageBin "epm.exe") (Join-Path $epmStage "epm.exe")
Copy-Item (Join-Path $root "LICENSE") (Join-Path $epmStage "LICENSE")

Write-Host "package zip archives"
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath (Join-Path $root "$env:BIN_NAME.zip") -Force
Compress-Archive -Path (Join-Path $epmStage "*") -DestinationPath (Join-Path $root "$env:EPM_BIN_NAME.zip") -Force

Write-Host "build setup wizard"
$iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (-not (Test-Path -LiteralPath $iscc)) {
    $iscc = "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
}
if (-not (Test-Path -LiteralPath $iscc)) {
    Write-Error "ISCC.exe not found; install Inno Setup 6"
    exit 1
}
Invoke-Checked -What "ISCC" -Block {
    & $iscc "/DAppVersion=$env:VERSION" "/DStageDir=$stage" "/O$root" (Join-Path $root "tools\windows\echo.iss")
}
$setup = Join-Path $root "echo-windows-x86_64-setup.exe"
if (-not (Test-Path -LiteralPath $setup)) {
    Write-Error "ISCC did not write echo-windows-x86_64-setup.exe"
    exit 1
}

Write-Host "smoke test the setup wizard"
$setupDir = Join-Path $env:RUNNER_TEMP "echo-setup-smoke"
$setupLog = Join-Path $env:RUNNER_TEMP "echo-setup-smoke.log"
$installed = Join-Path $setupDir "bin\echoc.exe"

# Inno's Setup.exe is a GUI subsystem binary. `&` in PowerShell does not wait
# for those, so LASTEXITCODE was still ISCC's 0 and the next line looked for
# echoc 136ms later. Start-Process -Wait is the wait; the poll is in case the
# stub returns before the inner installer has finished copying the sysroot
$proc = Start-Process -FilePath $setup -ArgumentList @(
    "/VERYSILENT",
    "/NORESTART",
    "/SUPPRESSMSGBOXES",
    "/CURRENTUSER",
    "/DIR=$setupDir",
    "/LOG=$setupLog"
) -Wait -PassThru
if ($null -eq $proc.ExitCode -or $proc.ExitCode -ne 0) {
    if (Test-Path -LiteralPath $setupLog) { Get-Content -LiteralPath $setupLog }
    Write-Error "silent setup exited $($proc.ExitCode)"
    exit 1
}

$deadline = (Get-Date).AddMinutes(15)
while (-not (Test-Path -LiteralPath $installed)) {
    if ((Get-Date) -gt $deadline) {
        if (Test-Path -LiteralPath $setupLog) { Get-Content -LiteralPath $setupLog }
        if (Test-Path -LiteralPath $setupDir) {
            Get-ChildItem -LiteralPath $setupDir -Recurse -ErrorAction SilentlyContinue |
                Select-Object -First 80 -ExpandProperty FullName
        }
        $fallback = Join-Path $env:LOCALAPPDATA "echo\bin\echoc.exe"
        if (Test-Path -LiteralPath $fallback) {
            Write-Error "silent setup wrote $fallback instead of $installed (/DIR was ignored)"
        } else {
            Write-Error "silent setup did not write $installed"
        }
        exit 1
    }
    Start-Sleep -Seconds 2
}

Invoke-WithCleanPath -BinDir (Join-Path $setupDir "bin") -Block {
    $reported = ((& $installed --version) | Out-String).Trim()
    if ($reported -ne $env:VERSION) {
        Write-Error "setup-installed echoc reports '$reported', expected '$env:VERSION'."
        exit 1
    }
    $scratch = Join-Path $env:RUNNER_TEMP "echo-setup-build"
    New-Item -ItemType Directory -Path $scratch -Force | Out-Null
    Set-Location $scratch
    Set-Content -Path hello.eco -Value "echo 1;`n" -NoNewline
    & $installed build hello.eco -o hello.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & .\hello.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
Set-Location $root

Write-Host "windows release ok: $env:VERSION"
