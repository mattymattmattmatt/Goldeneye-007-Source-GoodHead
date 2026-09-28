# Paths shared by the headless test scripts. Same discovery as Launch-GESVR.ps1:
# Steam from the registry, the SDK from whichever library has app 218.

function Get-GesvrSteamPath {
    $reg = Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue
    if ($reg -and $reg.SteamPath) { return ($reg.SteamPath -replace '/', '\') }
    throw "Steam not found in the registry"
}

function Get-GesvrSdkPath([string]$steamPath) {
    $libs = @($steamPath)
    $vdf = Join-Path $steamPath "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        foreach ($line in Get-Content $vdf) {
            if ($line -match '"path"\s+"([^"]+)"') { $libs += ($Matches[1] -replace '\\\\', '\') }
        }
    }
    foreach ($lib in $libs) {
        $p = Join-Path $lib "steamapps\common\Source SDK Base 2007"
        if ((Test-Path (Join-Path $lib "steamapps\appmanifest_218.acf")) -and (Test-Path (Join-Path $p "hl2.exe"))) { return $p }
    }
    throw "Source SDK Base 2007 not found"
}

$GesvrSteam = Get-GesvrSteamPath
$GesvrSdk = Get-GesvrSdkPath $GesvrSteam
$GesvrRepo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$GesvrResults = Join-Path $PSScriptRoot "results"
