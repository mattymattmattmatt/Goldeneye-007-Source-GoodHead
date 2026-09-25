# GoldenEye: Source VR launcher
# Installs d3d9.dll next to hl2.exe, then starts GE:S *through Steam*
# (SDK 2007 will not find VPK materials if you run hl2.exe by itself).

$ErrorActionPreference = "Stop"

function Get-SteamPath {
    $reg = Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue
    if ($reg -and $reg.SteamPath) { return $reg.SteamPath }
    $def = "C:\Program Files (x86)\Steam"
    if (Test-Path $def) { return $def }
    throw "Steam is not installed (no HKCU:\Software\Valve\Steam)."
}

function Get-SteamLibraries([string]$steamPath) {
    $libs = New-Object System.Collections.Generic.List[string]
    $libs.Add((Resolve-Path $steamPath).Path)
    $vdf = Join-Path $steamPath "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        foreach ($line in Get-Content $vdf) {
            if ($line -match '"path"\s+"([^"]+)"') {
                $p = $Matches[1] -replace '\\\\', '\'
                if (Test-Path $p) { $libs.Add($p) }
            }
        }
    }
    return $libs | Select-Object -Unique
}

function Find-Sdk2007([string[]]$libs) {
    # Prefer the library Steam actually has the app registered in. A reinstall
    # can land in a different library and leave the old folder behind (that is
    # what happened on 2026-09-24), and installing the mod into the orphan would
    # mean Steam launches a game with no VR in it and no obvious reason why.
    foreach ($lib in $libs) {
        $p = Join-Path $lib "steamapps\common\Source SDK Base 2007"
        if ((Test-Path (Join-Path $lib "steamapps\appmanifest_218.acf")) -and
            (Test-Path (Join-Path $p "hl2.exe"))) { return (Resolve-Path $p).Path }
    }
    foreach ($lib in $libs) {
        $p = Join-Path $lib "steamapps\common\Source SDK Base 2007"
        if (Test-Path (Join-Path $p "hl2.exe")) { return (Resolve-Path $p).Path }
    }
    return $null
}

function Find-GESource([string[]]$libs, [string]$steamPath) {
    $candidates = @()
    $candidates += Join-Path $steamPath "steamapps\sourcemods\gesource"
    foreach ($lib in $libs) {
        $candidates += Join-Path $lib "steamapps\sourcemods\gesource"
    }
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c "gameinfo.txt")) { return (Resolve-Path $c).Path }
    }
    return $null
}

function Ensure-Junction([string]$link, [string]$target) {
    if (Test-Path (Join-Path $link "gameinfo.txt")) { return }
    if (Test-Path $link) { cmd /c "rmdir `"$link`"" | Out-Null }
    Write-Host "Creating junction: $link -> $target"
    $p = Start-Process -FilePath "cmd.exe" -ArgumentList "/c mklink /J `"$link`" `"$target`"" -Wait -PassThru -WindowStyle Hidden
    if ($p.ExitCode -ne 0 -or -not (Test-Path (Join-Path $link "gameinfo.txt"))) {
        throw "Could not create junction $link"
    }
}

function Disable-SteamVRTheater([string]$steamPath) {
    $cfg = Join-Path $steamPath "config\steamvr.vrsettings"
    if (-not (Test-Path $cfg)) {
        Write-Host "No steamvr.vrsettings yet (start SteamVR once, then re-run)."
        return
    }
    try {
        $raw = [System.IO.File]::ReadAllText($cfg)
        $j = $raw | ConvertFrom-Json
        if (-not $j.dashboard) {
            $j | Add-Member -NotePropertyName dashboard -NotePropertyValue ([pscustomobject]@{}) -Force
        }
        $j.dashboard | Add-Member -NotePropertyName autoShowGameTheater -NotePropertyValue $false -Force
        $j.dashboard | Add-Member -NotePropertyName enableGameTheater -NotePropertyValue $false -Force
        $json = $j | ConvertTo-Json -Depth 30
        $utf8 = New-Object System.Text.UTF8Encoding $false
        [System.IO.File]::WriteAllText($cfg, $json, $utf8)
        Write-Host "SteamVR Theater disabled (dashboard.autoShowGameTheater = false)"
    } catch {
        Write-Host "Could not patch steamvr.vrsettings: $_"
        Write-Host "Do this by hand: SteamVR Settings > Show Advanced > Dashboard >"
        Write-Host "  Present Non-VR Applications on Theater Screen Upon Launch = Off"
    }
}

function Start-SteamVR([string]$steamPath) {
    $vrMon = Get-Process -Name "vrmonitor" -ErrorAction SilentlyContinue
    if ($vrMon) {
        Write-Host "SteamVR is already running."
        return
    }
    $steamExe = Join-Path $steamPath "steam.exe"
    Write-Host "Starting SteamVR..."
    Start-Process $steamExe "steam://rungameid/250820" | Out-Null
    $deadline = (Get-Date).AddSeconds(25)
    while ((Get-Date) -lt $deadline) {
        if (Get-Process -Name "vrmonitor" -ErrorAction SilentlyContinue) { return }
        Start-Sleep -Seconds 1
    }
    Write-Host "SteamVR did not report ready; launching GE:S anyway. Put the headset on first if you can."
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$dist = Join-Path $root "dist"
if (-not (Test-Path (Join-Path $dist "d3d9.dll"))) {
    throw "dist\d3d9.dll not found. Build GESVR.sln (Release | Win32) first."
}

$steam = Get-SteamPath
$libs = @(Get-SteamLibraries $steam)
$sdk = Find-Sdk2007 $libs
$ges = Find-GESource $libs $steam

if (-not $sdk) {
    Write-Host "Source SDK Base 2007 is not installed."
    Start-Process "$steam\steam.exe" "steam://install/218"
    throw "Install Source SDK Base 2007, then run this launcher again."
}

if (-not $ges) {
    Write-Host "GoldenEye: Source was not found in steamapps\sourcemods\gesource."
    throw "Install GoldenEye: Source into $steam\steamapps\sourcemods\gesource"
}

Write-Host "Steam:     $steam"
Write-Host "SDK 2007:  $sdk"
Write-Host "GE:S:      $ges"

Disable-SteamVRTheater $steam
Start-SteamVR $steam

function Install-D3d9([string]$dir) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Copy-Item (Join-Path $dist "d3d9.dll") (Join-Path $dir "d3d9.dll") -Force
    Copy-Item (Join-Path $dist "openvr_api.dll") (Join-Path $dir "openvr_api.dll") -Force
}

Install-D3d9 $sdk
Install-D3d9 (Join-Path $sdk "bin")

# Empty .local file forces Windows to prefer DLLs next to hl2.exe over SysWOW64.
$local = Join-Path $sdk "hl2.exe.local"
if (-not (Test-Path $local)) { New-Item -ItemType File -Path $local | Out-Null }

# Refuse to launch a modified hl2.exe. Setting the large-address-aware bit (a
# "4 GB patch", PE Characteristics 0x0102 -> 0x0122) makes the game die on
#     Error!  SteamStartup() failed: SteamAPI_Init_Internal failed
# and ONE launch like that leaves Steam refusing the app at that install
# location even after the exe is restored byte for byte -- verifying files,
# clearing Steam's cache, rebooting and reinstalling into the same library all
# fail to clear it. What does clear it is moving the install to a different
# Steam library (Properties > Installed Files > Move install folder). Proved by
# experiment on 2026-09-24; see HANDOFF.md. So never let it launch at all.
$hl2 = Join-Path $sdk "hl2.exe"
if (Test-Path $hl2) {
    $hdr = [System.IO.File]::ReadAllBytes($hl2)
    $peAt = [BitConverter]::ToInt32($hdr, 0x3C)
    $chars = [BitConverter]::ToUInt16($hdr, $peAt + 4 + 18)
    if (($chars -band 0x20) -ne 0) {
        Write-Host ""
        Write-Host "STOP: hl2.exe has been modified (large-address-aware / 4 GB patch)." -ForegroundColor Red
        Write-Host "Launching it makes Steam refuse the game at this install location," -ForegroundColor Red
        Write-Host "even after the file is put back. Restore it first:" -ForegroundColor Red
        Write-Host "  Steam > Library > Source SDK Base 2007 > Properties > Installed Files > Verify integrity"
        Write-Host "If the game already shows 'SteamStartup() failed', also use"
        Write-Host "  ... > Installed Files > Move install folder  (to any other Steam library)"
        exit 1
    }
}

# (The SDK's own 2007-era steamclient.dll next to hl2.exe is left alone. It was
# suspected during the 2026-09-23 "SteamStartup() failed" hunt, but a fresh,
# healthy install runs with it in place; the real cause was a modified hl2.exe,
# guarded against above.)

# Source loads bin\binkw32.dll by name from engine.dll *before* it asks for d3d9.
# Proxy pre-loads our d3d9.dll by full path so SysWOW64 cannot win.
$bin = Join-Path $sdk "bin"
$binkProxy = Join-Path $dist "binkw32.dll"
if (Test-Path $binkProxy) {
    $orig = Join-Path $bin "binkw32_orig.dll"
    $current = Join-Path $bin "binkw32.dll"
    if (-not (Test-Path $orig) -and (Test-Path $current)) {
        Copy-Item $current $orig -Force
        Write-Host "Backed up original Bink to bin\binkw32_orig.dll"
    }
    Copy-Item $binkProxy $current -Force
    Write-Host "Installed Bink preload proxy (forces our d3d9.dll to load)"
}

# DXVK's own settings, read from hl2.exe's folder (see dist\dxvk.conf). Installed
# once; after that the player's copy is kept, like config.txt.
$dxvkConf = Join-Path $dist "dxvk.conf"
if ((Test-Path $dxvkConf) -and -not (Test-Path (Join-Path $sdk "dxvk.conf"))) {
    Copy-Item $dxvkConf (Join-Path $sdk "dxvk.conf") -Force
    Write-Host "Installed dxvk.conf (DXVK memory setting for this 32-bit game)"
}

# Install the VR folder, but NEVER clobber config.txt.
#
# This used to copy VR\* with -Force on every launch, which overwrote the
# player's config every single time -- so any setting tuned by hand silently
# reverted on the next run, and every knob in that file was effectively
# read-only. The manifest and action bindings still get refreshed; config.txt
# is written only when it is missing.
foreach ($vrDest in @((Join-Path $sdk "VR"), (Join-Path $sdk "bin\VR"))) {
    New-Item -ItemType Directory -Force -Path $vrDest | Out-Null
    Get-ChildItem (Join-Path $dist "VR") -Force | ForEach-Object {
        $target = Join-Path $vrDest $_.Name
        # weapons.txt holds the player's numpad-tuned weapon positions.
        if ($_.Name -ieq "config.txt" -or $_.Name -ieq "weapons.txt") {
            if (-not (Test-Path $target)) {
                Copy-Item $_.FullName $target -Force
                Write-Host "Installed default $($_.Name) (yours will be kept from now on)"
            }
        } elseif ($_.PSIsContainer) {
            # Copy-Item -Recurse puts a folder INSIDE an existing folder of the
            # same name, so every bindings update after the first install landed
            # in SteamVRActionManifest\SteamVRActionManifest\ -- which SteamVR
            # never reads (it kept running the August bindings). Copy the
            # contents over the folder instead, and clear that stray copy.
            New-Item -ItemType Directory -Force -Path $target | Out-Null
            $stray = Join-Path $target $_.Name
            if (Test-Path $stray) { Remove-Item $stray -Recurse -Force }
            Copy-Item (Join-Path $_.FullName "*") $target -Recurse -Force
        } else {
            Copy-Item $_.FullName $target -Force
        }
    }
}

# Spaceless -game path so Source doesn't truncate "Program Files" / "Source SDK
# Base 2007". A junction named gesource inside the SDK folder lets us pass the
# RELATIVE "-game gesource", which Source resolves against hl2.exe's folder --
# no spaces in the argument and no drive letter. (This used to be a junction at
# a hard-coded G:\gesource, which only worked on the machine it was written on:
# anywhere without a G: drive the launcher stopped at "Could not create
# junction".) Copy or delete the SDK folder with junctions excluded (robocopy
# /XJ; rmdir the link first) -- this one points at the real GE:S install.
$gameLink = "gesource"
Ensure-Junction (Join-Path $sdk $gameLink) $ges

# Add "VR Settings" to the GE:S main and pause menus. The entry runs
# "engine echo gesvr_vrsettings", which the mod catches in the console output
# and opens its settings panel. Idempotent; the untouched original is kept as
# GameMenu.res.gesvr-orig (a GE:S update that replaces the file is re-patched
# on the next launch).
$gameMenu = Join-Path $ges "resource\GameMenu.res"
if (Test-Path $gameMenu) {
    $menuText = [IO.File]::ReadAllText($gameMenu)
    if ($menuText -notmatch "gesvr_vrsettings") {
        $menuOrig = "$gameMenu.gesvr-orig"
        if (-not (Test-Path $menuOrig)) { Copy-Item $gameMenu $menuOrig }
        $entry = "`t`"GESVR`"`r`n`t{`r`n`t`t`"label`" `"VR Settings`"`r`n`t`t`"command`" `"engine echo gesvr_vrsettings`"`r`n`t}`r`n"
        # Just above Options; if that block is not found, first in the menu.
        $m = [regex]::Match($menuText, '(?m)^[ \t]*"[^"]*"\s*\{[^{}]*"OpenOptionsDialog"[^{}]*\}')
        if ($m.Success) {
            $menuText = $menuText.Insert($m.Index, $entry)
        } else {
            $brace = $menuText.IndexOf('{')
            $menuText = $menuText.Insert($brace + 1, "`r`n" + $entry)
        }
        [IO.File]::WriteAllText($gameMenu, $menuText)
        Write-Host "Added 'VR Settings' to the GE:S main menu"
    }
}

# --- GoodHead main menu -----------------------------------------------------
# The splash behind the menu and the track that plays over it.
#
# The background is not ours to name: the engine hard-codes it. engine.dll holds
# the literal strings "materials/console/background01.vtf" and
# "materials/console/background01_widescreen.vtf" and uses them whenever
# scripts\ChapterBackgrounds.txt is absent, which it is in GE:S. So the only way
# in is to be those two files. Both are VTF 7.2 DXT1 with a DXT1 thumbnail and
# come out the same byte length as the originals -- see tools\Make-MenuAssets.py,
# which also explains why the widescreen one is 2048x1024 rather than 16:9.
#
# The music is GE:S's own system, not the engine's: its client.dll reads
# "scripts/music/level_music_%s" and plays one entry from the list at random, so
# _menu is the menu playlist. (The engine's own menu music never fires here --
# gameui.dll globs sound/ui/gamestartup*.mp3 and GE:S ships only a .wav there.)
#
# Every GE:S file this replaces is copied to <name>.gesvr-orig first. Set
# $goodheadMenu to $false and run the launcher once to put GE:S back exactly as
# it was; files of ours that had no original are removed, and only when they
# still match ours byte for byte.
$goodheadMenu = $true

$menuSrc = Join-Path $dist "menu"
$menuSkin = @(
    @{ src = "background01.vtf";            dest = "materials\console\background01.vtf" },
    @{ src = "background01_widescreen.vtf"; dest = "materials\console\background01_widescreen.vtf" },
    @{ src = "level_music__menu.txt";       dest = "scripts\music\level_music__menu.txt" },
    @{ src = "goodhead_title.mp3";          dest = "sound\music\goodhead_title.mp3" }
)
function Same-File([string]$a, [string]$b) {
    if (-not (Test-Path $a) -or -not (Test-Path $b)) { return $false }
    return (Get-FileHash $a -Algorithm SHA1).Hash -eq (Get-FileHash $b -Algorithm SHA1).Hash
}
if (Test-Path $menuSrc) {
    $skinned = 0
    foreach ($item in $menuSkin) {
        $src = Join-Path $menuSrc $item.src
        $dest = Join-Path $ges $item.dest
        $orig = "$dest.gesvr-orig"
        if ($goodheadMenu) {
            if (-not (Test-Path $src)) { continue }
            # Keep GE:S's file the first time we stand in for it. Never back up
            # a file that is already ours: goodhead_title.mp3 has no original,
            # so on the second launch the plain "no backup yet" test happily
            # saved our own mp3 as the "original" and the restore below then had
            # something to put back and left the track installed for good.
            if ((Test-Path $dest) -and -not (Test-Path $orig) -and -not (Same-File $src $dest)) {
                Copy-Item $dest $orig -Force
            }
            if (-not (Same-File $src $dest)) {
                New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
                Copy-Item $src $dest -Force
                $skinned++
            }
        } else {
            if (Test-Path $orig) {
                Copy-Item $orig $dest -Force
                Remove-Item $orig -Force
                $skinned++
            } elseif (Same-File $src $dest) {
                # No original means we added this file. Removing it is only safe
                # while it is still ours, so the hash has to match.
                Remove-Item $dest -Force
                $skinned++
            }
        }
    }
    if ($skinned -gt 0) {
        if ($goodheadMenu) {
            Write-Host "Installed the GoodHead menu splash and title music"
        } else {
            Write-Host "Restored the original GE:S menu splash and music"
        }
    }
}

# --- Render resolution ------------------------------------------------------
# EVERYTHING the headset sees is captured from this window, so this is the real
# resolution control -- not any setting inside the game. At 1280x720 each eye was
# upscaled to a ~2496x2688 panel, a 3.7x stretch vertically, which is where the
# jaggies come from.
#
# 1920x1080 is 2.25x the pixels and the most widely supported resolution there
# is. If the GPU can take it, 1600x1200 is actually BETTER for VR despite having
# fewer pixels: the headset eye is taller than it is wide (aspect ~0.96), so
# vertical resolution matters most, and 1200 beats 1080.
#
# RULE: the render resolution CANNOT EXCEED THE DESKTOP RESOLUTION.
# In windowed mode Source cannot create a window larger than the desktop and
# dies during video init, waiting forever for client.dll. Every failure fits:
#   1280x720   fits 1920x1080 desktop -> boots
#   1920x1080  fits exactly           -> boots
#   1600x1200  height 1200 > 1080     -> DOES NOT BOOT
#   1280x1280  height 1280 > 1080     -> DOES NOT BOOT
#   2560x1440  both exceed            -> DOES NOT BOOT
#
# An earlier note here blamed the ASPECT ratio. That was wrong - it only looked
# that way because every taller test also happened to exceed the desktop height.
#
# So 1920x1080 is the ceiling on a 1080p desktop, and raising the window is NOT
# the route to more sharpness. Use EyeRenderTargets + EyeRenderScale instead:
# that renders each eye at a MULTIPLE OF THE WINDOW internally and is not bound
# by the desktop at all.
# Image quality: the launcher no longer overrides the game's own video
# settings. Forcing them caused real damage -- mat_picmip -1 pushes texture
# detail BEYOND the in-game High setting, and in a 32-bit process on top of
# DXVK it hung the NVIDIA driver during texture upload (stack:
# D3D9Initializer::Flush -> submit -> nvoglv32), so maps would not load.
# They also bought nothing: the softness is post-processing, not aliasing.
#
# Set $gesForceGraphics = $true to re-enable the overrides below.
$gesForceGraphics = $false
$gesPicmip = 0      # 0 = high, 1 = medium, 2 = low. -1 is beyond High: avoid.
$gesAA     = 4      # MSAA samples.
$gesAniso  = 8      # anisotropic filtering.

# Window size: the largest 16:9 that fits the primary desktop, capped at
# 2560x1440. On a 1080p desktop that is exactly 1920x1080, as before. It only
# grows when the desktop does -- the way to more eye pixels on this render
# path, since each eye is rendered at the window's size and the window must
# fit the desktop or the game does not boot (see above).
# Tested 2026-09-23 on a 4K desktop: 2560x1440 looks great and holds the
# headset's rate; 3840x2160 crashed at character select (no log line, no
# dump) and made the menus huge. Hence the cap.
# Set $gesResolution to e.g. "1920x1080" to pin it instead.
$gesResolution = "auto"
function Get-DesktopSize {
    try {
        if (-not ("GESVR.Dpi" -as [type])) {
            Add-Type -Namespace GESVR -Name Dpi -ErrorAction Stop -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
'@
        }
        [void][GESVR.Dpi]::SetProcessDPIAware()   # real pixels, not scaled ones
        $w = [GESVR.Dpi]::GetSystemMetrics(0)      # SM_CXSCREEN, primary monitor
        $h = [GESVR.Dpi]::GetSystemMetrics(1)
        if ($w -ge 1280 -and $h -ge 720) { return @($w, $h) }
    } catch { }
    return @(1920, 1080)
}
if ($gesResolution -match '^(\d+)x(\d+)$') {
    $gesWidth = [int]$Matches[1]; $gesHeight = [int]$Matches[2]
} else {
    $desk = Get-DesktopSize
    $gesHeight = [Math]::Min(1440, [Math]::Min([int]$desk[1], [int][Math]::Floor($desk[0] * 9 / 16)))
    $gesHeight -= $gesHeight % 9                   # so the width comes out exact
    $gesWidth = [int]($gesHeight * 16 / 9)
    Write-Host "Desktop $($desk[0])x$($desk[1]) -> game window ${gesWidth}x${gesHeight}"
}

# engine_no_focus_sleep: Source sleeps 20ms EVERY frame while its window is not
#   the active app. In VR the window frequently is not, so this caps the whole
#   engine and stalls title -> menu. 0 removes the sleep.
# snd_mute_losefocus: Source mutes audio on focus loss -- the "buggy sound".
# Resolution is deliberately untouched: 1280x1280 is what broke boot on 08-28.
$vrArgs = "-insecure -window -novid +mat_motion_blur_percent_of_screen_max 0 +crosshair 1 +mat_queue_mode 0 +mat_vsync 0 +mat_grain_scale_override 0 +engine_no_focus_sleep 0 +snd_mute_losefocus 0 -width $gesWidth -height $gesHeight"

if ($gesForceGraphics) {
    $vrArgs += " +mat_antialias $gesAA +mat_forceaniso $gesAniso +mat_picmip $gesPicmip"
}

# Must go through Steam so SDK 2007 mounts its VPKs (startup_loading.vtf lives there).
$steamExe = Join-Path $steam "steam.exe"
$argString = "-applaunch 218 -game $gameLink $vrArgs"

$bat = Join-Path $sdk "Launch-GESVR.bat"
@(
    "@echo off"
    "echo Starting GoldenEye: Source VR via Steam..."
    "start `"`" `"$steamExe`" $argString"
) | Set-Content -Path $bat -Encoding ASCII

# Leftover hl2.exe from a crash makes Steam say "already running" / won't launch.
$deadline = (Get-Date).AddSeconds(8)
$leftover = @(Get-Process -Name hl2 -ErrorAction SilentlyContinue)
if ($leftover) {
    Write-Host "Killing leftover hl2.exe so Steam can launch..."
    $leftover | Stop-Process -Force -ErrorAction SilentlyContinue
}
while (Get-Process -Name hl2 -ErrorAction SilentlyContinue) {
    if ((Get-Date) -gt $deadline) { break }
    Get-Process -Name hl2 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400
}
Start-Sleep -Seconds 2

Write-Host "Launching:"
Write-Host "  $steamExe $argString"
Write-Host "Headset should leave Theater once d3d9.dll submits stereo frames."
Write-Host "If it stays 2D: SteamVR Settings > Advanced > Dashboard >"
Write-Host "  Present Non-VR Applications on Theater Screen Upon Launch = Off"
Write-Host "Logs:"
Write-Host "  $sdk\bin\vrmod_log.txt"
Write-Host "  $env:TEMP\gesvr_boot.log"

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $steamExe
$psi.Arguments = $argString
$psi.UseShellExecute = $true
[System.Diagnostics.Process]::Start($psi) | Out-Null
