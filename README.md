# GoodHead — GoldenEye: Source in VR

**[GoldenEye: Source](https://www.geshl2.com/)** — the fan-made Source remake of GoldenEye 007's multiplayer — played in a SteamVR headset. Full 6DoF stereo, the gun in your hand, bullets down the barrel, and a Q-branch watch on your wrist. Online and with bots, on any server, alongside flat players.

It installs as a `d3d9.dll` next to the game (DXVK plus the VR code, in the style of [L4D2VR](https://github.com/sd805/l4d2vr)). **Nothing in GoldenEye: Source is modified**, and the launcher never overwrites your settings.

---

## What you need

* **Windows 10 or 11** and a SteamVR headset. Built and played on a Meta Quest 3 over Link; bindings ship for Quest/Touch, Valve Index and Vive Cosmos.
* **[Source SDK Base 2007](steam://install/218)** — free on Steam. Search your library for it, or open `steam://install/218`.
* **GoldenEye: Source 5.0.x**, installed into `Steam\steamapps\sourcemods\gesource`.
* A reasonably strong GPU. Developed on an RTX 2080 Ti.

## Install

1. Install Source SDK Base 2007 and GoldenEye: Source. **Run GE:S once without VR** and make sure it reaches the menu.
2. Unzip this download anywhere — your desktop is fine. Keep the files together.
3. Start SteamVR.
4. Run **`Launch-GESVR.bat`**.

That's it. The launcher finds Steam, the SDK and GE:S, copies the mod next to `hl2.exe`, adds **VR Settings** to the game's menus, and starts the game through Steam. Run it the same way every time.

**Before your first match**, in GE:S's own options set **texture detail to Medium** (Options > Video > Advanced). GE:S is a 32-bit game and High can run it out of memory while a map loads — see [If it crashes while loading a map](#if-it-crashes-while-loading-a-map), which explains exactly why.

If your monitor allows it, set the Windows desktop to **2560×1440 or higher** before playing. Each eye is rendered at the game window's size and the window can't exceed the desktop, so a bigger desktop means a sharper headset image. 1920×1080 works, just softer.

---

## Controls

Defaults for Quest/Touch. Rebind anything in SteamVR > Settings > Controllers > Manage Controller Bindings, where these appear as **"Default GoodHead bindings"**.

| Action | Button |
|---|---|
| Move | Left stick |
| Turn | Right stick left / right |
| Next / previous weapon | Right stick down / up |
| Fire | Right trigger |
| Secondary attack (alt fire) | Left trigger |
| Scope zoom / two-handed grip | Hold left grip |
| Reload | **B** (shared with Use), or bring the controllers together |
| Use — doors, buttons, pickups | **B** |
| Jump | A |
| Crouch | Right grip |
| Scoreboard | Hold X |
| Pause menu | Y |
| Recentre your view | Left stick click |
| Open VR Settings | X while any menu is open |
| Click in menus | Point with your right hand, right trigger |
| Look at the watch | Turn your left wrist towards you |

On Index controllers the left A and B are scoreboard and pause.

**Melee and throwing** (free aim only):
* **Slappers or hunting knife** — chop with your right hand. The trigger still works.
* **Throwing knife** — throw it. It leaves your hand as the swing comes round, in the direction your hand is travelling.
* **Grenades** — squeeze the trigger to pull the pin, hold to cook, **let go to throw**. That's how GE:S grenades work, and you can move normally while cooking.
* **Mines** — press the trigger; the mine leaves a moment later, on the line you were pointing along.

---

## The VR menu

Open it with **X while any menu is open**, or pick **VR Settings** in the main or pause menu. Point with your right hand and click with the trigger. Close it with **B**, **Y**, or **X** again. Everything applies and saves the moment you change it.

### Comfort
Smooth or snap turning, turn speed and snap angle, a **height offset** if you sit or if you feel too short, dominant hand for left-handers, and **world scale** — lower makes you feel taller and the world smaller; 40 is life size.

### Aiming
The **reticle** on or off, with five styles (dot, cross, ring, ring + dot, and **Classic** — GoldenEye's own crosshair), five colours and several sizes. **Scope zoom** on or off.

**Aim mode** is the big one:
* **Free aim** — the gun is in your hand and bullets go where the barrel points. A dot marks exactly what you'll hit. Muzzle flash, tracers and ejected shells come from the gun in your hand.
* **Face aim** — you shoot where you look, gun held up by your face as in the original game.

**Throw guide** draws a thin dotted path from your hand to where a grenade, knife or mine will land, with a dot at the landing point. Thrown things fly where your hand points, so what you see is where it goes — you can place mines exactly. Cook a grenade too long and the path stops where it will burst in mid-air; the end dot turns red if that blast would catch you.

### Weapons
**Swing to attack** for the melee gestures above. **Off hand** puts the left arm that grenades and mines carry onto your left controller, with the watch sitting on the Seamaster modelled on its wrist (guns have no left arm in the game's models, so they show none). **Adjust position** turns on numpad tuning — see below.

### Display
The **wrist watch**, and whether it shows always or only when you look at it. **Watch notices** put kills, round start and round end on the watch face instead of the HUD. **Watch on model** sits the watch where the grenade hand's own watch is. **Menu distance** and **menu size** are independent: distance moves the game's menus deeper without resizing them. **Game HUD** — off, only when hurt, or always.

### Graphics
**Texture filtering** (16× by default) and **bloom**.

Everything here is also in `…\Source SDK Base 2007\bin\VR\config.txt`, one setting per line with a comment explaining each. The mod notices when that file is saved, **even mid-game**, so you can tune it on a second monitor while wearing the headset.

### Putting a weapon in your hand exactly right

Free aim only. Turn on **VR Settings > Weapons > Adjust position**, hold the weapon, and use the numpad with Num Lock on:

| Key | Does |
|---|---|
| 8 / 2 | Move forward / back |
| 4 / 6 | Move left / right |
| 9 / 3 | Move up / down |
| 5 | Switch between moving and rotating |
| 7 | Switch between the weapon and your off hand |
| + / − | Bigger or smaller steps |
| 0 | Save |
| . | Reset this weapon |

Weapons save to `VR\weapons.txt`, the off hand to `config.txt`. Every weapon already ships tuned, so this is only if you want it different.

---

## If it crashes while loading a map

**Try again first.** It very often loads on the second or third attempt, and there's a real reason for that, not just luck.

**Why it happens.** GoldenEye: Source is a 32-bit program, so everything it uses must fit in **2 GB of address space** — the game, the map, every texture, and the copies the graphics layer keeps while handing them to your GPU. Loading a map is the worst moment: it's reading in textures *and* compiling shaders at the same time. Measured on a real crash here, at High texture detail:

```
1295 MB used, largest free block 430 MB
1554 MB used, largest free block 165 MB
1915 MB used, largest free block  18 MB
1945 MB used, largest free block  10 MB   <- crash
```

Notice the second number. It's not just that memory runs out — it gets **broken into pieces**. A texture needs one unbroken block, and by the end the biggest gap left was 10 MB. That's why the failure looks random: it depends on how the pieces happen to fall.

**Why trying again works.** Shaders your graphics driver compiles are saved to disk as it goes. A crashed attempt still keeps whatever it finished, so the next attempt has less to do and needs less memory. Each try genuinely gets further.

**The fix, in order:**

1. **Set texture detail to Medium** in GE:S's own options (Options > Video > Advanced), from the main menu — not while a map is loaded. This is the single biggest win and costs little in a headset.
2. If it still struggles, **Low**, or drop your desktop resolution before launching — the render buffers scale with it.
3. Give it two or three attempts after either change, so the shader cache fills up.

**Never use a "4 GB patch" on `hl2.exe`** to raise the limit. It doesn't work and it breaks the game permanently: one launch with a patched exe makes Steam refuse to start that copy for good, *even after you put the original file back*. The launcher refuses to start a modified `hl2.exe` for this reason. If you've already done it, see the next section.

---

## Other problems

**"SteamStartup() failed: SteamAPI_Init_Internal failed"** — `hl2.exe` has been modified at some point (usually a 4 GB patch). Verifying the files isn't enough on its own. Fix it like this:

1. Steam > Library > Source SDK Base 2007 > Properties > Installed Files > **Verify integrity**.
2. If it still fails, same page > **Move install folder**, to any other Steam library. Seconds, no re-download.

Reinstalling into the *same* folder does not fix it. Your GE:S install and your VR settings are untouched by any of this.

**The game shows on a flat screen in SteamVR (Theater) instead of in VR** — the launcher turns Theater off, but if it comes back: SteamVR > Settings > Dashboard > "Present non-VR applications on theater screen" = Off.

**The game's own HUD looks vertically stretched** — its radar is an oval. Each eye is rendered into a widescreen buffer and then un-squashed for the headset, which is right for the world but stretches anything drawn flat on the screen. The wrist watch and the VR menus are drawn separately and look correct. Turning the HUD off (Display > Game HUD) and using the watch avoids it.

**Quitting from the menu pauses for a few seconds** — a Steam hang on exit that the mod detects and ends for you.

**Something else went wrong** — `…\Source SDK Base 2007\bin\vrmod_log.txt` records what happened, including memory use and frame pacing. It starts fresh once it passes 8 MB.

---

## Good to know

* **Multiplayer works normally.** Aiming goes through the game's own input, so free aim and face aim work on any server, including ones that have never heard of this mod, and flat players can play alongside you.
* **The mod changes a few GE:S settings** and the game saves them in its own config: fast weapon switching, the standard death camera, and — while the watch is on — the round timer, ammo count, weapon list and kill feed come off the game's HUD, because the watch shows them. To go back for flat play, change them in GE:S's options, or paste into its console:
  ```
  hud_fastswitch 0; ge_fp_ragdoll 1; cl_ge_show_timer 1; cl_ge_show_ammocount 1; cl_ge_hud_noswitchlist 0; cl_ge_drawkillfeed 1
  ```
* **Resolution** follows your desktop, capped at 2560×1440. A 4K window was tried and crashed at character select, hence the cap. To pin a size, set `$gesResolution` near the graphics section of `Launch-GESVR.ps1`.
* **Death is done for VR**: the red blood curtain drops over your view and the camera stays steady instead of riding the ragdoll.

---

## Building it yourself

Visual Studio 2022 (v143), Windows 10 SDK, **Release | Win32**. Clone with submodules:

```
git clone --recursive https://github.com/mattymattmattmatt/Goldeneye-007-Source-GoodHead.git
```

Open `l4d2vr.sln` and build; the output lands in `dist\d3d9.dll`. DXVK is the `dxvk\` submodule — the VR reticle, the throw guide and the compositor fixes live there, so a clone without submodules will not link.

`tools\Make-Release.ps1` builds the player download into `packages\`.

**`HANDOFF.md`** is the engineering log: every engine function the mod calls and how each was verified, plus every wrong turn and what it cost. If you're porting a Source game to VR, that file is the useful part of this repository.

## Credits

* [L4D2VR](https://github.com/sd805/l4d2vr) — the architecture, stereo path and usercmd packing this grew from
* [DXVK](https://github.com/doitsujin/dxvk) and [sd805's vr-dx9 fork](https://github.com/sd805/dxvk) — D3D9 to Vulkan, and the hand-off to the SteamVR compositor
* [Portal2VR](https://github.com/Gistix/portal2vr) — Source porting notes
* [MinHook](https://github.com/TsudaKageyu/minhook), [OpenVR](https://github.com/ValveSoftware/openvr)
* **Team GoldenEye: Source** — for the game itself. Go and play it flat too.

GoldenEye 007 is a trademark of its owners. This is an unofficial, non-commercial fan project with no affiliation to them, to Team GoldenEye: Source, or to Valve.
