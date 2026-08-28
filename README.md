# GoldenEye: Source VR

A VR conversion for **[GoldenEye: Source](https://www.moddb.com/mods/goldeneye-source)** — the multiplayer Source SDK 2007 remake of GoldenEye 007.

This is an external `d3d9.dll` hook, the same approach as [L4D2VR](https://github.com/sd805/l4d2vr) and [Portal2VR](https://github.com/Gistix/portal2vr). GE:S does not have Valve-granted engine source, so the mod injects into the running `hl2.exe` from Source SDK Base 2007 rather than forking the engine.

## What works

* 6DoF stereo view through SteamVR (OpenVR + DXVK)
* Motion-controlled weapon aiming (shots come from the controller)
* Dual-wield: second viewmodel tracks the left controller
* Two-handed rifle/shotgun grip when your off-hand is on the barrel
* Manual reload by bringing the controllers together
* Snap or smooth turning, roomscale locomotion, recenter
* Watch / secondary fire, scoreboard, pause menu laser pointer
* Listen-server multiplayer: VR gun pose is packed into `CUserCmd`, so a VR host and VR clients share aim. Flat players can still join.

## Requirements

1. **SteamVR** running
2. **Source SDK Base 2007** (Steam app 218, free)
3. **GoldenEye: Source 5.0.x** extracted to `Steam\steamapps\sourcemods\gesource`
4. A VR headset SteamVR can see (Index, Quest via Link/VD, Vive, WMR, …)

## Play

```
# from this folder, after a Release|Win32 build:
Launch-GESVR.bat
```

The launcher copies `d3d9.dll`, `openvr_api.dll` and the `VR\` folder next to `hl2.exe`, then starts GE:S with:

```
-insecure -window -novid +mat_motion_blur_percent_of_screen_max 0 +crosshair 0
+mat_queue_mode 0 +mat_vsync 0 +mat_antialias 0 +mat_grain_scale_override 0
-width 1280 -height 720
```

Start SteamVR first. The launcher turns off SteamVR Theater (`Present Non-VR Applications on Theater Screen Upon Launch`). Source SDK Base 2007 is not a Steam VR app, so SteamVR will otherwise show the 2D window on a cinema screen until the mod submits stereo frames.

If you still land in Theater: SteamVR window → hamburger → Settings → Show Advanced Settings → Dashboard → **Present Non-VR Applications on Theater Screen Upon Launch = Off**, then relaunch with `Launch-GESVR.bat`. A log is written next to `hl2.exe` as `vrmod_log.txt`.

## Multiplayer

GE:S is multiplayer-only, so this is the actual game. CovertLAB-style BONELAB ports cannot do this.

1. Every VR player installs the same `d3d9.dll` + `VR\` next to `hl2.exe` (run `Launch-GESVR.bat` once on each PC).
2. One of you **creates a local/listen server** (Create Game → Local). Dedicated servers do not load `d3d9.dll`.
3. Flat players can join without the mod. Their aim stays face-forward; they just see VR players as normal models.
4. VR players stream gun pose in the usercmd. On the host, firing uses that pose so shots come from the controller, not the forehead.

`EnableNetVR=true` in `config.txt` (default on). Turn it off if a lobby desyncs.

You will see `VR client on slot N` in `vrmod_log.txt` on the host when a VR player’s cmds start arriving.

Edit `VR\config.txt` next to `hl2.exe` while the game is running — the mod hot-reloads it.

| Key | Default | Notes |
|-----|---------|--------|
| `VRScale` | `40.0` | Source units per metre. Raise if the world feels tiny. |
| `SnapTurning` | `false` | `true` for snap turn |
| `LeftHanded` | `false` | Swaps tracked controllers |
| `HideArms` | `true` | Hide first-person arms (N64 guns sit in your hand) |
| `HudAlwaysVisible` | `false` | Pin the full HUD in front of your face |
| `ShowWristHUD` | `true` | Ammo + health/armor watch on the off-hand. Raise that hand to your face like checking a watch |
| `HurtHUDSeconds` | `2.5` | How long the visor health bars stay up after you take a hit |
| `HurtHealthThreshold` | `80` | Keep the visor bars up while health is at or below this |

Wrist UV crops (`WristWatchUMin` … `WristAmmoVMax`) are 0–1 into the HUD texture. If ammo/health land in the wrong corner on your GE:S build, tweak those and hot-reload `config.txt`.

## Controls (Touch / Index)

| Action | Binding |
|--------|---------|
| Move | Left stick |
| Turn | Right stick |
| Fire | Right trigger |
| Alt fire / watch | Left trigger |
| Jump | Right A / A |
| Reload | Face button, **or clap controllers together** |
| Use | Right grip |
| Next / prev weapon | Right stick click / left stick click (see bindings) |
| Recenter height | Left stick click |
| Peek full HUD | ShowHUD bind / stick click (see bindings) |
| Watch HUD | Raise off-hand to your face |
| Pause | Menu / B |

Bindings live in `VR\SteamVRActionManifest\`. Rebind them in SteamVR if you want a left-handed layout.

## Build

Visual Studio 2022 (v143, Win32 / x86) with the Windows 10 SDK:

```
git clone --recurse-submodules <this-repo>
# dxvk is already vendored under dxvk/
```

Open `l4d2vr.sln`, set **Release | Win32**, Build. Output lands in `dist\`:

```
dist\d3d9.dll
dist\openvr_api.dll
dist\VR\config.txt
dist\VR\manifest.vrmanifest
dist\VR\SteamVRActionManifest\...
```

Then run `Launch-GESVR.bat`.

## How it works

```
hl2.exe  (Source SDK Base 2007)
  └─ d3d9.dll          DXVK + this mod (loaded because it sits next to the exe)
        ├─ OpenVR      HMD + controllers
        ├─ MinHook     CViewRender::RenderView, CalcViewModelView, CreateMove, …
        └─ client.dll / server.dll / materialsystem.dll  (GE:S + 2007 engine)
```

Stereo is rendered by calling `CViewRender::RenderView` twice into VR-sized render targets, then submitting the Vulkan images to the compositor. Weapon origin/angles are taken from the right controller; dual-wield viewmodels from the left. Multiplayer VR pose is packed into unused `CUserCmd` fields the same way L4D2VR does, so a listen-server host can reconstruct other VR players' gun poses.

Signatures for SDK 2007 are in `L4D2VR\offsets.h`. L4D2-only hooks (melee swing, `FireTerrorBullets`, splitscreen) are optional and silently skipped on GE:S.

## Known limits

* Signatures must resolve against *your* `client.dll` / `server.dll`. A future GE:S rebuild may need `offsets.h` updated (the scanner already tries several candidates).
* In-game VGUI menus are an overlay; some pause-menu widgets are fiddly.
* Watch gadgets share alt-fire rather than a dedicated wrist pose.
* Dedicated servers: only the host's VR pose is authoritative unless every client also runs the mod.

## Credits

* [L4D2VR](https://github.com/sd805/l4d2vr) — architecture, stereo path, usercmd packing
* [Portal2VR](https://github.com/Gistix/portal2vr) — Source-game porting notes
* [dxvk (vr-dx9)](https://github.com/sd805/dxvk) — D3D9 → Vulkan for the compositor
* [MinHook](https://github.com/TsudaKageyu/minhook), [OpenVR](https://github.com/ValveSoftware/openvr)
* Team GoldenEye: Source
