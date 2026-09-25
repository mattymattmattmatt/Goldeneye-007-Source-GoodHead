# Builds the player package: packages\GESVR-<version>.zip, holding exactly what
# Launch-GESVR needs -- the launcher, README and dist\ -- plus the licences of
# what d3d9.dll contains. Build Release | Win32 first (dist\d3d9.dll).
#
#   powershell -ExecutionPolicy Bypass -File tools\Make-Release.ps1 [-Name GESVR-v0.4]
#
# The default name is git's description of the checkout ("v0.3-sharper",
# "v0.3-sharper-2-gabc1234", "...-dirty" with uncommitted changes), so a
# package cannot pass itself off as a tagged version it is not.
param([string]$Name = "")
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dist = Join-Path $root "dist"
if (-not (Test-Path (Join-Path $dist "d3d9.dll"))) {
    throw "dist\d3d9.dll not found. Build l4d2vr.sln (Release | Win32) first."
}
if (-not $Name) {
    $desc = ""
    try { $desc = (& git -C $root describe --tags --always --dirty 2>$null) } catch { }
    $Name = if ($desc) { "GESVR-$desc" } else { "GESVR-" + (Get-Date -Format "yyyyMMdd") }
}

# Not "release": on Windows that is the same folder as MSBuild's Release\.
$out = Join-Path $root "packages"
$stage = Join-Path $out $Name
$zip = "$stage.zip"
New-Item -ItemType Directory -Force -Path $out | Out-Null
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
if (Test-Path $zip) { Remove-Item $zip -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

foreach ($f in @("Launch-GESVR.bat", "Launch-GESVR.ps1", "README.md")) {
    Copy-Item (Join-Path $root $f) (Join-Path $stage $f)
}
Copy-Item $dist (Join-Path $stage "dist") -Recurse

# Licences for what ships inside d3d9.dll and next to it.
$lic = Join-Path $stage "licenses"
New-Item -ItemType Directory -Force -Path $lic | Out-Null
Copy-Item (Join-Path $root "dxvk\LICENSE") (Join-Path $lic "DXVK.txt")
Copy-Item (Join-Path $root "dxvk\include\openvr\LICENSE") (Join-Path $lic "OpenVR.txt")
Copy-Item (Join-Path $root "L4D2VR\sdk\LICENSE.txt") (Join-Path $lic "Source-SDK.txt")
# MinHook's licence is the comment block at the top of its header.
$mh = Get-Content (Join-Path $root "thirdparty\minhook\include\MinHook.h")
$end = [Array]::IndexOf($mh, ($mh | Where-Object { $_ -match '^\s*\*/' } | Select-Object -First 1))
($mh[0..$end] | ForEach-Object { $_ -replace '^\s*/?\*+/?\s?', '' }) | Set-Content (Join-Path $lic "MinHook.txt") -Encoding ASCII

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true)

$files = Get-ChildItem $stage -Recurse -File
$list = $files | ForEach-Object { "  " + $_.FullName.Substring($stage.Length + 1) }

# The staging folder has served its purpose. Leaving it behind next to the zip
# made it easy to miss which of the two was the download.
Remove-Item $stage -Recurse -Force

Write-Host ""
Write-Host ("  {0}" -f $zip) -ForegroundColor Green
Write-Host ("  {0} files, {1:N1} MB" -f $files.Count, ((Get-Item $zip).Length / 1MB))
Write-Host ""
$list
