# GoodHead — GoldenEye: Source in VR

**[GoldenEye: Source](https://www.geshl2.com/)** — the fan-made remake of GoldenEye 007's multiplayer — played in a SteamVR headset.

Full 6DoF stereo, the gun in your hand, a working sniper scope, two-handed rifles, and a Q-branch watch on your wrist that shows your health, ammo and the scoreboard. Play online or with bots, on any server, alongside flat players.

## What's in v1.0

* **Sharp, full-resolution picture** — each eye is rendered at your headset's own resolution.
* **Tight head tracking** — no lag behind your head movements.
* **Guns in your hands** — bullets go where the barrel points, with muzzle flash, tracers and shells coming from the gun you're holding.
* **A real sniper scope** — grab the rifle with both hands and the zoomed view appears *inside the scope lens*, with its own crosshair.
* **Two-handed rifles** — hold the AR33 or KF7 with both hands and pull the left trigger to zoom. Aim is steadied while zoomed.
* **The wrist watch** — health, armour, ammo, round time, your weapon and kill notices. Click the right stick to flip it to the **scoreboard**, and click again to flip back.
* **Reload by bringing your hands together** (or press B). You can switch the gesture off.
* **Melee and throwing by motion** — chop with the slappers or knife, throw knives, cook and throw grenades, place mines exactly where you point.
* **A VR settings panel** inside the game, including GoldenEye: Source's own video settings.
* **The GoodHead title screen and theme** on the main menu.

---

## What you need

* **Windows 10 or 11** and a SteamVR headset. Made and played on a Meta Quest 3. Controls are included for Quest/Touch, Valve Index and Vive Cosmos.
* **[Source SDK Base 2007](steam://install/218)** — free on Steam.
* **GoldenEye: Source 5.0.x**, installed into `Steam\steamapps\sourcemods\gesource`.
* A reasonably strong graphics card (made on an RTX 2080 Ti).

## Install

1. Install Source SDK Base 2007 and GoldenEye: Source. **Run GoldenEye: Source once without VR** and make sure it reaches the menu.
2. Download the zip from **Releases** and unzip it anywhere — your desktop is fine.
3. Start SteamVR.
4. Run **`Launch-GESVR.bat`**.

That's it. Use `Launch-GESVR.bat` to start the game every time.

**Updating from an older version?** Just unzip the new one and run it. Your settings are kept.

---

## Controls

These are the defaults for Quest/Touch controllers.

| Action | Button |
|---|---|
| Move | Left stick |
| Turn | Right stick left / right |
| Next / previous weapon | Right stick down / up |
| Fire | Right trigger |
| Hold with both hands | Hold left grip near the gun |
| Zoom (AR33, KF7) | Both hands on the gun, then left trigger |
| Sniper scope | Both hands on the sniper rifle — the lens zooms by itself |
| Reload | **B**, or bring your hands together |
| Use — doors, buttons, pickups | **B** |
| Jump | **A** |
| Crouch | Right grip |
| Scoreboard on the watch | Click the right stick (click again to go back) |
| Full scoreboard | Hold **X** |
| Pause menu | **Y** |
| Recentre your view | Click the left stick |
| Look at the watch | Turn your left wrist towards you |
| Click in menus | Point with your right hand and pull the right trigger |

**Throwing and melee:**
* **Slappers or hunting knife** — chop with your right hand. The trigger works too.
* **Throwing knife** — throw it like a real knife.
* **Grenades** — squeeze the trigger to pull the pin, hold to cook, **let go to throw**.
* **Mines** — press the trigger and the mine goes where you're pointing.

**Controls not working?** If you've used an older version or changed your bindings, SteamVR may still be using your old ones. Go to **SteamVR > Settings > Controllers > Manage Controller Bindings**, pick **GoldenEye: Source**, and choose **Default GoodHead bindings**. Anything can be rebound there too.

---

## VR Settings

Press **Y** for the pause menu (or use the main menu) and click **VR Settings**. Point and pull the trigger to change things; every change applies and saves straight away. Click **Close** at the bottom when you're done.

* **Comfort** — smooth or snap turning, turn speed, a height offset if you play seated, left-handed mode, and world scale.
* **Aiming** — reticle style, colour and size, scope zoom, **aim mode** (free aim with the gun in your hand, or face aim where you shoot where you look), and a **throw guide** that shows where grenades, knives and mines will land.
* **Weapons** — swing-to-attack melee, your off hand shown in the game, the reload gesture on or off, and fine-tuning of where each gun sits in your hand.
* **Display** — the wrist watch, kill notices on the watch, and how far away and how big the menus are.
* **Graphics and Detail** — texture detail, anti-aliasing, texture filtering, bloom and the other detail settings. GoldenEye: Source's own video options can't be used from inside a headset, so they're here instead.

**Tip:** set **Texture detail to Medium** before your first match. It makes map loading much more reliable and still looks great in a headset.

---

## Troubleshooting

**The game closes or crashes while loading a map** — just try again. It usually loads on the second or third go, because each attempt makes the next one easier. If it keeps happening, set **Texture detail** to **Medium** (or **Low**) in VR Settings > Graphics. GoldenEye: Source is an old 32-bit game with limited memory, and big maps can fill it up.

**Never use a "4 GB patch" on `hl2.exe`.** It breaks the game, and Steam will refuse to start it again even after you undo it. If you've already done this: in Steam, go to Source SDK Base 2007 > Properties > Installed Files, click **Verify integrity**, and if that doesn't help, use **Move install folder** to move it to another Steam library.

**The game shows on a flat screen in SteamVR instead of in VR** — go to SteamVR > Settings > Dashboard and turn **"Present non-VR applications on theater screen"** off.

**There's no radar** — the game's flat HUD doesn't work in a headset, so the watch shows what you need: health, armour, ammo, time, kills and the scoreboard.

**Recording or streaming** — the game window on your desktop doesn't show exactly what you see. Use SteamVR's mirror (SteamVR menu > Display VR View) or your headset's own recording instead.

**Something else went wrong** — the file `Source SDK Base 2007\bin\vrmod_log.txt` records what happened. Attach it if you report a problem.

---

## Good to know

* **Multiplayer works normally** on any server, with or without other VR players.
* **The game's own files are safe.** The launcher changes a few of GoldenEye: Source's menu files for the GoodHead title screen and theme, and keeps the originals next to them (as `.gesvr-orig`) so they can go back.
* **The mod changes a few game settings** that work better in VR, such as fast weapon switching. To put them back for flat play, paste this into the game's console:
  ```
  hud_fastswitch 0; ge_fp_ragdoll 1; cl_ge_show_timer 1; cl_ge_show_ammocount 1; cl_ge_hud_noswitchlist 0; cl_ge_drawkillfeed 1
  ```

---

## Building it yourself

Visual Studio 2022, **Release | Win32**. Clone with submodules:

```
git clone --recursive https://github.com/mattymattmattmatt/Goldeneye-007-Source-GoodHead.git
```

Open `l4d2vr.sln` and build. The mod lands in `dist\d3d9.dll`, and `tools\Make-Release.ps1` builds the download zip. **`HANDOFF.md`** has the full technical notes.

## Credits

* [L4D2VR](https://github.com/sd805/l4d2vr) — the VR foundation this grew from
* [DXVK](https://github.com/doitsujin/dxvk) and [sd805's VR fork](https://github.com/sd805/dxvk)
* [Portal2VR](https://github.com/Gistix/portal2vr), [MinHook](https://github.com/TsudaKageyu/minhook), [OpenVR](https://github.com/ValveSoftware/openvr)
* **Team GoldenEye: Source** — for the game itself. Go and play it flat too.

GoldenEye 007 is a trademark of its owners. This is an unofficial, non-commercial fan project with no affiliation to them, to Team GoldenEye: Source, or to Valve.
