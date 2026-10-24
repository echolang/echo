# installs the latest released echoc and epm into %LOCALAPPDATA%\echo:
#
#   irm https://raw.githubusercontent.com/echolang/echo/master/install.ps1 | iex
#
# a released echoc carries the standard library inside it. epm is the package
# manager and sits next to it. clang, lld-link and a Windows sysroot ship in
# the same archive, so `echoc build` works without a separate LLVM install.
# set ECHO_INSTALL_DIR to install somewhere other than %LOCALAPPDATA%\echo.
# set ECHO_REQUIRE_ATTESTATION=1 to refuse an archive `gh attestation verify` cannot prove.
# set ECHO_UNINSTALL=1 (or pass -Uninstall when invoking the file) to remove it.

$ErrorActionPreference = "Stop"

$Repo = "echolang/echo"
$Uninstall = $false
if ($args -contains "-Uninstall") {
    $Uninstall = $true
}
if ($env:ECHO_UNINSTALL -eq "1") {
    $Uninstall = $true
}

$ManifestName = ".echo-install-manifest"

function Get-EchoInstallRoot {
    $root = Join-Path $env:LOCALAPPDATA "echo"
    if ($env:ECHO_INSTALL_DIR -and $env:ECHO_INSTALL_DIR.Trim().Length -gt 0) {
        $root = $env:ECHO_INSTALL_DIR.Trim()
    }
    $full = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($root)
    $trimmed = $full.TrimEnd("\", "/")
    $driveRoot = [System.IO.Path]::GetPathRoot($full)
    if ($trimmed.Length -eq 0 -or ($driveRoot -and $driveRoot.TrimEnd("\", "/") -eq $trimmed)) {
        Write-Error "echo: refusing to use the drive root $full - set ECHO_INSTALL_DIR to a directory of its own."
        exit 1
    }
    return $trimmed
}

function Get-InstallManifest([string]$Root) {
    $path = Join-Path $Root $ManifestName
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        return $null
    }
    return @(Get-Content -LiteralPath $path | Where-Object { $_ -and $_.Trim().Length -gt 0 })
}

function Test-ManifestEntry([string]$Entry) {
    if ([System.IO.Path]::IsPathRooted($Entry)) {
        return $false
    }
    foreach ($part in $Entry.Split([char[]]@("\", "/"))) {
        if ($part -eq "..") {
            return $false
        }
    }
    return $true
}

function Remove-ManifestFiles([string]$Root, [string[]]$Entries) {
    $dirs = @{}
    foreach ($entry in $Entries) {
        if (-not (Test-ManifestEntry $entry)) {
            continue
        }
        $path = Join-Path $Root $entry
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            Remove-Item -LiteralPath $path -Force
        }
        $dir = Split-Path -Parent $path
        while ($dir.Length -gt $Root.Length) {
            $dirs[$dir] = $true
            $dir = Split-Path -Parent $dir
        }
    }
    foreach ($dir in ($dirs.Keys | Sort-Object { $_.Length } -Descending)) {
        if ((Test-Path -LiteralPath $dir -PathType Container) -and -not (Get-ChildItem -LiteralPath $dir -Force | Select-Object -First 1)) {
            Remove-Item -LiteralPath $dir -Force
        }
    }
}

function Get-EchoBinDir([string]$Root) {
    return Join-Path $Root "bin"
}

function Broadcast-Environment {
    try {
        Add-Type -Namespace EchoInstall -Name Native -MemberDefinition @"
        [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Auto)]
        public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);
"@ -ErrorAction SilentlyContinue
        $HWND_BROADCAST = [IntPtr]0xffff
        $WM_SETTINGCHANGE = 0x1a
        $result = [UIntPtr]::Zero
        [void][EchoInstall.Native]::SendMessageTimeout($HWND_BROADCAST, $WM_SETTINGCHANGE, [UIntPtr]::Zero, "Environment", 2, 5000, [ref]$result)
    }
    catch {
    }
}

function Get-UserPath {
    $key = Get-Item -LiteralPath "HKCU:\Environment" -ErrorAction SilentlyContinue
    if ($null -eq $key) {
        return ""
    }
    $path = $key.GetValue("Path", "", [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    if ($null -eq $path) {
        return ""
    }
    return [string]$path
}

function Set-UserPath([string]$Path) {
    $key = "HKCU:\Environment"
    if (-not (Test-Path -LiteralPath $key)) {
        New-Item -Path $key -Force | Out-Null
    }
    $existing = Get-ItemProperty -LiteralPath $key -Name Path -ErrorAction SilentlyContinue
    if ($null -eq $existing) {
        New-ItemProperty -LiteralPath $key -Name Path -Value $Path -PropertyType ExpandString | Out-Null
    }
    else {
        Set-ItemProperty -LiteralPath $key -Name Path -Value $Path -Type ExpandString
    }
    Broadcast-Environment
}

function Add-UserPath([string]$Dir) {
    $current = Get-UserPath
    $parts = @()
    if ($current.Length -gt 0) {
        $parts = $current.Split(";", [System.StringSplitOptions]::RemoveEmptyEntries)
    }
    foreach ($part in $parts) {
        if ($part.TrimEnd("\", "/").ToLowerInvariant() -eq $Dir.TrimEnd("\", "/").ToLowerInvariant()) {
            return
        }
    }
    if ($current.Length -eq 0) {
        Set-UserPath $Dir
    }
    else {
        Set-UserPath ($current.TrimEnd(";") + ";" + $Dir)
    }
}

function Remove-UserPath([string]$Dir) {
    $current = Get-UserPath
    if ($current.Length -eq 0) {
        return
    }
    $want = $Dir.TrimEnd("\", "/").ToLowerInvariant()
    $kept = @()
    foreach ($part in $current.Split(";", [System.StringSplitOptions]::RemoveEmptyEntries)) {
        if ($part.TrimEnd("\", "/").ToLowerInvariant() -ne $want) {
            $kept += $part
        }
    }
    Set-UserPath ($kept -join ";")
}

$InstallRoot = Get-EchoInstallRoot
$BinDir = Get-EchoBinDir $InstallRoot

if ($Uninstall) {
    $entries = Get-InstallManifest $InstallRoot
    if ($null -eq $entries) {
        if (Test-Path -LiteralPath $InstallRoot) {
            Write-Error "echo: $InstallRoot has no $ManifestName, so there is no record of what this script installed there. Nothing was removed. Re-run the installer once to record it, use Apps & Features for a setup-wizard install, or delete the directory by hand."
            exit 1
        }
    }
    else {
        Remove-ManifestFiles $InstallRoot $entries
        Remove-Item -LiteralPath (Join-Path $InstallRoot $ManifestName) -Force
        if (-not (Get-ChildItem -LiteralPath $InstallRoot -Force | Select-Object -First 1)) {
            Remove-Item -LiteralPath $InstallRoot -Force
        }
    }
    Remove-UserPath $BinDir
    Write-Host "uninstalled Echo from $InstallRoot"
    exit 0
}

$arch = $env:PROCESSOR_ARCHITECTURE
if ($env:PROCESSOR_ARCHITEW6432) {
    $arch = $env:PROCESSOR_ARCHITEW6432
}
if ($arch -ne "AMD64") {
    Write-Error "echo: no prebuilt Echo for windows $arch.`nbuild it from source instead: https://github.com/$Repo"
    exit 1
}

$asset = "echo-windows-x86_64"
# resolve the tag once: two requests against /latest can see two releases
$tag = $null
try {
    $latest = & curl.exe -fsSLI -o NUL -w "%{url_effective}" "https://github.com/$Repo/releases/latest" 2>$null
    if ($LASTEXITCODE -eq 0 -and $latest) {
        $tag = ($latest.Trim().TrimEnd("/") -split "/")[-1]
    }
}
catch {
}
if (-not $tag) {
    try {
        $req = [System.Net.HttpWebRequest]::Create("https://github.com/$Repo/releases/latest")
        $req.AllowAutoRedirect = $false
        $req.Method = "HEAD"
        $resp = $req.GetResponse()
        $location = $resp.Headers["Location"]
        $resp.Close()
        if ($location) {
            $tag = ($location.TrimEnd("/") -split "/")[-1]
        }
    }
    catch {
        if ($_.Exception.Response -and $_.Exception.Response.Headers["Location"]) {
            $tag = ($_.Exception.Response.Headers["Location"].TrimEnd("/") -split "/")[-1]
        }
    }
}
if (-not $tag) {
    Write-Error "echo: could not resolve the latest release at https://github.com/$Repo/releases/latest"
    exit 1
}
$base = "https://github.com/$Repo/releases/download/$tag"
$url = "$base/$asset.zip"
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("echo-install-" + [guid]::NewGuid().ToString("n"))
New-Item -ItemType Directory -Path $tmp | Out-Null

try {
    Write-Host "downloading $asset ..."
    $zip = Join-Path $tmp "$asset.zip"
    try {
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    }
    catch {
        Write-Error "echo: could not download $url`nsee https://github.com/$Repo/releases for what is available."
        exit 1
    }

    # checked before it is unpacked: an archive SHA256SUMS does not vouch for is never expanded
    $sums = Join-Path $tmp "SHA256SUMS"
    try {
        Invoke-WebRequest -Uri "$base/SHA256SUMS" -OutFile $sums -UseBasicParsing
    }
    catch {
        Write-Error "echo: could not download SHA256SUMS, so $asset.zip cannot be checked."
        exit 1
    }

    $expected = $null
    foreach ($line in Get-Content -LiteralPath $sums) {
        $fields = $line -split '\s+', 2
        if ($fields.Count -eq 2 -and $fields[1].TrimStart('*') -eq "$asset.zip") {
            $expected = $fields[0].ToLowerInvariant()
        }
    }

    $actual = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    if (-not $expected -or $expected -ne $actual) {
        Write-Error "echo: $asset.zip does not match the release's SHA256SUMS - nothing was installed."
        exit 1
    }

    if ($env:ECHO_REQUIRE_ATTESTATION -eq "1") {
        if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
            Write-Error "echo: ECHO_REQUIRE_ATTESTATION=1 needs gh on PATH."
            exit 1
        }
        & gh attestation verify $zip --repo $Repo *> $null
        if ($LASTEXITCODE -ne 0) {
            Write-Error "echo: $asset.zip has no valid build attestation from $Repo - nothing was installed."
            exit 1
        }
    }
    elseif (Get-Command gh -ErrorAction SilentlyContinue) {
        & gh attestation verify $zip --repo $Repo *> $null
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "echo: no valid build attestation from $Repo; SHA256SUMS still matched, installing."
        }
    }

    Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force

    $extractedBin = Join-Path $tmp "bin"
    if (-not (Test-Path -LiteralPath (Join-Path $extractedBin "echoc.exe"))) {
        Write-Error "echo: the downloaded archive does not contain bin\echoc.exe."
        exit 1
    }
    if (-not (Test-Path -LiteralPath (Join-Path $extractedBin "epm.exe"))) {
        Write-Error "echo: the downloaded archive does not contain bin\epm.exe."
        exit 1
    }

    New-Item -ItemType Directory -Path $InstallRoot -Force | Out-Null
    $previous = Get-InstallManifest $InstallRoot
    $installed = @()
    foreach ($name in @("bin", "lib", "sysroot")) {
        $src = Join-Path $tmp $name
        if (Test-Path -LiteralPath $src) {
            $srcFull = (Get-Item -LiteralPath $src).FullName.TrimEnd("\", "/")
            foreach ($file in Get-ChildItem -LiteralPath $srcFull -Recurse -File -Force) {
                $relative = $name + "\" + $file.FullName.Substring($srcFull.Length).TrimStart("\", "/")
                $dest = Join-Path $InstallRoot $relative
                New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force | Out-Null
                Copy-Item -LiteralPath $file.FullName -Destination $dest -Force
                $installed += $relative
            }
        }
    }
    foreach ($name in @("README.md", "LICENSE")) {
        $src = Join-Path $tmp $name
        if (Test-Path -LiteralPath $src) {
            Copy-Item -LiteralPath $src -Destination (Join-Path $InstallRoot $name) -Force
            $installed += $name
        }
    }

    if ($null -ne $previous) {
        $current = @{}
        foreach ($entry in $installed) {
            $current[$entry.ToLowerInvariant()] = $true
        }
        $stale = @($previous | Where-Object { -not $current.ContainsKey($_.ToLowerInvariant()) })
        Remove-ManifestFiles $InstallRoot $stale
    }
    Set-Content -LiteralPath (Join-Path $InstallRoot $ManifestName) -Value $installed -Encoding UTF8

    Add-UserPath $BinDir

    $echocVersion = & (Join-Path $BinDir "echoc.exe") --version
    $epmVersion = & (Join-Path $BinDir "epm.exe") --version
    Write-Host "installed echoc $echocVersion to $(Join-Path $BinDir 'echoc.exe')"
    Write-Host "installed epm $epmVersion to $(Join-Path $BinDir 'epm.exe')"
    Write-Host "open a new terminal so PATH picks this up."
}
finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
