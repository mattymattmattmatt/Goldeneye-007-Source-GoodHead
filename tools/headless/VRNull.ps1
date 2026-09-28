# Switch SteamVR to its built-in null (virtual) headset so the mod can be run and
# tested with nobody in the headset -- and back.
#
#   VRNull.ps1 -Mode on       back up steamvr.vrsettings, enable the null HMD
#   VRNull.ps1 -Mode off      put the original settings back exactly
#   VRNull.ps1 -Mode status
#
# The null HMD reports the render size below (what a Quest 3 at 1.3x SteamVR
# supersampling asks for) and a symmetric frustum, so the eye-target crop is
# 1.0 -- close to a real headset, not identical. It has NO controllers: aim dot,
# reticle and anything hand-driven cannot be judged this way.
#
# SteamVR rewrites steamvr.vrsettings when it exits, so it is always stopped
# BEFORE the file is touched, in both directions. ALWAYS run "-Mode off" when
# done, or the real headset will not be found next time.
param([ValidateSet("on", "off", "status")][string]$Mode = "status")

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")
$cfg    = Join-Path $GesvrSteam "config\steamvr.vrsettings"
$backup = "$cfg.gesvr-nulltest-backup"

function Stop-SteamVR {
    $names = "vrmonitor", "vrserver", "vrcompositor", "vrdashboard", "vrstartup", "vrwebhelper", "vrprismhost"
    $procs = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $names -contains $_.ProcessName })
    if ($procs.Count -eq 0) { return }
    Write-Host "Stopping SteamVR ($($procs.Count) processes)"
    $procs | Stop-Process -Force -ErrorAction SilentlyContinue
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline -and (Get-Process -ErrorAction SilentlyContinue | Where-Object { $names -contains $_.ProcessName })) {
        Start-Sleep -Milliseconds 500
    }
    Start-Sleep -Seconds 1
}

if ($Mode -eq "status") {
    Write-Host "backup present: $(Test-Path $backup)"
    python -c "import json;d=json.load(open(r'$cfg'));print('steamvr:',{k:d['steamvr'].get(k) for k in ('forcedDriver','requireHmd','activateMultipleDrivers')});print('driver_null:',d.get('driver_null'))"
    return
}

Stop-SteamVR

if ($Mode -eq "on") {
    if (-not (Test-Path $backup)) { Copy-Item $cfg $backup; Write-Host "Backed up $cfg" }
    python -c @"
import json
p = r'$cfg'
d = json.load(open(p, encoding='utf-8'))
s = d.setdefault('steamvr', {})
s['forcedDriver'] = 'null'; s['requireHmd'] = False; s['activateMultipleDrivers'] = True
d['driver_null'] = {'enable': True, 'serialNumber': 'GESVR Test HMD', 'modelNumber': 'GESVR Null',
                    'windowX': 0, 'windowY': 0, 'windowWidth': 1280, 'windowHeight': 720,
                    'renderWidth': 2688, 'renderHeight': 2880,
                    'secondsFromVsyncToPhotons': 0.01111111, 'displayFrequency': 72.0}
json.dump(d, open(p, 'w', encoding='utf-8'), indent=3)
print('null headset on')
"@
}
elseif (Test-Path $backup) {
    Copy-Item $backup $cfg -Force
    Remove-Item $backup -Force
    Write-Host "Restored the real SteamVR settings"
}
else { Write-Host "No backup present; nothing to restore" }
