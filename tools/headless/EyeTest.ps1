# One headless run: launch straight into a map, let EyeDiag trace two frames at
# the D3D9 device and dump both eye images, then collect everything.
#
#   EyeTest.ps1 -Name eyert -Set "EyeRenderTargets=true|AntiAliasing=0"
#   EyeTest.ps1 -Name ingame -DelaySec 30 -Set "EyeDiagCommands=jointeam 0; joinclass bond"
#   EyeTest.ps1 -Name menu -MainMenu -Set "EyeDiagMenuSec=25"          main menu images
#   add "EyeDiagDisconnect=true" to an in-map run for the menu after leaving a map
#
# Needs VRNull.ps1 -Mode on first (or a headset on someone's head). Starts from
# the player's own config.txt, backed up once and never overwritten, appends the
# overrides (the parser keeps the LAST duplicate key), and restores it after.
# Separate overrides with '|' or ','; ';' is left alone because ExtraCvars and
# EyeDiagCommands values are ';'-separated console commands.
#
# Results land in tools\headless\results\<Name>: log.txt (this run's slice of
# vrmod_log.txt) and gesvr_eye_L/R.bmp (1024 wide). Summarise.py reads them.
#
# To get past the join screen with no controllers: GE:S's Join button sends
# "jointeam", and "joinclass <character>" spawns you. ExtraCvars cannot do it --
# it waits for menus to close, and the join screen is a menu.
param(
    [Parameter(Mandatory)][string]$Name,
    [string[]]$Set = @(),
    [string]$Map = "ge_archives",
    [switch]$MainMenu,
    [int]$DelaySec = 20,
    [int]$TimeoutSec = 300
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")
$cfg    = Join-Path $GesvrSdk "bin\VR\config.txt"
$backup = "$cfg.gesvr-eyetest-backup"
$log    = Join-Path $GesvrSdk "bin\vrmod_log.txt"
$out    = Join-Path $GesvrResults $Name

if (-not (Test-Path $backup)) { Copy-Item $cfg $backup; Write-Host "Backed up config.txt" }
Copy-Item $backup $cfg -Force

$Set = @($Set | ForEach-Object { $_ -split '[,|]' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$lines = @("", "# ---- EyeTest '$Name' overrides (removed after the run) ----",
           "EyeDiag=true", "EyeDiagQuit=true", "EyeDiagDelaySec=$DelaySec") + $Set
Add-Content -Path $cfg -Value $lines -Encoding ASCII

New-Item -ItemType Directory -Force -Path $out | Out-Null
Get-ChildItem $env:TEMP -Filter "gesvr_eye_*.bmp" -ErrorAction SilentlyContinue | Remove-Item -Force
$logStart = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }

try {
    # -MainMenu stays at the main menu (pair it with EyeDiagMenuSec=N). An empty
    # -ExtraArgs would reach the launcher as a bare switch, so it is left out.
    $launch = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $GesvrRepo "Launch-GESVR.ps1"))
    if (-not $MainMenu) { $launch += @("-ExtraArgs", "+map $Map") }
    Write-Host "[$Name] launching into $(if ($MainMenu) { 'the main menu' } else { $Map }) with: $($Set -join ', ')"
    & powershell @launch | Out-Null

    $t0 = Get-Date
    $started = $false
    while (((Get-Date) - $t0).TotalSeconds -lt 120) {
        if (Get-Process -Name hl2 -ErrorAction SilentlyContinue) { $started = $true; break }
        Start-Sleep -Seconds 2
    }
    if (-not $started) { Write-Host "[$Name] hl2.exe never started" }
    $timedOut = $false
    while ($started -and (Get-Process -Name hl2 -ErrorAction SilentlyContinue)) {
        if (((Get-Date) - $t0).TotalSeconds -gt $TimeoutSec) {
            $timedOut = $true
            Get-Process -Name hl2 -ErrorAction SilentlyContinue | Stop-Process -Force
            break
        }
        Start-Sleep -Seconds 2
    }
    $elapsed = [int]((Get-Date) - $t0).TotalSeconds
    Start-Sleep -Seconds 2

    if (Test-Path $log) {
        $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
        try {
            if ($fs.Length -lt $logStart) { $logStart = 0 }
            [void]$fs.Seek($logStart, 'Begin')
            [IO.File]::WriteAllText((Join-Path $out "log.txt"), (New-Object IO.StreamReader($fs)).ReadToEnd())
        } finally { $fs.Close() }
    }
    Get-ChildItem $env:TEMP -Filter "gesvr_eye_*.bmp" -ErrorAction SilentlyContinue | Copy-Item -Destination $out -Force
    $bmps = @(Get-ChildItem $out -Filter "*.bmp" -ErrorAction SilentlyContinue).Count
    Write-Host "[$Name] done in ${elapsed}s, timedOut=$timedOut, images=$bmps -> $out"
}
finally {
    Copy-Item $backup $cfg -Force
}
