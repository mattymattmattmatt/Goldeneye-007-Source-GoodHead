# GoldenEye: Source VR

A VR mod for **[GoldenEye: Source](https://www.geshl2.com/)**, the fan-made Source-engine remake of GoldenEye 007's multiplayer. It is the real game, online and with bots, in a SteamVR headset: full 6DoF stereo, the gun in your hand, and a Q-branch watch on your wrist.

It works as a `d3d9.dll` that sits next to the game (DXVK plus the VR code), in the style of [L4D2VR](https://github.com/sd805/l4d2vr). Nothing in GoldenEye: Source itself is modified.

## Features

* **Two ways to aim** (VR Settings > Aiming > Aim mode):
  * **Free aim**: the gun is in your right hand and bullets go down the barrel. A dot marks exactly what you will hit, and the muzzle flash, tracers and ejected shells come out of the gun in your hand.
  * **Face aim**: shoot where you look, with the gun at your head as in the original. The reticle sits on the surface you are aiming at.
* **Melee by motion** (free aim): chop with the slappers or the hunting knife, and throw the knife with a real throwing motion -- it leaves your hand as the swing comes round, in the direction your hand is travelling.
* **Throw guide**: with a grenade, throwing knife or mine in hand, a thin dotted path runs from your hand to the spot where it will land. Thrown items fly where your hand points, so what you see is where it goes -- including mines, which you can place exactly where you aim. Cook a grenade too long (they are thrown when you let go of the trigger) and the path stops where it will burst in the air; the end dot turns red if that would catch you. VR Settings > Aiming > Throw guide.
* **Your off hand**: grenades and mines are carried in a left hand that follows your left controller, and the watch on your wrist lines up with the one the model is wearing. (Guns have no left arm in the game's models, so they show none.) Turn it off in VR Settings > Weapons > Off hand.
* **Scope zoom**: hold the left grip with a scoped weapon to zoom. The reticle always shows while zoomed.
* **Q-branch wrist watch** on your left wrist, GoldenEye 64 style: health and armour arcs, your weapon, ammo and round time. Kills ("YOU KILLED…", "KILLED BY…"), round start and round end pop up on it, and it flashes up when you change weapon.
* **VR Settings** in GE:S's main and pause menus: aim mode, five reticle styles (including GoldenEye's own crosshair, "Classic") in five colours and several sizes, snap or smooth turning, height, the watch, the game HUD, texture filtering, bloom, and more.
* **Sharp picture**: renders at your desktop's resolution up to 2560×1440, with 16× texture filtering.
* **Death done for VR**: the red blood curtain drops over your view and the camera stays steady.

## Requirements

* Windows 10 or 11 with a SteamVR headset. Developed on a Meta Quest 3 (Link / Air Link); bindings are included for Quest/Touch, Valve Index and Vive Cosmos controllers.
* **Source SDK Base 2007**: free on Steam. Search your library for "Source SDK Base 2007", or open `steam://install/218`.
* **GoldenEye: Source 5.0.x**, installed into `Steam\steamapps\sourcemods\gesource`.
* A reasonably strong GPU. Developed on an RTX 2080 Ti.

## Install and play

1. Install Source SDK Base 2007 and GoldenEye: Source, and run GE:S once without VR to make sure it starts.
2. Set your Windows desktop to **2560×1440 or higher** if your monitor can: each eye is rendered at the game window's size, and the window cannot be bigger than the desktop. At 1920×1080 it works, just softer.
3. Start SteamVR.
4. Run **`Launch-GESVR.bat`** from this folder.

The launcher finds Steam, Source SDK Base 2007 and GE:S, copies the mod next to `hl2.exe` (`d3d9.dll`, `openvr_api.dll`, `dxvk.conf` and the `VR` folder), adds **VR Settings** to GE:S's menu, and starts the game through Steam. Run it the same way every time; it never overwrites your settings.

## Controls

Defaults for Quest/Touch controllers. Rebind anything in SteamVR > Settings > Controllers > Manage Controller Bindings.

| Action | Button |
|---|---|
| Move | Left stick |
| Turn | Right stick left / right |
| Next / previous weapon | Right stick down / up |
| Fire | Right trigger |
| Aim mode (secondary) | Left trigger |
| Scope zoom, two-handed grip | Hold left grip |
| Reload | B (also Use), or bring the controllers together |
| Jump | A |
| Use | B |
| Crouch | Right grip |
| Scoreboard | Hold X |
| Pause menu | Y |
| Recenter | Left stick click |
| Menus | Point with the right controller, right trigger to click |
| VR Settings | "VR Settings" in the main or pause menu, or X while a menu is open |
| Watch | Look at the back of your left wrist |

On Index controllers the left A and B are the scoreboard and pause. Melee swings and knife throws work in free aim.

In SteamVR these appear under Manage Controller Bindings as "Default GoodHead bindings for..." for your controller.

## VR Settings

| Tab | What is there |
|---|---|
| Comfort | Smooth or snap turning, turn speed and angle, height offset, dominant hand, world scale |
| Aiming | Reticle on/off, style, size and colour; scope zoom; aim mode (face or free); throw guide |
| Weapons | Swing to attack; adjust weapon position (numpad, see below); off hand |
| Display | Wrist watch; when the watch shows; watch notices; watch on the model hand; menu distance and size; game HUD |
| Graphics | Texture filtering; bloom |

Menu size is how big the menus look and menu distance is how deep they sit: moving them does not resize them.

Everything is also in `…\Source SDK Base 2007\bin\VR\config.txt`, with a comment on every setting. The mod notices when that file is saved, even mid-game.

**Adjusting where a weapon sits in your hand** (free aim): turn on VR Settings > Weapons > Adjust position, hold the weapon, and use the numpad (Num Lock on). 8/2, 4/6 and 9/3 move it; 5 switches to rotating; +/- change the step; 0 saves to `VR\weapons.txt`; . resets it. 7 switches between the weapon and the off hand, which saves to `config.txt` instead.

## Good to know

* **Texture detail: keep it at High.** GE:S is a 32-bit game with a 2 GB ceiling, and its textures plus the copies the graphics layer keeps of them are what run it out of memory. Very High goes over, and changing texture detail while a map is loaded can crash it — set it from the main menu. (Do not use a "4 GB patch" on `hl2.exe` to raise it: see the next point.)
* **"SteamStartup() failed: SteamAPI_Init_Internal failed"** means `hl2.exe` has been modified, usually by a "4 GB" / large-address-aware patch, or was modified once. Launching it just once leaves Steam refusing the game at that install location, even after the file is put back, so the launcher refuses to start a modified `hl2.exe`. To recover: Steam > Library > Source SDK Base 2007 > Properties > Installed Files > **Verify integrity**, and if it still fails, **Move install folder** to any other Steam library (seconds, no download). Reinstalling into the same library does not fix it. GE:S itself and your VR settings are not affected.
* **The game's own HUD looks vertically stretched** (its radar is an oval). Each eye is rendered into a 16:9 window buffer and then un-squashed for the headset, which is correct for the world but stretches anything the game draws flat on the screen. The wrist watch and the VR menus are drawn separately and are not affected.
* **Resolution** follows your desktop, up to 2560×1440. A 4K window was tried: it crashed at character select and made the menus huge, so the launcher caps it. To pin a size, set `$gesResolution` near the top of the graphics section in `Launch-GESVR.ps1`.
* **The mod changes a few GE:S settings for VR**, and GE:S saves them in its own config: fast weapon switching, the standard death camera, and, while the watch is on, the round timer, ammo count, weapon list and kill feed are taken off GE:S's own HUD because the watch shows them. For flat play, switch them back in GE:S's options, or in its console: `hud_fastswitch 0; ge_fp_ragdoll 1; cl_ge_show_timer 1; cl_ge_show_ammocount 1; cl_ge_hud_noswitchlist 0; cl_ge_drawkillfeed 1`.
* **If the game appears on a flat screen in SteamVR** (Theater) instead of in VR: the launcher turns that off, but if it comes back, SteamVR > Settings > Dashboard > "Present non-VR applications on theater screen" = Off.
* **Quitting from the menu** can pause for a few seconds before the game closes: a Steam hang on exit that the mod detects and ends for you.
* **Multiplayer**: aiming goes through the game's normal input, so free aim and face aim work on any server, including ones without the mod, and flat players can play alongside you.
* **Logs**: `…\Source SDK Base 2007\bin\vrmod_log.txt`, started fresh once it passes 8 MB. If something goes wrong, that file says what.

## Building

Visual Studio 2022 (v143), Windows 10 SDK, **Release | Win32** (x86). Open `l4d2vr.sln` and build; the output is copied to `dist\d3d9.dll`. DXVK is included as a submodule under `dxvk\`. `HANDOFF.md` holds the engineering notes: every engine function the mod calls, how each one was verified, and what has been tried and why.

`tools\Make-Release.ps1` then builds the player download, `packages\GESVR-<version>.zip`: the launcher, this README, `dist\` and the licences of what `d3d9.dll` contains.

## Credits

* [L4D2VR](https://github.com/sd805/l4d2vr): the architecture, stereo path and usercmd packing this grew from
* [Portal2VR](https://github.com/Gistix/portal2vr): Source porting notes
* [DXVK (vr-dx9)](https://github.com/sd805/dxvk): D3D9 to Vulkan, and the hand-off to the SteamVR compositor
* [MinHook](https://github.com/TsudaKageyu/minhook), [OpenVR](https://github.com/ValveSoftware/openvr)
* Team GoldenEye: Source, for the game
