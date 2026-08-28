# GESVR — GoldenEye: Source VR — Handoff

Last updated: **2026-08-28**, after the "crashes just after the menu pops up" session.
Owner: Matty. Headset: SteamVR. Target quality: HL2VR / HaloCEVR, not "2D in Theater".

---

## READ THIS FIRST (morning of 2026-08-28)

The build installed at **01:58** has never been playtested. It contains six fixes,
four of which change behaviour you will notice immediately. If something is wrong,
**the fastest lever is `config.txt`, which hot-reloads while the game is running** —
you do not need to rebuild or even restart.

Config lives in three places, all kept in sync by the build/install step:

```
G:\Like Grok Work\GESVR\dist\VR\config.txt
G:\SteamLibrary\steamapps\common\Source SDK Base 2007\VR\config.txt
G:\SteamLibrary\steamapps\common\Source SDK Base 2007\bin\VR\config.txt
```

The one the game actually reads is resolved from `Game::ModDir()`, which the boot log
reports as `...\Source SDK Base 2007\bin`. **Edit the `bin\VR\config.txt` copy.**

### The one setting that matters most

```
DisplayMode=compositor
```

This session switched the display path from the old head-locked side-by-side quad to
real per-eye compositor submits. If the headset shows **nothing / black** in-game,
that path is not reaching your HMD — change it to `DisplayMode=sbs`, save, and the old
(working but "giant 3D TV") behaviour comes straight back without a restart.

---

## INPUT + LOCOMOTION REVIEW (20:16)

### Console command flood (fixed)

`ProcessInput` had **32** `ClientCmd_Unrestricted` call sites, several of them
unconditional if/else pairs that fire every single frame regardless of input:
`+attack2`/`-attack2`, `+duck`/`-duck`, `-showscores`. At 200+ fps that is well
over a thousand console commands per second pushed into Source's **fixed-size**
command buffer. Best case it is pure waste every frame; worst case the buffer
overflows and commands are dropped, which would read as unresponsive or stuck
input in game.

`VR::MoveCmd()` now sends a given `+cmd`/`-cmd` only when its state actually
changes. All 32 sites converted. Non-toggle commands (`invnext`, `impulse 100`)
pass straight through -- they were already edge-triggered via
`PressedDigitalAction(..., true)`.

### Controller bindings (diagnostic added)

The action manifest ships default bindings for exactly three controller types:

```
oculus_touch, knuckles, vive_cosmos_controller
```

Nothing for `vive_controller` (Vive wands), `holographic_controller` (WMR) or
`hpmotioncontroller` (Reverb G2). If the headset in use is one of those, **every
digital action silently reads false forever** -- no error, no warning, nothing in
the log. That is entirely consistent with `sel=0 atk=0` on every `MenuHealth`
sample so far, though those samples are also consistent with simply not pressing
at that instant.

Startup now logs it explicitly:

```
Controller 1 type='oculus_touch' model='Oculus Quest' bindingShipped=1
Controller 2 type='vive_controller' model='...' bindingShipped=0  <-- NO DEFAULT BINDING, actions will never fire
```

If `bindingShipped=0` appears, the fix is to add a `bindings_<type>.json` and a
`default_bindings` entry -- copying `bindings_oculus_touch.json` and changing the
controller type is usually enough to get started.

### Locomotion design notes (not changed)

- Movement is **digital, not analog**: `analogActionData.y > 0.5` means full
  speed. The thumbstick behaves as a D-pad. The code carries a TODO to move this
  into `CreateMove` instead, which is the correct fix and would also give analog
  speed -- but the `CreateMove` hook is deliberately left unhooked (wrong return
  convention, see landmines).
- Movement is relative to view angles, and `ApplyHeadAndIpd` snaps view angles to
  the gun while firing. With purely digital `+forward`, **firing while moving will
  visibly veer your direction of travel.** Worth watching for once it is playable;
  the fix is to keep locomotion on HMD yaw and let only the shot use gun angle.

---

## WEAPON + HUD REVIEW (19:54)

Static review while the headset was unavailable. Headline: **large parts of the
mod were never connected to anything.**

### Was dead, now wired

| System | Why it was dead |
|---|---|
| Per-weapon viewmodel poses (`Weapons::GetOffset`) | only caller was `UpdateTracking()`, which has **no call sites** |
| Melee / dual-wield / throwable predicates | same |
| Two-handed rifle grip | same |
| `UpdateWristHUD()` (watch: health/armor) | **no call sites** |
| `UpdateHurtHUD()` (damage flash) | **no call sites** |
| `ReadLocalHealth()` / `ResolvePlayerNetvars()` | only reachable from the two above |

The whole `weapons.cpp` table -- pp7, dd44, klobb, ar33, rcp90, sniper, shotguns,
knife, grenades, ~30 entries -- has never once been consulted. Every gun used the
generic fallback pose. `m_ActiveWeaponModel` **is** populated (by
`dDrawModelExecute`), so the data was being collected and thrown away.

Roughly 20 config keys (`ShowWristHUD`, `WristOffset`, `WristRotation`,
`WristWatch*`, `WristAmmo*`, `HurtHUD*`, `HurtHealthThreshold`) did nothing.

Fixes:
- Weapon pose + two-handed grip moved into `ApplyHeadAndIpd`, where the viewmodel
  basis is actually built. New flags `PerWeaponOffsets`, `TwoHandedGrip`.
- `ViewmodelOffset` is now **additive** on top of the per-weapon value rather than
  overwriting it.
- `UpdateWristHUD()` / `UpdateHurtHUD()` called from the in-game path, gated on
  `IsInMap()`.
- **`m_VKHUD` is now refreshed in game.** The wrist overlays crop their watch and
  ammo faces out of it, but it was only ever filled by the MENU branch -- in game
  it held a stale menu frame forever, so the watch could never have worked even
  if it had been called.

All of this runs only in-map, so it cannot affect menu-freeze testing.

### Cannot be fixed without new signatures

These offsets are **L4D2 signatures that do not match GE:S** and silently fail to
resolve (confirmed in the boot log as "Optional signature not found"):

| Offset | Consequence |
|---|---|
| `Weapon_ShootPosition` (client + server) | shot ORIGIN is not moved to the gun |
| `ClientFireTerrorBullets` / `ServerFireTerrorBullets` | same -- and "TerrorBullets" is an L4D2-only function name |
| `ProcessUsercmds` / `ReadUsercmd` / `WriteUsercmd*` | **VRNet is entirely inert** -- no VR poses shared with other players |
| melee swing hooks, `EyePosition`, `GetRenderTarget` | melee collision not moved to the controller |

**Net effect on motion guns:** the weapon *model* follows your hand correctly
(`dDrawModelExecute` + `CalcViewModelView` both resolve), and shot *direction*
follows the gun because `ApplyHeadAndIpd` calls `SetViewAngles(gunAngle)` while
firing. But the shot *origin* is still the player's eye. Expect bullets to travel
where the gun points while starting at your head -- fine at range, visibly off for
close or hip-fired shots.

Fixing that needs GE:S-specific signatures for `CBasePlayer::Weapon_ShootPosition`
and GE:S's own FireBullets equivalent. That is a disassembly job against GE:S's
`client.dll` / `server.dll`, not something that can be guessed.

### Also worth knowing

`SetViewAngles` is called every frame with the HMD angle, and snaps to the gun
angle while firing. Movement is relative to view angles, so the fire snap may
tug the locomotion direction. Worth watching for once it is playable.

---

## ROOT CAUSE FOUND (18:48): unsynchronised VkQueue access

The stack sampler caught it, and resolving the offsets against a `/MAP` build
turned it into names:

```
EIP  ntdll (wait)
[01] D3D9DeviceEx::Flush              +0x15D
[02] DxvkSubmissionQueue::synchronizeSubmission
[03] DxvkDevice::waitForSubmission     <- blocked here, forever
[04] D3D9SwapChainEx::PresentImage
[11] D3D9SwapChainEx::Present
[13] D3D9DeviceEx::Present
```

Identical across two samples four seconds apart: a hard block, not a spin.

**The main thread was stuck inside Present waiting on a Vulkan submission fence
that never signalled, because OpenVR was submitting to the same VkQueue at the
same time from another thread.**

DXVK submits from its own submission thread. `vr::VRCompositor()->Submit()` does
a `vkQueueSubmit` on `device->queues().graphics.queueHandle` -- the same queue.
Vulkan requires queue access to be externally synchronised; concurrent submits
are undefined behaviour, and in practice a fence is lost and
`waitForSubmission` never returns.

DxvkDevice says so in as many words:

> "Since Vulkan queues are only meant to be accessed from one thread at a time,
> **external libraries need to lock the queue before submitting command buffers.**"

`lockSubmission()` / `unlockSubmission()` exist for exactly this, and **this
project has never called them**. That predates this entire session and explains
the whole history: intermittent freezes, "terrible performance", crashes that
moved around whenever timing changed, and why it got dramatically worse once
Submit moved to a dedicated thread submitting continuously at 90Hz (more
concurrent submits = near-certain collision) -- and why clicking triggers it,
since a click causes extra GPU work and raises the collision odds.

### The fix

`IDirect3DVR9` gains `LockSubmission()` / `UnlockSubmission()`, forwarding to
`DxvkDevice::lockSubmission/unlockSubmission`. Both submit paths in
`VR::SubmitThreadBody` are bracketed:

```cpp
if (g_D3DVR9) g_D3DVR9->LockSubmission();
el = comp->Submit(vr::Eye_Left,  ...);
er = comp->Submit(vr::Eye_Right, ...);
if (g_D3DVR9) g_D3DVR9->UnlockSubmission();
```

**Any future code that hands a texture to OpenVR must be bracketed the same
way.** This is the single most important invariant in the project.

### Technique worth reusing

When a hang resists theory, resolve it rather than guessing:

1. Watchdog thread samples the stalled thread (`SuspendThread` +
   `GetThreadContext`), scans its stack for executable addresses, resolves
   modules with `VirtualQuery` + `GetModuleFileName`.
2. Rebuild **with sources unchanged** and `<GenerateMapFile>true</GenerateMapFile>`
   -- adding /MAP does not alter codegen, so RVAs still match the tested binary.
3. Parse the MAP, binary-search the symbol below each RVA.

That turned five hours of wrong theories into an exact answer in one run.

### Known DXVK bug found in passing (not yet hit)

`HookWindowProc` takes `g_windowProcMapMutex` and then calls `ResetWindowProc`,
which takes the same non-recursive `std::mutex` -- an unconditional self-deadlock
if it ever runs. It only fires on a fullscreen transition, and GE:S runs
`-window`, so it has not been hit. Worth fixing before anyone tries fullscreen.

---

## THE FREEZE IS THE CLICK (13:52) -- and a process reset

### The bisect settled it

Cursor publishing was armed at frame 90 and OpenVR action polling at frame 300,
each bracketed in the log. Both survived:

```
f=90  -> publishing aim -> aim published
f=300 -> polling OpenVR actions -> actions polled
... ran to frame 451 ...
Overlay MouseButtonDown at (196,378)
MenuInput click posted at (196,378) fg=1
WATCHDOG no Present for 3438 ms
```

**Posting a mouse click to the game window freezes the main thread.** Not the
warmup, not the cursor, not the action polling. `stereoPasses=0` also rules out
the stereo path. This matches the very first symptom of the session ("crashes
just after the menu pops up") and Grok's note that a click posted from
AfterPresent froze the main thread -- the click has been the bug the whole time,
and every fix so far has been fixing something else.

### Stop guessing -- sample the stalled thread

Three theories about this freeze have now been wrong (USER32-on-Present,
compositor-on-Present, input arming). Both of the first two were real bugs and
are correctly fixed, but neither was this one. Rather than a fourth guess, the
watchdog now suspends the Present thread on a stall and reports where it
actually is:

```
STACK: EIP  vguimatsurface.dll+0x1A2C4
STACK:  [00] vgui2.dll+0x8F31
STACK:  [01] engine.dll+0x12ABC
```

No new link dependencies -- addresses are resolved with `VirtualQuery` +
`GetModuleFileName`, and the stack is scanned for executable return addresses
because these are old MSVC builds with frame-pointer omission. Sampled twice,
seconds apart: identical EIP = hard block, moving EIP = spin.

Module names alone are decisive:

| Module in EIP | Meaning |
|---|---|
| `openvr_api` | compositor / overlay call |
| `user32` / `win32u` | window or cursor |
| `vgui2` / `vguimatsurface` | VGUI processing the click |
| `engine` / `client` / `GameUI` | the game's own click handling |
| `d3d9` | our code (this DLL) |
| `ntdll` waiting | a lock -- look at the return addresses for who took it |

### Version control (this was the real process failure)

The project had **no source control**, which is why the 2026-08-27 ~23:55 build
that reached in-game is unrecoverable, and why five hours of changes could not be
bisected. Fixed:

- `git init` + full snapshot in `G:\Like Grok Work\GESVR` (`.gitignore` excludes
  Release/, *.obj, *.pdb).
- `dxvk/` has its own repo; its VR changes are committed there too.

**From now on: commit before each experiment.** Every change in this session was
otherwise destructive and unrecoverable.

### Honest assessment of the session

Real bugs found and fixed, all verified by measurement:
uninitialised texture holders; the unused per-eye frustum crop; `WaitGetPoses`
twice per frame; four blocking GPU syncs per frame; ~900 IPC calls/sec of Theater
suppression; a varargs bug reading garbage off the stack; six file opens per
frame of logging on the render thread; USER32 on the Present callstack;
compositor Submit on the Present callstack; the compositor starved during map
load; `UpdateTracking()` and `SubmitVRTextures()` dead with no callers.

But: too many unverified changes were stacked between playtests, on a codebase
that cannot be tested without a headset. That is the process error to avoid
repeating -- one behavioural change per run, and commit first.

---

## THE ARCHITECTURAL RULE (13:33) -- read this before changing anything

Two independent deadlocks have now been proven by the watchdog, and they are the
same mistake in two different libraries:

> **Nothing on the D3D Present callstack may call USER32 or the OpenVR
> compositor.**

`VR::Update` and `AfterPresent` both run inside `D3D9DeviceEx::PresentEx`.

### Deadlock 1 -- USER32 (found 13:12)

`DriveGameCursor` called `SetCursorPos` / `ClientToScreen` / `PostMessageA` from
Present. The hang began on the exact frame menu input went live. `SetCursorPos`
transacts with the desktop input thread; from a thread inside Present that is not
pumping messages, it deadlocks.

### Deadlock 2 -- OpenVR compositor (found 13:26)

The smoking gun:

```
MENU f=4 ... TOTAL=0.4ms
AfterPresent tick #4 1.1ms      <- keepalive idle
MENU f=5 ... TOTAL=0.3ms
[nothing]                        <- keepalive fires -> hang, 22s+
```

Every frame logs `MENU f=N` then `AfterPresent tick #N`. Frame 5 logged the first
and never the second, and frame 5 is when the 33ms keepalive fired again. Frame
1's keepalive had already taken **152ms** (`SubmitBlackEyes L=108.5 R=44.0`).

DXVK's present and OpenVR's Vulkan Submit drive the same graphics queue. Doing
both from one thread deadlocks. Grok's original 500ms cadence did not fix this --
it only made the dice roll rarely. Raising it to 33ms made it near-certain.

### The architecture now

Three threads, strict ownership:

| Thread | Owns | Must never |
|---|---|---|
| Render (inside PresentEx) | all D3D: capture, blit, overlay texture upload | USER32, compositor Submit/WaitGetPoses |
| `MenuInput` | all USER32: SetCursorPos, WM_MOUSEMOVE, WM_LBUTTON* | D3D, compositor |
| `VRSubmit` | all compositor: WaitGetPoses, Submit | D3D (it must never touch g_D3DVR9) |

The render thread *publishes* (atomics + a pre-prepared texture); the worker
threads *act*. `VR::PrepareBlackTexture()` exists precisely so the black keepalive
surface is built with D3D on the render thread and only submitted elsewhere.

Blocking in `WaitGetPoses` on the VRSubmit thread is correct and costs the game
nothing -- that is where a compositor wait belongs.

### Functions deliberately neutered (do not revive)

- `GESVR_SubmitBackBufferFallback()` -- now empty.
- `SubmitBlackEyes()` / `SubmitStereoToCompositor()` -- publish flags only.
- `SubmitVRTextures()` -- dead for the life of the project; left as a named stub
  with a warning because it is exactly what someone re-wires by accident.
- `UpdatePosesAndActions()` -- no longer calls WaitGetPoses; poses arrive from
  the VRSubmit thread under `VRSubmit::g_poseLock` (held only for the memcpy).
- `FadeGrid` / `CompositorBringToFront` -- removed from the per-frame path.

### New log lines

```
Black keepalive texture prepared on the render thread
VRSubmit #1 waitErr=0 Lerr=0 Rerr=0 eyes=0
```

If `VRSubmit #N` climbs steadily and the watchdog stays quiet, the deadlock class
is gone.

---

## CODE REVIEW + the main-menu hang (13:20)

The watchdog added at 12:39 paid for itself on its first run:

```
[13:12:41] Menu input armed after warmup (90 frames)
[13:12:41] AfterPresent tick #91 0.0ms inmap=0 waitErr=0 eyes=0 click=0
[13:12:45] WATCHDOG no Present for 3422 ms (inMap=0 stereoPasses=0)
[13:13:04] WATCHDOG no Present for 22453 ms (inMap=0 stereoPasses=0)
```

Three things that were previously guesswork are now facts:
1. It is a **hang, not a crash** -- the process lives, the main thread never Presents again.
2. It has **nothing to do with the map or stereo** (`inMap=0`, `stereoPasses=0`).
3. It begins on **the exact frame menu input goes live**.

### Root cause: USER32 on the Present callstack

`VR::Update` runs inside `D3D9DeviceEx::PresentEx`. Once `inputLive` flipped,
`ProcessMenuInput` -> `DriveGameCursor` began calling `ClientToScreen`,
`SetCursorPos` and `PostMessageA` from that callstack. `SetCursorPos` transacts
with the desktop input thread; issuing it from a thread that is inside Present
and not pumping its own message queue deadlocks. The in-code comment asserting
"SetCursorPos and PostMessage are safe from Present" was wrong and is now
corrected in place.

**Rule going forward: nothing on the Present callstack may call USER32.**

### The fix: a MenuInput worker thread

All USER32 for menu input now lives in one place (`namespace MenuInput` in
vr.cpp) and nowhere else:

- The render thread only stores plain atomics (aim point, click counter, key
  counter). It never blocks, never locks, never enters USER32.
- The worker applies them at ~120Hz: `SetCursorPos`, `WM_MOUSEMOVE`,
  `WM_LBUTTONDOWN` / 24ms / `WM_LBUTTONUP`.
- It also *publishes back* window state (foreground / iconic / visible / hwnd)
  so even the diagnostic logging on the Present path reads an atomic instead of
  calling `FindWindow` / `GetForegroundWindow`.
- This also replaces the per-click `std::thread(...).detach()` that was being
  spawned from inside PresentEx -- thread creation takes the loader lock.

### Other review findings fixed

- **Logging was doing six file opens per frame on the render thread.**
  `Game::logMsg` did `fopen`/`fwrite`/`fclose` on *two* files per call, and
  per-frame tracing emits 2-3 lines. Handles are now opened once and `fflush`ed
  per line -- still crash-complete, without the open/close cost inside Present.

### Review findings NOT fixed (deliberately)

- **`ComputeMenuPointer` has never once hit** (`tip=0` in every log). Both it and
  `PlaceMenuPanelInFront` use `GetTrackingSpace()`, so it is not the space
  mismatch first suspected. SteamVR's own laser works and produces aim points,
  so this is a redundant fallback, not a blocker. Left alone rather than stacked
  on top of an architectural change in the same build.
- **In-map stereo still never exercised.** `eyes=0`, `stereoPasses=0` in every
  log to date. The bracketed stereo-pass logging from 12:39 is in place and
  waiting.

---

## OPEN: freeze on Create Server / map load (12:39)

Menu clicking is **confirmed working** after the 12:11 revert:

```
Overlay MouseButtonDown at (172,394)
Queued menu click at (172,394) down=1 up=0
Queued menu click at (175,392) down=0 up=1
```

The log then stops dead at frame 811. The in-map path has still never produced a
single stereo frame -- `eyes=0` in every log ever captured.

### Why we were blind here

`dRenderView`'s capture logging was gated on `if (call < 8)`, where `s_calls`
increments on **every** RenderView call. RenderView runs every menu frame, so by
the time a map loads the counter is in the hundreds and that log could never fire.
The entire in-map path has been invisible this whole time. Now on its own
`s_stereoPass` counter.

### Real bug fixed

`AfterPresent` read:

```cpp
if (inMap) { if (haveEyes) SubmitStereoToCompositor(); }
else if (!clicking) { ...keepalive... }
```

An in-map frame with no captured eyes therefore submitted **nothing** and never
called WaitGetPoses -- which is exactly the state for the whole of a multi-second
map load. The compositor got no frames at all across the load. The condition is
now `haveEyes && inMap`, so the black keepalive covers the load window.

### Instrumentation for the next run

- **Stereo pass bracketing.** Each half is logged before and after, so a hang
  inside the engine's own RenderView appears as a `BEGIN` with no matching
  `L ok` / `R ok`:
  `stereo pass #0 BEGIN` -> `L render...` -> `L rendered, capturing` -> `L ok` -> same for R.
- **inMap transition** logged once on change.
- **Present watchdog.** A separate thread samples the last Present timestamp; a
  frozen main thread cannot report its own freeze. Logs
  `WATCHDOG no Present for N ms (inMap=.. stereoPasses=..)` after 3s.
  This distinguishes "engine busy loading" (recovers) from "deadlocked" (never
  does), and says how far the stereo path got before dying.

### Reading the next log

| Pattern | Meaning |
|---|---|
| `BEGIN` then nothing, watchdog counting | hang inside the engine's RenderView -- the double-render is not safe here |
| `L ok` then nothing | hang in the second RenderView specifically |
| `R ok` then nothing | hang after capture, in submit or present |
| watchdog silent, log just ends | process died, not a hang -- look for a crash |
| watchdog fires then recovers | it was only the map load; look further down |

---

## REGRESSION I CAUSED: don't hoist overlay input calls (12:11)

The 11:57 build reduced overlay IPC by moving ten sticky `SetOverlay*` calls out
of the per-frame path into a configure-once block. The stutter fix worked (no
`SubmitBlackEyes` stall lines at all in the 12:03 log, `gap` 1-4ms), but the menu
became **unclickable**, and the 12:03 log contains **zero** `Overlay
MouseButtonDown` events where the previous build had them.

**Overlay input routing is not as sticky as the cosmetic properties.** These three
must be re-asserted every frame in `ProcessMenuInput`:

```cpp
SetOverlayMouseScale(...)
SetOverlayFlag(..., VROverlayFlags_MakeOverlaysInteractiveIfVisible, true)
SetOverlayInputMethod(..., VROverlayInputMethod_Mouse)
```

`ShowOverlay` is also back to per-frame: gating it on a local "already shown" flag
means that if SteamVR ever drops the overlay, nothing brings it back.

Only genuinely cosmetic properties stay in the once-block: texture bounds, alpha,
color, sort order, HideLaserIntersection. That is still a real reduction, and none
of it touches input.

**Do not re-optimize the three calls above without a headset test that proves
clicks still land.** The IPC cost is paid deliberately.

### New: MenuHealth diagnostic

Once a second while a menu is up:

```
MenuHealth live=1 moves=412 down=3 up=3 actionsOk=1 sel=0 atk=0 interactive=1 vis=1 tip=0 ctrlPose=1 aim=(566,347)
```

The 12:03 run produced neither an overlay button event nor a digital-action press
from the same trigger pull -- two independent paths failing together. This reports
both, plus whether the action handles even resolved (`actionsOk`) and whether the
controller is tracking (`ctrlPose`). If `actionsOk=0` the action manifest or the
controller bindings never loaded and no overlay work will help.

Also note `tip=0` in every log to date: `ComputeMenuPointer` (controller ray ->
overlay intersection) has **never once hit**. That is the aim path that does not
depend on SteamVR routing laser events to us, so it is worth fixing as a
fallback -- likely a tracking-space mismatch between the ray origin
(`GetTrackingSpace()`) and the overlay's transform.

---

## The periodic freeze was compositor queue backlog (11:57)

Menu now reaches the main menu, pointer and items render, clicks post. Remaining
symptom was a recurring freeze. The log named it:

```
SubmitBlackEyes 123.9ms Lerr=0 Rerr=0
SubmitBlackEyes  81.3ms Lerr=0 Rerr=0
SubmitBlackEyes  86.1ms Lerr=0 Rerr=0
```

That timer wraps **only the two Submit() calls** -- not the texture fetch. So the
cost is inside OpenVR. OpenVR's Vulkan submit path synchronizes against the
graphics queue the app hands it, which means its cost is proportional to how much
GPU work has been queued since the previous submit.

The keepalive was submitting **every 500ms** while the engine free-ran at ~225fps
(`gap` was 1.4-4ms per frame). That is ~110 frames of queued rendering to drain per
submit. 500ms of backlog produced an 83-150ms stall, twice a second. The arithmetic
matches the measurement.

Fixed by cadence, not by removing the submit: `MenuKeepaliveMs`, default **33ms**.
That cuts backlog ~15x (to roughly 5ms, imperceptible) while deliberately NOT
returning to the every-frame cadence recorded as deadlocking against a VGUI click.
`11` is available if 33 proves stable; `500` restores the old behaviour.

### Also fixed this pass

- **Varargs bug.** The `MENU f=...` log line had five format specifiers and four
  arguments -- it was reading a garbage float off the stack (which is why every
  `TOTAL=` printed `0.0ms`). Undefined behaviour in a per-frame path.
- **~2900 IPC calls/sec to vrserver.** `ShowMenuPanel` set ten sticky overlay
  properties (bounds, mouse scale, alpha, color, two flags, input method, sort
  order, show, hide) on *every frame*, and `ProcessMenuInput` re-set three more.
  Overlay properties persist; only the texture changes per frame. Now configured
  once, and again only on window resize.
- **Dead config knob removed.** `MenuBlockingSync` was still parsed but no code
  path read it after the AfterPresent rewrite -- a switch that silently does
  nothing is worse than no switch. Grok's finding that WaitGetPoses must precede
  Submit is preserved as a comment at the call site, where it belongs.

### Architecture worth preserving (from Grok's pass)

- Compositor Submit happens in `AfterPresent`, **after** `m_implicitSwapchain->Present()`.
  Calling WaitGetPoses/Submit *before* the swap deadlocks DXVK Present against
  SteamVR after a handful of frames.
- Menu clicks are posted from a short-lived worker thread, not from the Present
  callstack.
- A 90-frame warmup before menu input goes live, so GameUI finishes building its
  panel list before the cursor is driven.

### Still open

- Hard freeze after clicking a menu item is **not** confirmed fixed -- the 11:47 log
  ends shortly after a click, but a click that starts a map load also legitimately
  stops Present for a while. Needs a run that gets into a map.
- In-map stereo has never been exercised. `eyes=0` in every log so far.
- Eye resolution: eyes capture at window size (1280x720) and upscale to a
  ~2496x2688 eye. Soft. A taller window helps, but launch-arg changes are the one
  thing that has broken boot -- change alone and re-verify.

---

## RESOLVED: the menu stall was compositor blocking (09:14)

The instrumentation answered it. From the 08:57 session, which **worked** and
reached the main menu:

```
MENU f=12 poses=25.1 cap=0.1 panel=0.1 input=0.1 submit=0.2 TOTAL=25.7ms
MENU f=13 poses=24.4 cap=0.1 panel=0.1 input=0.1 submit=0.2 TOTAL=24.8ms
```

`poses` is `WaitGetPoses`. It was **24-25 ms of a 25 ms frame** — the entire frame
budget — while every other stage cost ~0.1 ms. The capture, the overlay, the menu
input and the submit together came to about half a millisecond. **None of the
rendering work was ever the bottleneck.** The engine's main loop was simply chained
to the SteamVR compositor, one blocking call per frame, inside `Present`.

Three sessions, same build, different outcomes:

| Session | Result |
|---|---|
| 08:56 | stalled at frame 4; frame 3 logged `submit=415.4ms` |
| 08:57 | **ran fine**, reached frame 91+, menu items appeared |
| 08:58 | stalled at frame 6 |

Non-deterministic, which rules out a plain logic bug and points at a race with the
compositor. The user also reported it "locked up when I changed window".

### Two fixes

**1. The menu no longer blocks on the compositor.** There is no stereo frame to
synchronize while a menu is up, so `WaitGetPoses` there buys nothing and couples the
engine to SteamVR's frame clock. It is now `GetLastPoses` (non-blocking) unless
`MenuBlockingSync=true`. This should also let title→menu complete, since that
transition needs a number of frames that a stalled compositor was preventing.

**2. Focus is never stolen.** `SetForegroundWindow` was being called from the render
thread, inside `Present`, with the message pump on that same thread. It participates
in cross-process foreground arbitration and can block — the best fit for the lockup on
window change. The cursor is now driven only while the game already owns the
foreground; otherwise the desktop mouse is the fallback.

Note the in-game path still uses `WaitGetPoses`, which is correct — there you *do*
have a stereo frame to time.

---

## PREVIOUS: engine stops after ~3 frames (2026-08-28 08:43)

The crash **is fixed** — the 08:43 log ends cleanly with all three
`Floating menu overlay` lines (the old build died after two) and the eye handles
read `00000000` instead of `"able"`.

But the game never reaches the main menu. Title screen renders on the flat panel,
music plays, no progress.

**The key pattern: both builds stopped after 2-3 frames.** The 01:34 build crashed
at frame 2; the 08:43 build simply stops at frame 3. `VR::Update` logs every 90
frames and `frame=91` never appears in either. So the engine has never managed more
than a handful of Presents in *any* recent build — this looks **pre-existing**, not
introduced by the 01:58 changes. Music continuing (separate thread) and the overlay
holding its last texture is what makes it read as "rendering but stuck".

Two readings fit: a genuine main-thread block, or the game crawling at ~1 fps so
title-to-menu never completes. That matches the older "performance terrible" report.

### Instrumentation added (build 08:54)

Rather than guess a third time, `VR::Update` and `WaitGetPoses` are now timed:

```
WaitGetPoses #N  12.3ms worst=12.3ms err=0
MENU f=3 poses=0.1 cap=2.4 panel=0.8 input=0.2 submit=1.1 TOTAL=4.6ms
GAME f=3 hide=0.1 present=3.2 input=0.4 TOTAL=3.7ms
```

Verbose for the first 40 frames, then every 90, plus any `WaitGetPoses` over 50 ms.
Read it two ways: the **timestamps** give the true frame rate, and the **stage
numbers** say which call eats the time. `WaitGetPoses` is instrumented separately
because it is called from the `CViewRender::Render` hook, not from `VR::Update`,
and would otherwise be invisible.

### Kill switches for bisecting (all hot-reload)

| Key | Default | Rules out |
|---|---|---|
| `MenuDriveCursor` | `true` | all SetCursorPos / SetForegroundWindow / PostMessage |
| `TheaterHideThrottleMs` | `2000` | `0` restores the old per-frame Theater suppression |
| `DisplayMode` | `compositor` | `sbs` restores the old quad |
| `UseVerticalCrop` | `true` | vertical half of the frustum crop (sign convention unverified) |

### Also reverted

`GetBlackTexture` no longer skips `TransferSurface` on later calls. That was a
gratuitous optimization of mine — DXVK can move the image out of
`TRANSFER_SRC_OPTIMAL` between frames, and handing OpenVR an image in the wrong
layout is a correctness hazard for negligible gain.

---

## What was actually wrong

### 1. The crash — two candidate causes, both now removed

The boot log ends at the same point on both runs: two `Floating menu overlay ...
visible=1` lines, then death. Deterministic, so it was findable by reading.

**Cause A (most likely): the VGUI input vtable.** `ProcessMenuInput` called
`IInput::InternalCursorMoved` / `InternalMousePressed` through `VGUI_InputInternal001`.
In the retail Source SDK, vgui's `IInput` derives from `IBaseInterface`, which declares
a **virtual destructor** — under MSVC that occupies vtable slot 0, so every method in
`sdk/sdk.h`'s `IInput` is potentially off by one against the real `vgui2.dll`.

Calling a wrong `__thiscall` slot does **not** raise an access violation. It mismatches
argument counts and corrupts the stack. That explains exactly why the previous session's
`__try/__except` wrapping caught nothing and the process died a few frames *later*
rather than at the call. It is the best fit for the evidence.

→ Menu input now defaults to posting real Win32 messages to the `Valve001` window,
which is layout-independent. The vtable path is behind `MenuInputVguiInternal=false`.

**Cause B (latent, real regardless): uninitialized memory.** `VR` is built with
`new VR(this)`, a *user-provided* constructor, so members without initializers hold
heap garbage. `SharedTextureHolder` had none. That is why frame 1 logged:

```
VR::Update frame=1 ... left=656C6261 right=00016400
```

`0x656C6261` is ASCII `"able"` — stale heap text sitting where a texture pointer
belongs. Every filler sets `handle = &m_VulkanData`, so a valid handle can only ever be
`0` or a real address; `"able"` proved the struct was never initialized. A garbage
non-null handle passes every `if (!...handle)` check in the codebase and gets handed
straight to OpenVR.

→ All six holders, plus the pose array, texture bounds, eye/surface pointers, FOV,
aspect and every action handle are now zero-initialized. A new
`VR::TextureReady()` helper checks the self-pointer signature instead of mere
non-null, and guards every `SetOverlayTexture` / `Submit` call site.

### 2. "Feels like a giant 3D TV" — the frustum crop was computed and never used

This was the big one. The constructor derives `m_TextureBounds[0]` and `[1]` from
`GetProjectionRaw` — the correct asymmetric per-eye crop — and then **nothing in the
entire codebase ever read them.** Every `Submit()` passed `&full` (0,0,1,1) or `nullptr`.

The pipeline renders each eye once at a symmetric *superset* FOV (`m_Fov` ≈ 106°, the
max of all four half-angles). That is the right technique. Step two — pulling each eye's
real asymmetric rectangle back out of that superset — was never wired up. So the
compositor stretched a 106° image across an eye that is not 106° wide, which pushes the
effective optical centres outward and makes the whole world read as oversized.

For a typical headset the left eye should use roughly `u[0.000 .. 0.946]` and the right
`u[0.054 .. 1.000]`. Submitting `0..1` to both is the "giant screen" bug.

→ `SubmitStereoToCompositor()` now applies `m_TextureBounds`. Toggle with
`UseEyeFrustumCrop=false` to reproduce the old look for comparison.

Note this also means the old SBS quad **could never have felt right** — one flat
rectangle cannot reproduce two asymmetric eye frusta, no matter how you size it.

### 3. Performance — three separate causes

- **`WaitGetPoses` was called twice per frame.** Both `CViewRender::Render` and
  `CViewRender::RenderView` called `UpdatePosesAndActions()`, and Render calls RenderView
  internally. `WaitGetPoses` blocks until the compositor's running start, so the second
  call parked the render thread for a **full frame period**. This is very likely the
  dominant cause. Now latched to once per frame via `m_PosesThisFrame`.
- **Four blocking GPU stalls per frame.** `TransferSurface(surface, TRUE)` called
  `WaitForResource` — a full CPU-blocks-on-GPU sync — and ran four times per in-map
  frame (left eye, right eye, SBS blit, black texture). OpenVR only needs the copy
  *submitted*, not completed; it synchronizes on its own side. Now a `Flush()`.
  (The previous session correctly removed `WaitDeviceIdle` from Present but missed this.)
- **~900 IPC round-trips per second.** `GESVR_HideTheaterOverlays()` ran on every
  Present, doing a `SetBool`, `CompositorBringToFront`, `FadeGrid` and **seven**
  `FindOverlay` lookups — every one an IPC call to `vrserver`. Now throttled to once
  every 2 s, forced at startup.

Compositor mode also no longer does the SBS blit or the per-frame black-texture path
at all, so that is two more `StretchRect`s and a flush saved per frame.

### 4. Motion weapons — the grip correction never ran

`VR::UpdateTracking()` is **dead code with zero call sites.** It is where the ~45°
grip correction, the two-handed rifle grip and the per-weapon viewmodel offsets lived,
so none of them have ever executed. The gun pointed straight down the controller's
device axis, which is not where a barrel points when you hold one.

→ The grip correction now runs where the viewmodel basis is actually built, in
`ApplyHeadAndIpd`, and re-derives `m_RightControllerAngAbs` from it so the rendered gun,
the shot direction and the view angles while firing all agree. Tunable live:

```
GunGripAngle=45.0        # 0 = aim straight down the controller axis
ViewmodelOffset=0,0,0    # forward,right,up in Source units
```

Also worth knowing: `m_ViewmodelPosOffset` was one of the uninitialized members, and it
is subtracted in `GetRecommendedViewmodelAbsPos()`. Gun placement was being offset by
*garbage* before this session. That may explain the odd
`viewmodel motion #0 ... -> (1168,593,-430)` in the old log.

---

## Morning test plan

Launch:

```
powershell -ExecutionPolicy Bypass -File "G:\Like Grok Work\GESVR\Launch-GESVR.ps1"
```

Check `%TEMP%\gesvr_boot.log` in this order:

| Look for | Means |
|---|---|
| `-width 1280 -height 720` in cmdline | correct args (never 1280x1280 — that killed boot) |
| `client.dll ready after N ms` | injection is healthy |
| `Eye frusta: superset fov=... L u[...] R u[...]` | **new** — confirms the crop was computed |
| `DisplayMode=compositor frustumCrop=1 ...` | config was read |
| `Menu input: hwnd=... win32=1 vguiInternal=0` | **new** — confirms the game window was found |
| `compositor stereo frame=N Lerr=0 Rerr=0` | **new** — frames are reaching the HMD |
| `Menu click #N at (x,y) hwnd=... fg=1` | **new** — a click was posted; `fg=1` means focused |

Then, in order:

1. **Does it survive the menu?** That is the crash that sent you to bed. Just let it sit.
2. **Does the laser click?** Point at a menu item, pull the trigger. If the log shows
   `Menu click` lines but nothing happens, check `fg=` — if it is `0`, Windows is
   refusing the foreground change and the desktop mouse remains the fallback.
3. **Load a map. Does the world have correct scale?** This is the real question. It
   should feel like standing in the level, not looking at a screen. If it is black,
   set `DisplayMode=sbs` and save — it comes back live.
4. **Perf.** Should be dramatically better. The double `WaitGetPoses` alone was
   costing a full frame.
5. **Gun.** If it points too high, lower `GunGripAngle`; too low, raise it.

---

## Known landmines (unchanged, still true)

| Don't | Why |
|---|---|
| Square 1280×1280 window | hangs/dies before client.dll — this is what broke boot on 08-28 |
| HMD recommended RT / swapchain resize | `client.dll+0x28ac17` crash |
| `WaitDeviceIdle` every Present | GPU stall |
| `CreateMove` hook | wrong ret; left unhooked deliberately |
| `CalcViewModelView` at `0xD89A0` ret4 | wrong; use `0x115360` ret12 |
| `RenderView` at wrong addr | use `0x288BA0`, 3-arg (2007 has no `hudViewSetup`) |
| MessageBox on missing signatures | log only |
| Construct `Game()` before client.dll | first-launch crash |
| Same VkImage to overlay and compositor | overlay crash ~1 s |
| Re-aim menu overlay every frame | laser unusable; yaw locked until ~70° |
| Trigger without ~350 ms cooldown | a million clicks |
| Two d3d9.dll copies without the mutex | double `Game()` |
| `IsInGame()` from D3D code | deadlock |

---

## Build & install

```
"F:\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "G:\Like Grok Work\GESVR\l4d2vr.sln" /p:Configuration=Release /p:Platform=x86 /m /v:minimal
```

Post-build copies `Release\d3d9.dll` → `dist\d3d9.dll`. Then **both** copies must be
installed, or the system d3d9 wins and no log ever appears:

```
...\Source SDK Base 2007\d3d9.dll
...\Source SDK Base 2007\bin\d3d9.dll
```

Note: MSBuild's `/p:` switches get mangled by Git Bash into paths. Run the build from
PowerShell, not bash.

---

## Next steps, in priority order

1. **Confirm the crash is gone and compositor stereo is visible.** Everything below is
   blocked on that answer.
2. **Eye resolution.** Eyes are captured at the window size (1280×720) and upscaled to
   a ~2496×2688 eye — vertically undersampled by ~3.7×, so it will look soft. The fix is
   a taller window (≈1440×1080 is much closer to the eye's ~0.93 aspect than 16:9),
   **but launch-arg changes are the single thing that has broken boot before.** Change
   it alone, and re-verify boot before touching anything else.
3. **Menu clicks**, if the Win32 path still does not take. Next thing to try is the
   `IInput` destructor-slot theory documented in `sdk/sdk.h`.
4. Wrist HUD (config already present), MP-safe gun pose, hurt HUD.

## Dead code worth knowing about

`VR::UpdateTracking()` and `VR::SubmitVRTextures()` both have **zero call sites**.
Don't assume anything inside them is running. `UpdateTracking()` in particular contains
roomscale movement and two-handed grip logic that has never executed; wiring it back up
is a real feature opportunity, but it dereferences `C_BasePlayer` and needs care.
