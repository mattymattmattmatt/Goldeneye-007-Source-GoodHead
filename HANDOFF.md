# GESVR — GoldenEye: Source VR — Handoff

Last updated: **2026-09-25**, after v1.0 and the main-menu skin (below).
Owner: Matty. Headset: SteamVR. Target quality: HL2VR / HaloCEVR, not "2D in Theater".

---

## SKINNING THE MAIN MENU: BACKGROUND AND MUSIC (2026-09-25)

Matty wanted the GoodHead key art behind the menu and his own title track over
it. Both are GE:S files, so the launcher replaces them and keeps the originals.

**The background name is the engine's, not the mod's.** Strings in
`Source SDK Base 2007\bin\engine.dll`:

```
materials/console/%s.vtf
materials/console/%s_widescreen.vtf
materials/console/background01.vtf
materials/console/background01_widescreen.vtf
scripts/ChapterBackgrounds.txt
```

`%s` comes from `ChapterBackgrounds.txt`. GE:S does not ship that file, so the
engine falls back to the built-in `background01`, and the only way in is to BE
those two files. Not the VMTs beside them -- the engine names the `.vtf`
directly, so repointing `$basetexture` at a new texture is not enough.

**The VTF shape that works.** Both GE:S originals are VTF 7.2, DXT1, one mip,
flags `0x300` (`NOMIP|NOLOD`), with a DXT1 thumbnail, 80-byte header:

```
background01.vtf             1024x1024  thumb 16x16   80 + 128 + 524288 =  524496 bytes
background01_widescreen.vtf  2048x1024  thumb 16x8    80 +  64 + 1048576 = 1048720 bytes
```

`tools\Make-MenuAssets.py` writes exactly that, and the generated files come out
at those same two byte counts -- a free structural check that the header,
thumbnail and mip data all landed where the engine expects. Header fields were
verified field by field against the shipped files; only reflectivity differs.

The encoder is ours (numpy): principal-axis endpoints per 4x4 block, then two
least-squares refits against the indices they produced. 35-37 dB PSNR on this
artwork. Do not be tempted by an uncompressed format here -- BGR888 at 2048x1024
is 6 MB of a 2 GB address space that already crashes on map load.

**ASPECT: the texture has to be pre-distorted.** The engine stretches the
texture over the whole screen, so a 2048x1024 (2.00) texture on a 16:9 (1.778)
screen is squeezed horizontally by 0.889. Feed it a 16:9 picture stretched by
1.125 and it comes out right. Same reason the 4:3 file is a square: a 4:3 centre
crop squashed into 1024x1024, which the 1.333 stretch undoes. Get this backwards
and Brosnan is 12% too wide.

**The music is GE:S's, not the engine's.** `gameui.dll` globs
`sound/ui/gamestartup*.mp3`, and GE:S ships only `gamestartup1.wav` there (1.2 KB
of silence), so the engine's menu music never fires. GE:S's own `client.dll`
holds `scripts/music/level_music_%s`, reads the list and plays one entry at
random -- so `scripts\music\level_music__menu.txt` is the menu playlist. Nine
GoldenEye tracks by default; we replace the list with one file.

**Backups.** Every replaced file is copied to `<name>.gesvr-orig` first, and
`$goodheadMenu = $false` in the launcher restores them and deletes the files we
added. Tested: three installs then two restores leaves the three originals
byte-identical with nothing of ours left behind.

**Bug worth remembering.** The backup test started as "back up if there is no
backup yet". `goodhead_title.mp3` has no original, so on the *second* launch
that test was still true and the launcher saved OUR OWN mp3 as the "original" --
after which restore had something to put back and left the track installed for
good. The condition also has to check that the destination is not already ours:

```powershell
if ((Test-Path $dest) -and -not (Test-Path $orig) -and -not (Same-File $src $dest))
```

Any "snapshot the original once" scheme that can be handed a file it already
wrote has this hole. Run the installer twice in the test, not once.

---

## MAP LOADS CRASHING: DXVK COMPILING ON 14 THREADS (2026-09-24)

Matty: "it crashes everytime i try to load the map. it get futher each time i
try like it must be caching or something and then if i restart enough its load
the map."

* **Signature**: three crashes 19:27-19:28 -- client.dll 0xc0000005, then
  ucrtbase 0xc0000409 twice at the same offset (0x9eddb): the runtime aborting,
  the shape of an allocation failure ending in abort(). Textures on High
  (mat_picmip 0). hl2.exe cannot be made large-address aware (see the section
  on that), so the process has 2 GB.
* **Cause (strongly indicated, confirm from the new MEMORY lines)**: DXVK
  compiles pipelines through the NVIDIA driver inside the 32-bit process, and
  on an i7-8700 (12 threads) its auto setting ran 7 state-cache compiler
  threads AND 7 async compiler threads -- hl2_d3d9.log: "Using 7 async compiler
  threads", "Read 2794 valid state cache entries", "Using 7 compiler threads".
  This dxvk-async fork creates the async compiler unconditionally
  (dxvk_pipemanager.cpp, the enableAsync test is commented out). Fourteen
  concurrent driver compiles is a large transient spike in the same 2 GB as
  the textures, at exactly the moment a map loads. "Further each time" fits:
  what compiles before a crash is cached, so the next burst is smaller.
  The reinstall may have made it worse: NVIDIA's cache in
  steamapps\shadercache\218\nvidiav1\GLCache was last written 08:40, before
  the evening's crashes, so crashed sessions never saved what they compiled.
* **Fix**: dxvk.conf `dxvk.numCompilerThreads = 2`, `dxvk.numAsyncThreads = 2`,
  in dist\dxvk.conf and written straight into Matty's installed copy (the
  launcher never overwrites a player's dxvk.conf). Cost: a map's first seconds
  may show pop-in while the rest compile.
* **Instrumentation**: the watchdog thread now logs
  `MEMORY <used> of <total> MB address space in use (peak N), largest free
  block N MB` on every 64 MB of growth, and the WATCHDOG stall lines carry the
  same numbers. The last MEMORY line before a crash says how close it was.
* **Next levers, in order, if it still dies**: threads 1 and 1;
  `dxvk.enableStateCache = False` (no startup burst of 2794 compiles);
  Medium textures; a 1920x1080 window (the eye surfaces, backbuffer and
  captures are full window size).

## THE SETTINGS PANEL TAKES THE TRIGGER AND NOTHING ELSE (2026-09-25)

Matty, on the v1.0 README: "it only opens with the clicking the menu option
with the trigger and then trigger to do anything and clicking the close button
eith the trigger. the other buttons do nothing."

So, tested in a headset at last: **every action-driven shortcut on the VR
settings panel is dead.** X does not open it, B / Y / left X do not close it.
Only the SteamVR laser works -- opening it from the GameMenu.res entry, moving
between rows, and the Close button.

This confirms the hypothesis from 2026-09-23 (the session that got stuck with
the game menu over the Close button) and kills the fix that was written for it:
while SteamVR's laser is driving an interactive overlay it takes that hand's
input, so `PressedDigitalAction` reads inside VRSettings::Frame never fire.
The trigger works because SteamVR delivers it to the overlay itself as
VREvent_MouseButtonDown, not through the action system at all.

`VR::LegacyMenuButtonDown` (GetControllerState, k_EButton_ApplicationMenu) was
added as a way round that and **does not work either** -- SteamVR's legacy
input is evidently not answering for this device. It is left in place, harmless
and doing nothing, rather than removed on one negative result.

The panel's own footer used to read "Changes apply and save immediately.
B / Y / left X: close". That line was written from the code, never from a
headset, and it was a lie in the player's face. It now reads only the first
sentence. The README no longer claims X opens it. **Do not put a button hint
back on that panel without pressing the button in a headset first.**

If someone wants the shortcuts to work, the lead is the overlay event queue:
the panel already reads VREvent_MouseButtonDown from it, and whatever else
SteamVR delivers there is the only input that reaches an overlay with the laser
on it.

## THROWING KNIVES WENT INTO THE THROWER (2026-09-24)

Matty: "with manual throwing knives, i cant throw them properly they look like
they might be hitting myself ... see if we can get it to release later or
futher through my throw".

The swing is detected the moment hand speed crosses SwingSpeed (2 m/s), which
is the START of the wind-up: the arm is still going back and the blade points
at your own head. GE:S spawns the knife at the EYE (Weapon_ShootPosition + 2
forward + 3 right) along the eye angles, so a release there puts a 16-unit hull
inside the thrower, aimed backwards. Worse, this code then took the direction
from `PointingDirAgo(80)` -- 80 ms EARLIER again, deeper into the wind-up.
(That was added on the reasoning that a flick rotates the wrist off target, so
the pre-flick pointing direction would be truer. In a real throw the wind-up is
the one moment the hand points the wrong way, so it was exactly backwards.)

Now the flick only ARMS the throw. The release comes at whichever is first:
* the hand passing its peak speed and dropping below 65% of it -- the natural
  end of a throw, when the arm is coming round to the target; or
* ThrowReleaseMs (config, default 150) after the arm began, for a slow lob that
  never shows a clear peak.
The direction is taken at the hand's FASTEST moment, not at the release test.
Taking it at the release threw knives into the ground (Matty, same evening):
by the time the hand is measurably slowing, an overhand throw has curved past
the target into the follow-through and the velocity points at the floor. Peak
speed is where a real throw lets go.

Even at the peak a swung arm aims a little low, while the hand you consciously
aimed with does not, so the direction is a mix of the two at that instant:
`ThrowAimMix` (config, default 0.5), 0 = where the hand was travelling, 1 =
where it was pointing (which is what the throw guide draws, so 1 makes the
guide exact at the cost of ignoring the swing). Below 1.5 m/s the motion says
nothing and pointing is used outright.
The knife also gets the gun's aim gate back (apply the angles, then attack the
next frame) so the view angles are on the throw direction before +attack goes
out; grenades and mines still skip that gate, because they must not lock the
view while held.

Log line per throw (12 max): `Throw released N ms in by slowing|time, peak X
m/s: motion=(...) pointing=(...) mix M -> (...)`. It carries both candidate
directions, so the next report can be settled by reading them rather than by
another guess: if motion is much lower than pointing and throws still land
short, raise ThrowAimMix; if knives fly where the head looks rather than the
hand, the two will agree and the fault is elsewhere.

## FACE AIM IGNORED THE HEIGHT SETTING, AND WHAT COSTS FRAMES (2026-09-24)

Matty: "I just tried changing my height and in free aim it works well but with
face aim the gun stays at the original position ... also im not getting the
best performance, seems to be hitching a little".

**Face aim's viewmodel now rides the eye we render from.** The engine draws it
at ITS eye (m_SetupOrigin); we render from setup.origin + the roomscale head
offset + HeightOffsetMeters * VRScale. Nothing carried the difference, so the
gun kept the height the game thought you had. ApplyHeadAndIpd now publishes
`m_ViewEyeDelta` (rendered eye minus game eye) and FaceAimBones adds it to the
same translation it already applies for the FOV spread, before the eye squash.
This also fixes leaning: the face-aim gun used to swim when you moved your head
in roomscale. Free aim was never affected -- its weapon is placed at the
controller, which is built from the same corrected camera.

**Hitching: two of the causes were added on 2026-09-24 and are now fixed.**
* The MEMORY probe walked the whole 2 GB address space with VirtualQuery ONCE A
  SECOND. That walk takes the process's address-space lock -- the same lock the
  game needs to allocate -- so it stalled allocations. Now the per-second sample
  is a single GlobalMemoryStatusEx, and the walk (for "largest free block")
  happens only on the rare line that is actually logged.
* The throw guide simulated at 1/40 s, up to 120 engine hull sweeps per frame
  while a throwable was held. Now 1/20 s: half the traces, and the chord error
  against the true arc is about 0.2 units, far below the size of what it hits.
* Per-frame tracing in the present path (FRAME / VR::Update / CURSOR) fired
  about once a second EACH, and every logMsg is a printf plus a flushed write to
  two files under a mutex, on the present thread. Now once per ~900 frames.

**New: PACING lines.** Every 10 s in a map the log gets
`PACING <n> frames in 10 s (<fps>), mean <ms>, worst <ms>, <n> over 22 ms`.
That separates a steady drizzle from occasional big stalls before anything else
is changed.

**Still-suspected costs, in order, none of them measured yet:**
1. The wrist watch redraws whenever its content changes -- which includes the
   round timer, so at least once a second -- and the render thread then has
   SteamVR load that PNG from disk (VRWatch::Place -> g_face.Load). A once-a-
   second overlay upload is exactly the shape of the reported hitch.
2. DXVK's compiler threads were capped at 2 + 2 today to stop the map-load
   crash. Fewer threads means a new pipeline takes longer to appear, and the
   2794-entry state cache takes longer to work through after launch. If the
   PACING lines show the stalls early in a session, raising it to 4 is the
   first thing to try -- with texture detail still on Medium.
3. The eye capture does a full-size StretchRect plus a Vulkan transfer per eye
   per frame at 2560x1440. Constant cost, not a hitch, but it is the floor.

## THE DEAD AIM VECTOR: m_RightControllerForward (2026-09-24)

Matty: "the aiming position is still staying in one direction ... no matter
what direction i point the grenade only goes lets say north and i cant get it
to go higher or lower either and if i try to run while cooked my running
direct gets messed up".

**`VR::m_RightControllerForward` is never updated. It has always held its
header value, `{1,0,0}`.** The only code that assigns it is `VR::UpdateTracking()`,
which has NO CALL SITE -- the note at the top of the per-weapon pose block in
ApplyHeadAndIpd says as much ("lived in UpdateTracking(), which has never had a
call site") but the vectors themselves were left behind with no warning on them.
The throw guide read it, so every grenade and mine was aimed along world +X
with zero pitch: one fixed direction, no up or down, exactly as reported. The
guide's landing points still moved a little between log samples, which is what
made this look like a throw-side problem at first -- but that was the player
walking, not the aim turning.

Use **`VR::HandForward()`** (new) for hand aim: the forward vector of
`m_RightControllerAngAbs`, which ApplyHeadAndIpd's placeController refreshes
every frame and which already carries the grip correction as a plain pitch
offset. The dead members now carry a comment saying all this. The live basis is
`m_ViewmodelForward/Right/Up`; the left-hand set (`m_LeftControllerForward`
etc.) IS live -- it is filled by the same placeController call -- which is why
the off-hand work was unaffected.

Also fixed, from the same report: **cooking a grenade wrecked movement.** The
attack-aim lock was re-armed every frame the trigger was HELD
(`m_AttackAimUntil = now + 150`), so the view angles stayed pinned to the hand
for the whole cook, and the game walks along view angles. Throwables are now
excluded from that lock (and from its one-frame gate, so they still attack at
once): the direction only needs to be right when the item leaves, which the
freeze windows cover -- press for a mine, release for a grenade.

The throw guide's log line now carries `dir=(x,y,z)`, the live hand direction.
If it ever stops changing as the hand points, an aim vector has gone dead again.

**Worth a look when there is time:** UpdateTracking() is ~200 lines of dead
code that still compiles, and it is where the ONLY writes to those vectors
live. Deleting it (and them) would make this class of bug impossible. It was
not done now because it is a large edit in a file under active change.

## THROW GUIDE, FIRST TEST BACK (2026-09-24)

Matty: "the mines reticle need work, its only going in one direction and not
where i point so i cannot place them where i want them. also the reticle is too
chunky and distracting. just needs to be a thin dotted path to a slight bigger
dot at the end point. doesnt need the ring"

* **Mines flew where he LOOKED, not where he pointed, and the guide was
  innocent.** The guide followed the hand the whole time (the log's
  `Throw guide: kind=3 lands at ...` lines vary sample to sample). The throw
  did not: GE:S's CGEWeaponMine::PrimaryAttack only starts the animation and
  sets `m_flReleaseTime = curtime + GetFireDelay()`, and **ItemPreFrame spawns
  the mine when that time arrives, along EyePosition/EyeAngles of that
  moment** (weapon_mines.cpp). Our aim hold was the gun's 150 ms, so the view
  angles had already snapped back to the head. The direction is now frozen at
  the PRESS (where the guide was pointing when you committed) and held 800 ms,
  which covers the throw animation. Grenades keep their release-edge freeze:
  they really are thrown 0.1 s after the trigger comes up.
  The mines' fire_delay could not be read to set the hold exactly --
  gesource/scripts/weapon_*.ctx are ICE-encrypted -- so 800 ms is deliberately
  generous. If a mine ever still lands off-aim, raise it before looking
  elsewhere. Note the cost of a long hold: the game walks along view angles, so
  moving while the hold is active drifts you toward where you pointed.
* **Restyled to a thin dotted path and one end dot.** The ring (16 dots laid on
  the surface, draining with the fuse) and the blast circle (40 dots) are gone;
  up to 51 dots and a 7 px blob became 15-25 dots at 3 px down to sub-pixel.
  Path dots are world-sized (0.17 units) so they recede with distance; the end
  dot is screen-sized (0.0021 of eye height, about 3 px at 1440) so it stays
  readable at the far end of a long throw without being fat up close -- that is
  what the `onScreen` flag on the emit lambda is for. The DXVK clamp went from
  0.8-7 px to 0.55-4.5 px. The end dot still turns red when a grenade would
  catch you; the fuse now shows only as the path stopping at the burst point.

## MAP LOAD MEMORY, MEASURED (2026-09-24)

The new MEMORY lines caught the load that crashed, at texture detail High:

    19:43:39  1295 of 2047 MB in use, largest free block 430 MB
    19:43:41  1554 MB, largest free block 165 MB
    19:43:42  1915 MB, largest free block  18 MB
    19:43:43  1945 MB, largest free block  10 MB      <- then it died

So the 2 GB ceiling is real and the failure is fragmentation as much as volume:
by the end the biggest contiguous hole was 10 MB, and a texture or a driver
shader compile needs one unbroken block. At Medium, with the compiler threads
capped at 2 + 2 (hl2_d3d9.log now says "Using 2 compiler threads" / "2 async"),
the same map loaded on the second attempt and play peaked around 1836 MB.
A stall was logged at 19:47 -- `WATCHDOG no Present for 5563 ms (inMap=1)` at
1689 MB with an 87 MB hole -- worth watching; if it recurs, it is the next
thing to chase.

## THROW GUIDE (2026-09-24; untested in the headset)

Matty: "with the throwing items is there a reticle or something we can add to
help the aim on them? currently its pretty hard to know where its going".

**Why throws were hard to aim.** GE:S launches every thrown item from the
EYE. The mod pointed the eye at wherever a STRAIGHT ray from the hand landed
-- right for a bullet, wrong for anything that falls -- so grenades dropped
short of what you pointed at, and nothing showed the arc or the landing.

**What it does now** (VR::UpdateThrowGuide, drawn by d3d9_vr.cpp through
L4D2VR/vr_guide.h):
* A dotted arc from the gun hand to a ring where the item first lands. The
  ring lies on the surface hit (trace plane normal): flat on floors, upright
  on walls, where a mine will stick. Dots are spaced evenly along the path so
  perspective makes them recede; world-sized, clamped to 0.8-7 px.
* A cooking grenade's fuse drains the ring (16 dots, one per 0.25 s). If the
  fuse runs out in flight, the arc stops at the burst point and the ring
  faces the viewer there.
* Grenade only: the ring turns red, and the blast radius (260) is drawn flat at
  the landing height, when your chest would be inside it.
* Uses the reticle colour. VR Settings > Aiming > Throw guide (ThrowGuide).

**Physics, from GE:S's own source** (ges-legacy-code, SDK 2007 -- the
version this runs on; see the file list in the function's comment):
  grenade  eye + 18 fwd + 8 right (hull-checked back from walls, 6 units),
           (forward + 0.1 z, not renormalised) * 750, VPhysics WITH its
           default drag (not modelled -- expect it to land a little short of
           the ring on long throws), 4 s fuse from the pin (press + 0.1 s),
           released 0.1 s after the trigger comes up, damage 320 radius 260
  knife    eye + 2 fwd + 3 right, forward * 820, VPhysics, EnableDrag(false)
  mine     eye, forward * 600 + up * 80, MOVETYPE_FLYGRAVITY (an exact
           parabola), sticks to the first surface, radius 120
  all + the thrower's velocity (estimated from the eye's frame-to-frame
  motion, eased), sv_gravity 600 (GE:S's cfg does not change it). The knife's
  SetGravity(540) is on a VPhysics object, where entity gravity does not
  apply, so 600 is used for it too -- worth confirming by eye.
Simulated at 40 steps/s for up to 3 s with 2-unit hull sweeps
(MASK_SHOT_HULL, local player skipped).

**Aim changes (Matty approved both):**
1. Thrown items fly along the hand's POINTING direction
   (m_RightControllerForward, the dominant hand after the left-handed swap),
   not at a straight ray's hit. The arc is drawn from the hand and eased into
   the true eye-launched path over its first 0.25 s, so the ring is exact.
2. The knife flies where the blade pointed 80 ms BEFORE the flick was detected
   (a 32-frame pointing history), not along the flick's velocity. The log line
   `Throw ... blade=(...) flick=(...)` shows both.
Grenades and mines freeze the pointing direction at trigger release (30 ms
before) and hold the view angles on it for 450 ms, because GE:S spawns the
grenade 0.1 s after the release along the eye angles of that moment; guns only
needed 150 ms.

**First headset session: read these log lines.**
* `Throw guide: kind=1 lands at (...) N units away, T s, fuse F s, ...` --
  every 3 s while a throwable is held (12 lines max). A 45-degree grenade
  throw from standing should land about 1120 units (~28 m) away in about
  2.1 s.
* Where the grenade actually lands versus the ring tells how much its drag
  matters; if it lands consistently short, scale the launch speed down in
  UpdateThrowGuide rather than guessing a drag model.

## FULL AUDIT (2026-09-24)

Everything since v0.3-sharper, checked against evidence rather than notes.

**Fixed during the audit**
* **Launcher was not portable (release blocker).** `$gameLink = "G:\gesource"`
  dated from the first snapshot: on any machine without a G: drive the junction
  failed and the launcher stopped. It now passes the RELATIVE `-game gesource`,
  resolved against hl2.exe's folder through the `Source SDK Base 2007\gesource`
  junction it already made. Tested: GE:S loads and `GoldenEye: Source VR
  initialized.` (2026-09-24 08:40). The old G:\gesource junction on Matty's
  machine is now unused and harmless.
* **Bindings referenced two actions that do not exist.** All three files bound
  the right stick's left/right to `/actions/main/in/boolean_turnleft|right`,
  which are not in action_manifest.json and are never read by the code (turning
  is the `Turn` vector action). Removed in L4D2VR\SteamVRActionManifest, rebuilt,
  verified identical in source, dist and both installed folders. Possibly why
  SteamVR's binding page would not activate the config; not proven.
* The launcher printed the log path without `bin\`.
* The launcher's steamclient.dll rename is gone (unproven; see above) and the
  G: install's copy is back to stock.

**Checked and clean**
* Every shipped file matches across L4D2VR (source), dist and the G: install:
  manifest, action manifest, three bindings, d3d9.dll (root and bin),
  openvr_api.dll, Bink proxy, dxvk.conf, default config.txt. dist\d3d9.dll is as
  new as the newest source file.
* config.txt: all 99 keys are read by the code (six of them by name lookup, not
  Cfg*), every key the code reads is documented, no duplicates.
* Actions: every action the code asks for exists in the manifest. Deliberately
  without a default button: Flashlight (GE:S has none), ShowHUD, Spray. Pose,
  skeleton and vibration actions are defined but unused (raw poses are read).
* Code review of the uncommitted diff (hooks, vr, vr_settings, vr_watch,
  vr_events, game): no bugs found. Left-handed mode is right (GetPoses swaps
  the roles, so "left controller" is always the off hand). vr_events calls the
  engine by raw slot but prologue-checks each slot first and turns itself off
  for good on a mismatch; every engine read is SEH-guarded.
* Release package (tools/Make-Release.ps1): 17 files, 1.0 MB -- launcher,
  README, dist, licences; no logs or personal settings.

**Known, low priority, left alone**
* Unsaved numpad off-hand tweaks are reset if anything else saves config.txt
  in the meantime (the hot reload re-reads the saved values). Press 0 to keep.
* manifest.vrmanifest names `l4d2vr_capsule_main.png` (L4D2VR's art, not
  shipped) and a `Launch-GESVR.bat` beside it that does not exist. Harmless:
  the manifest is only registered, temporarily, while the game runs.
* The game's own HUD is stretched 1.84x vertically (see the stretch section);
  the fix is per-eye render targets, planned after the trailer.
* The live session used for the audit had no headset connected
  (`VR_Init failed: Hmd Not Found`), so VR behaviour was not re-tested.

## OFF HAND AND THE MODEL WATCH (2026-09-23 night; untested in the headset)

Matty: "the mines and grenade all show the left arm model, it seems like its
one mesh with the right arm... are we able to decouple it and use it for our
left arm and align our watch to it". Done, both.

* **The arms are one model but two bone chains.** Checked offline with
  scratchpad/mdl_arms.py (bodyparts + .vvd weights): v_grenade and the mines
  have bodyparts hand_R, hand_L and item, and mirrored L_/R_ chains with
  their own weighted vertices, so the left arm can be driven on its own
  without touching the right. Guns have no hand_L at all - those weapons keep
  showing no left arm, which Matty accepted.
* **Finding it** (hooks.cpp, ArmRig::leftHand/leftArm/watchBone/leftSet):
  FindLeftArm walks the bone list once per model. A bone is left if its name
  starts with "L_" or its parent is already in the left set (the chains are
  strictly parented, so one pass in bone order is enough - Source stores
  parents before children). The hand anchor is L_FK_Hand_jnt; the fold bones
  are L_FK_Collar_jnt / L_FK_Arm_null / L_FK_Shoulder_jnt / L_FK_Elbow_jnt,
  mirroring what MeleeHideArm already does on the right.
* **Driving it**: in TrackedWeaponBones, a second anchor block builds leftTotal
  from GetLeftControllerAbsPos() plus m_LeftHandOffset in the controller basis
  (m_LeftControllerForward/Up) and m_LeftHandAngle, eased at 0.02 like the
  weapon. The bone loop then picks per bone:
  Concat(InLeftArm(rig, i) ? leftTotal : total, src[i]).
* **The watch overlay follows the model watch** (WatchFollowModel=true): the
  grenade/mine left wrist carries a Seamaster (bones seamaster, beep, hours,
  min, sec under L_FK_Wrist_int). VR::NoteModelWatchPose turns the seamaster
  bone's world position into an offset in off-hand device space
  (forward/left/up, metres), eased 0.08, rejected past 0.6 m as a bad pose,
  and VRWatch::Place uses it when m_HaveModelWatch and not left-handed.
  Guarded to once per stereo frame (static s_watchFrame != g_stereoFrame),
  so both eyes do not push the same sample twice.
* **Tuning**: LeftHandOffset and LeftHandAngle ship at 0,0,0 and hot-reload
  from config.txt. In the headset, VR Settings > Weapons > Adjust position,
  then numpad 7 switches the numpad between the weapon and the off hand;
  0 saves the off hand to config.txt (VRSettings::SaveConfigValue wraps the
  same QueueSave the panel uses - its writer thread runs whether or not the
  panel is open). VR Settings > Weapons > Off hand turns the whole thing off
  (LeftHandOnController), which restores the animated left arm.

## NEVER PATCH hl2.exe: STEAM THEN REFUSES THE APP AT THAT LOCATION (2026-09-24, SOLVED)

**Setting the large-address-aware bit on hl2.exe makes Steam refuse the app at
that install location, and restoring the file does not undo it. Launch-GESVR.ps1
now refuses to start a modified hl2.exe.** The cure is to move the install to a
different Steam library.

### The chain, each link proved by experiment

1. **Trigger.** Set bit 0x20 in hl2.exe's PE Characteristics (0x0102 -> 0x0122,
   the classic "4 GB patch") and launch: instant
   `SteamStartup() failed: SteamAPI_Init_Internal failed`, ~30 MB, 4 threads,
   no mod code reached. Reproduced twice, on two different installs.
2. **Persistence.** Restore the exe from a backup taken seconds earlier
   (hash-identical, 0x0102): still fails. It keeps failing after Steam's file
   verification, clearing Steam\appcache, logging out and in, restarting Steam,
   rebooting Windows, re-registering the app from a backed-up manifest, and a
   full 3.7 GB reinstall into the SAME library.
3. **Location, not files.** A byte-identical copy of the failing install
   (6,325,742,840 bytes, robocopy /XJ) runs fine once registered in another
   library: "Source Engine Test" and then `GoldenEye: Source VR initialized.`
   Moved back to the original library, the same files fail again. So Steam
   holds the poisoned state against the app's install location, and moving the
   install is what clears it -- no download needed.
4. The first poisoned location (G:, 2026-09-23 23:18) works again as of
   2026-09-24 08:12, while the second (F:, 00:50) still fails. Either Steam
   keeps one bad location per app and the second poisoning replaced the first,
   or the state expires after several hours. Not distinguished; it does not
   change the fix.

### What it is NOT (each ruled out by test)

* Not the mod: the stock game (no d3d9.dll, no proxy, no hl2.exe.local) fails
  identically in a poisoned location.
* Not a DRM wrapper: hl2.exe has five plain sections (.text .rdata .data .rsrc
  .reloc), no `.bind` (SteamStub), no signature, PE checksum 0. Nothing in it can
  detect a changed byte. (Earlier notes said it was "Steam-wrapped". Wrong.)
* Not Windows: no AppCompatFlags\Layers or Compatibility Assistant entry for
  hl2.exe, Fault Tolerant Heap not tracking it, no Image File Execution Options,
  and no hl2.exe crash events after 2026-09-23 23:07 (the poisoned launches never
  crash, they show the dialog).
* Not the per-library shader cache (steamapps\shadercache\218): setting it aside
  changes nothing.
* Not the SDK's 2007-era steamclient.dll next to hl2.exe: a healthy install runs
  with it present. Earlier notes called it "a real bug"; unproven, and the
  launcher no longer moves it.
* Not Steam's launch process: replaying Steam's exact environment (96
  variables, captured from the live process) from a process we started fails the
  same way.

### Where it fails, as far as it can be seen from outside

The game's only debug output is the error line itself (captured with a DBWIN
listener). Without a steam_appid.txt, SteamAPI init fails before any Steam
client DLL is loaded; with one, init passes and the game then parks in
filesystem_steam's content handshake. Both are the Steam client declining to
serve app 218 to that install -- consistent with state Steam keeps for the app
and its install location, which the Steam client's own files do not visibly
record (no config.vdf / localconfig.vdf / userdata entry mentions the path or
the exe).

### How to restore, for anyone who hits it

1. Put hl2.exe back: Steam > Library > Source SDK Base 2007 > Properties >
   Installed Files > Verify integrity.
2. Still "SteamStartup() failed"? Same page > **Move install folder** to any
   other Steam library. Seconds, no download. (Done here by hand: robocopy
   /E /XJ to the other library's steamapps\common, move appmanifest_218.acf to
   that library's steamapps, restart Steam.)
3. GE:S itself lives in steamapps\sourcemods and is never affected. VR\config.txt
   and VR\weapons.txt travel with the install folder.

### Tools written for this (scratchpad/probe)

* `probe.exe dbwin <sec>` -- prints every OutputDebugString in the session with
  PID (Steam logs its launch steps there too).
* `probe.exe env|cwd <pid>` -- environment block / command line / working
  directory of a 32-bit process (x86 build, WOW64 PEB offsets).
* `try_env.ps1` -- launch hl2.exe with a saved environment, report PASS/FAIL.
* `mods32.ps1` -- 32-bit module list via the SysWOW64 PowerShell.

### Two hazards met on the way

* **The launcher creates a junction `Source SDK Base 2007\gesource` that points
  at the real GE:S folder.** A plain `robocopy /E` follows it (a 9 GB copy to an
  almost-full C: drive had to be killed), and a recursive delete of an old
  install tries to delete through it (it stopped on "access denied"; GE:S was
  verified intact, 22,336 files). Always use `/XJ`, and remove junctions with
  `rmdir` before deleting a tree.
* Steam will not start an install from `steam://install/218` when the app is
  unregistered; its dialog needs a click.

## EARLIER, DURING THE SAME FAILURE (investigation trail)

(Superseded by the section above. Kept for the evidence; where it says "reinstall is the only cure" or calls steamclient.dll a real bug, the section above is right.)

Since 2026-09-23 23:21 the game dies at startup with
`SteamStartup() failed: SteamAPI_Init_Internal failed`. Unresolved at the time
of writing; Steam has a 3.68 GB repair download queued and suspended.

What is RULED OUT, each by test, not by reasoning:

* **Not the mod.** With d3d9.dll, openvr_api.dll and the Bink proxy removed and
  hl2.exe.local renamed away, the failure is identical. Our code never loads:
  no new lines in vrmod_log.txt or %TEMP%\gesvr_boot.log since 23:08:46, and the
  failing process has 4 threads / ~30 MB.
* **Not GE:S.** Launching the stock `sourcetest` game fails the same way.
* **Not the LARGE_ADDRESS_AWARE patch.** hl2.exe is byte-identical to the
  pre-patch backup (hash-compared), and Steam's own file verification accepted
  it -- it replaced exactly one file, our binkw32.dll proxy.
* **Not stale process/system state.** Windows rebooted 23:37, Steam restarted
  several times, Steam's appcache cleared and rebuilt (23:39, 00:11).
* **Not Steam being unwell.** Logged on OK each time, same user, not elevated,
  client unchanged since 2026-09-03, registry ActiveProcess\SteamClientDll
  correct, app 218 owned by this account, StateFlags 4, no staging leftovers.
  SteamVR (app 250820) launches fine, so Steam's launch path works in general.
* **Not antivirus or a compatibility shim** (no Defender detections, no
  AppCompatFlags\Layers entry for hl2.exe).

What IS known:

* **Launched by Steam** (`-applaunch 218`): dies at SteamStartup having loaded
  steam.dll but NO steamclient.dll at all.
* **Launched by hand** with SteamAppId/steam_appid.txt: passes SteamStartup,
  loads Steam's real steamclient.dll, then parks at ~27 MB / 0.1 s CPU in the
  filesystem_steam content handshake. Adding SteamClientLaunch/SteamEnv to the
  environment does not change that.
  So both routes break at the game <-> Steam client content handshake for 218.
* **A real bug found on the way** (fixed, but not the cause): the SDK ships a
  2007-era steamclient.dll (8.8 MB) next to hl2.exe, and Windows prefers a DLL
  in the application directory over the path steam_api asks for, so it shadows
  the Steam client's current 21 MB one. Proved by listing the failing process's
  modules. Launch-GESVR.ps1 now renames it to steamclient.dll.stale on every
  run (Steam's verification restores it).
* Diagnostics that worked, worth reusing: list a 32-bit process's modules with
  the WOW64 PowerShell (scratchpad/mods32.ps1 via
  C:\Windows\SysWOW64\WindowsPowerShell\v1.0\powershell.exe), and read a dialog's
  windows with scratchpad/dlgtext.ps1 (DllImport must be CharSet.Unicode).

State left behind:

* appmanifest_218.acf DELETED to force Steam to re-register the install;
  backup at scratchpad/appmanifest_218.acf.bak. Steam did NOT reuse the 5.9 GB
  on disk -- it queued a 3.68 GB download, which was suspended after ~50 s.
  App 218 is "Update Required, Update Queued, Suspended": starting Steam
  resumes the download; restoring the manifest backup returns to the previous
  (broken) state with no download.
* Test artifacts cleaned up (steam_appid.txt removed); hl2_orig.exe and
  steamclient.dll.stale left in the game folder on purpose.

## NO 4 GB FOR THIS 32-BIT GAME, AND WHY THE GAME HUD IS STRETCHED (2026-09-23 night)

Matty: "im having a few crashes, I think im right near my texture limit" and
"the hud is still a little vertically stretched ... when i turn the radar on,
its more of an oval than a circle".

* **hl2.exe cannot be made large-address aware. DO NOT TRY IT AGAIN.** It is
  not LAA (PE Characteristics 0x0102, bit 0x20 clear), so Windows caps the whole
  process at 2 GB: game, textures and the copies DXVK keeps of every managed
  texture. That is the ceiling the High/Very High texture crashes hit. Setting
  the bit (0x0102 -> 0x0122) was approved and applied, and the game then would
  not start at all:
      Error!  SteamStartup() failed: SteamAPI_Init_Internal failed
  This hl2.exe is a 98 KB Steam-wrapped shim; changing any byte of it makes
  Steam refuse to initialise. Restoring hl2_orig.exe over it fixed it at once
  (verified back to 0x0102), and Launch-GESVR.ps1 now carries a comment where
  the patch was, so nobody re-adds it. Any future attempt at the 2 GB ceiling
  has to work without editing the exe -- the remaining levers are the ones that
  reduce demand: texture detail, window resolution, and what DXVK keeps
  (d3d9.evictManagedOnUnlock, which crashed when tried; see dxvk.conf).
* **The game's own HUD is stretched vertically by 1.844x**, and it is the same
  geometry the viewmodel squash already corrects for: the view is rendered with
  the headset's eye aspect (0.964) into the 16:9 window buffer, so a pixel is
  1.844 times taller than it is wide (the log line "eye aspect 0.964, pass
  aspect 1.778 -> squash 0.542" IS this number). The world is right because the
  per-eye submit crop undoes it; our overlays (watch, menus, toasts, hurt HUD)
  are right because they are their own quads. Anything GE:S draws in screen
  pixels -- radar, its health bars -- is stretched, and nothing in the current
  path can undo it for them alone.
  Ways out, in order of cost: run the window at the eye aspect (1392x1440
  instead of 2560x1440: no stretch, but horizontal sharpness nearly halves);
  a middle ground (1920x1440 leaves 1.38x); or render each eye into its own
  target at the headset's aspect, which removes the whole class of problem
  (squash, stretch, wasted vertical resolution) and is the known-broken
  EyeRenderTargets path. Matty chose to keep the sharpness for the trailer and
  have the render-target route attempted afterwards.

## THE BUILD OVERWRITES dist\VR -- EDIT L4D2VR\SteamVRActionManifest (2026-09-23 night)

**Read this before touching bindings or config.txt.** The post-build event in
l4d2vr.vcxproj (both Release and Debug) runs:

    xcopy /Y /E /I "$(ProjectDir)SteamVRActionManifest" "$(SolutionDir)dist\VR\SteamVRActionManifest"
    copy  /Y "$(ProjectDir)config.txt"        "$(SolutionDir)dist\VR\config.txt"
    copy  /Y "$(ProjectDir)manifest.vrmanifest" "$(SolutionDir)dist\VR\manifest.vrmanifest"

So `L4D2VR\SteamVRActionManifest\*`, `L4D2VR\config.txt` and
`L4D2VR\manifest.vrmanifest` are the sources; the copies under `dist\VR\` are
build output and any edit to them is destroyed by the next build.

That is what happened to the Reload binding twice. Both times it was edited in
`dist\VR\SteamVRActionManifest`, verified by reading it back, and then wiped by
the next rebuild -- which is why the handoff note said Reload was on the right
B while the file on disk (and in the game) still had it on the left Y, shared
with Pause. Matty: "The controller binding with the reload was gone again and
the steam vr bindings only show the left for dead vr mod one."

Now done in `L4D2VR\SteamVRActionManifest`, rebuilt so the build itself carries
it into dist, and both installed copies under Source SDK Base 2007 (`VR\` and
`bin\VR\`) refreshed. Reload is `/user/hand/right/input/b`, the right stick
click is free, and the configs are named "Default GoodHead bindings for ...".

Checks worth keeping:
* config.txt is edited in BOTH places already (that is why it survived), and
  the pair must stay in step -- the build only copies L4D2VR -> dist.
* After a binding change: rebuild, then read the file back from `dist\VR\`
  (not from the source), and confirm the timestamp is newer than the build.
* The mod registers `bin\VR\manifest.vrmanifest` with AddApplicationManifest
  (temporary) and IdentifyApplication each run, so `Steam\config\appconfig.json`
  never lists us -- that is expected, not a fault. Verified in the log:
  `Identified process as gesource.vr ... bin\VR\manifest.vrmanifest` and
  `SetActionManifestPath(... bin\VR\SteamVRActionManifest\action_manifest.json) -> 0 OK`.
* Nothing overrides the defaults: no gesource.vr entry in
  `Steam\config\steamvr.vrsettings` or in userdata's binding_config.json. The
  only stale trace is SteamVR's OpenXR remapping cache,
  `Steam\config\openxr\auto-remapping_*.json` (2026-08-30), which still holds
  the old binding for app key gesource.vr. Delete those two files if SteamVR
  ever seems to serve the old mapping; they regenerate.

## THE SETTINGS PANEL COULD BE BURIED BY THE GAME MENU (2026-09-23 night)

Matty: "I accidently brought the menu closer than the vr settings menu and now
i couldnt close it because i couldnt click the close button as the other menu
was in the way."

The log confirms it: `VRSettings: opened` at 22:22:58 with no `closed` after it,
and `UpdateActionState -> 0 OK` nearby, so the action system was alive and the
panel simply never got a close.

* **The panel is now always in front of the game menu.** It used to sit at a
  fixed 1.25 m while Menu distance goes down to 1.0, so the game menu covered
  it -- including the Close button, which the laser then could not reach.
  VRSettings::Place takes EffectiveMenuGeometry's distance, sits 0.25 m in
  front of it (floor 0.5 m), and scales its width by dist/1.25 so it keeps its
  apparent size. Frame re-places it when the menu distance changes, so it
  follows while you are standing at that very setting.
* **The game menu stops being laser-interactive while the panel is open**
  (MakeOverlaysInteractiveIfVisible false in Open; ProcessMenuInput asserts it
  true again the frame we close), so it cannot take a click meant for us.
* **B closes the panel, read from the device.** The action-system close
  (MenuBack/Pause) was already there and did not fire: while SteamVR's laser is
  driving an interactive overlay it takes that hand's input, so those actions
  go quiet exactly when you need them. VR::LegacyMenuButtonDown reads
  k_EButton_ApplicationMenu (B and Y on Touch, both B on Index) with
  GetControllerState, the same legacy path LegacyTriggerDown already uses, with
  an edge guard so a button held from opening does not close it immediately.
  If SteamVR ever stops answering legacy state this goes quiet too -- the
  placement fix above is what guarantees the Close button is reachable.
  Closes now log `VRSettings: closed by button (device=N)`.

## MENU SIZE AND DISTANCE NOW MEAN WHAT THEY SAY (2026-09-23 night)

Matty: "Trying to resize the menu it is a little funky, like its not doing
what i think it should with size and distant changes."

Three things made the two settings interfere, all in VR::EffectiveMenuGeometry:

* **Distance was also a size control.** An overlay's width is metres at its own
  position, so pushing the panel from 1.6 m to 3.0 m shrank it by nearly half.
  The width is now scaled by distM/kMenuRefDist (1.6 m), so the panel subtends
  the same angle wherever you put it -- distance changes depth only.
* **The pause menu forced distM >= 1.8 m**, so every step of the setting below
  1.8 did nothing at all while you were in a map, which is where you are most
  likely to be fiddling with it. Gone.
* **The character/level panel ignored the setting entirely** (fixed
  InGameMenuDistance = 2.4 m). It was only out there to make it smaller, which
  distance no longer does, so it takes the one distance and the in-map size
  factor. InGameMenuDistance is deleted from vr.h, vr.cpp and both configs.
* **In-map menus keep a 0.72 size factor** (pause and character/level both):
  full size covers too much of the view. Measured, at 1440p and the shipped
  2.0/1.6: main menu 79.6 deg before and after, pause 56.1 -> 61.9, character
  select 58.1 -> 61.9. Nothing moved more than six degrees, and the distance
  slider now holds all of them steady across 1.0-3.0 m.
* **The size setting reads as a percentage** (50-200%, 2.0 = 100%) instead of
  metres, because the metres depend on the distance and on the resolution
  (MenuScaleWithRes): the label could not have been honest. The config key
  stays MenuWidthMeters, documented as the width at 1.6 m.
* The pointer needs no matching change: both the SteamVR laser and the tip ray
  hit the overlay itself through ComputeOverlayIntersection, so aiming follows
  whatever geometry this function chooses. (The old comment above the function
  claimed the pointer repeated the maths and had to be kept in sync. It does
  not, and the claim is now corrected in place.)

## BINDINGS: THE RELOAD EDIT NEVER LANDED (2026-09-23 night)

Matty: "The reload isnt there but it might be because im on the left for dead
config in steam."

* **It was not SteamVR: the files still had the old binding.** The previous
  session's note in RELEASE POLISH (Reload on the right B, names no longer
  saying Left 4 Dead 2) describes an edit that never reached disk --
  `git diff` showed dist/VR/SteamVRActionManifest untouched, and all three
  files still had Reload on `/user/hand/left/input/y`, the same button as
  Pause, on Index a button that does not exist. Pressing Y fired Pause and
  Reload together, so all you saw was the pause menu. Done now, for real:
  Reload is `/user/hand/right/input/b` beside Use and MenuBack, and the right
  stick click is no longer Flashlight (GE:S has none). Verified by reloading
  each file and printing every source (scratchpad/bindings_reload_fix.py).
  Lesson: check `git diff` for the file you claim to have edited before
  writing the handoff note.
* **The configs are now called GoodHead** (Matty's name for it, confirmed):
  "Default GoodHead bindings for Oculus Touch / Index Controllers / Vive
  Cosmos Controllers", with a description line. That title is what SteamVR
  shows under Manage Controller Bindings -- it used to read "Default Left 4
  Dead 2 VR bindings", which is the "left for dead config" that was suspected.
  The SteamVR application entry (manifest.vrmanifest, app key `gesource.vr`)
  is still named "GoldenEye: Source VR".
* **Nothing overrides them**: Steam's userdata binding_config.json and
  config/steamvr.vrsettings have no entry for `gesource.vr`, so SteamVR is
  reading the default files straight from the manifest folder. They install
  on the next launcher run (the folder-copy fix from last night is in place),
  so the new binding takes effect the next time GE:S starts.

## OFF HAND, FIRST TEST BACK (2026-09-23 night)

Matty, after trying it: the position could not be dialled in because the hand
"rotated weirdly ingame when turning the vr controller", and the Display tab
ran off the bottom of the settings panel.

* **The tilt was Euler addition, not a rotation.** The first cut did
  lang.x/y/z += LeftHandAngle on the angles VectorAngles had just produced
  from the controller. Adding degrees to an Euler triple is not a rotation in
  the hand's frame: how much of the tilt lands on which axis depends on where
  the controller is pointing, so the hand turned about a moving point and the
  offset, applied in that skewed basis, swung it further off the more you
  turned. Now the tilt is composed as a matrix on the right
  (Concat(lraw, PoseMatrix(tilt))), the same way the melee hand has always
  done it, and LeftHandOffset is applied in the raw CONTROLLER basis. The two
  knobs are now independent: rotating turns the hand where it stands, moving
  slides it without turning it, and in play the hand is still rigid to the
  controller (it orbits with your wrist, because lraw turns with it).
* **The settings panel had run out of room**: rows were a fixed 104 px from
  y=196 with the footer at 830, so six fit exactly and the seventh (Display,
  once "Watch on model" was added) fell off the panel. VRSettings::RowPitch
  now shrinks the pitch to fit the tab (88 px at seven rows, floor 72), and
  the row internals -- label, hint, control, divider -- are derived from it,
  so they stay centred. Layout and DrawPanel share the one number, so the
  laser still hits what it looks like it hits.
  Checked offline with scratchpad/preview (panel_tab*.png): every tab's last
  row now ends above the footer, no hit-test mismatches, nothing off-panel.

## RELEASE POLISH (2026-09-23 night, after v0.3-sharper; untested in the headset)

* **Why a timer showed with the HUD off**: GE:S paints its HUD into the eye
  images. `VGui_Paint` (engine.dll) never resolves on this build ("Optional
  signature not found: 55 8B EC E8 ..."), so dVGui_Paint's "no VGUI in the
  stereo pass" guard is dead code, and the in-map menus now DEPEND on that
  painting (the overlay captures it). Do not "fix" the hook without re-testing
  character select and the pause menu. Instead `VR::SyncHudCvars` switches off,
  while the watch is on, what the watch shows: `cl_ge_show_timer 0`,
  `cl_ge_show_ammocount 0`, `cl_ge_hud_noswitchlist 1`, and with WatchKillFeed
  `cl_ge_drawkillfeed 0` (all GE:S client cvars, archived by GE:S).
* **Kill feed on the watch** (`vr_events.cpp`, `WatchKillFeed=true`, VR Settings
  > Display > Watch notices): an IGameEventListener2 on player_death,
  round_start, round_end. All engine calls by raw slot, prologue-checked
  (slots verified by disassembly of engine.dll):
    IGameEventManager2 "GAMEEVENTSMANAGER002": AddListener 3
      (56 57 8B 7C 24 10 85 FF 8B F1 74), FindListener 4 (8B 44 24 08 57 50 8B F9 E8);
    listener: dtor 0, FireGameEvent 1 (FireEventIntern calls [vt+4](event));
    IGameEvent (CGameEvent over KeyValues): GetName 1, GetInt 6, GetString 8;
    IVEngineClient: GetPlayerInfo 8 (8B 44 24 04 83 E8 01 3B 05; copies 0x84
      bytes, name first), GetPlayerForUserID 9 (8B 0D ?? ?? ?? ?? 85 C9 75).
  Registered from VR::Update in map (every 2 s until it takes; FindListener
  re-checks). Weapon names from GE:S's resource/gesource_english.txt (UTF-16)
  for the event's "#GE_..." print-name token. Notices: VRWatch::Notify(head,
  detail, kind 0 news / 1 good / 2 bad, ms, pop). While one shows, it takes
  the watch screen in place of weapon and ammo (preview renders in scratchpad
  preview/watch_note_*.png). Your kills and deaths pop the watch up.
  Log: `Kill feed: game event manager verified`, `Kill feed event ...`.
* **Bindings were never updating**: Launch-GESVR's `Copy-Item -Recurse` copied
  dist\VR\SteamVRActionManifest INTO the existing folder, so SteamVR kept
  reading the August files (no Scope action at all) while updates piled up in
  SteamVRActionManifest\SteamVRActionManifest. Fixed: contents are copied over,
  the stray copy removed.
* **Binding fixes**: Reload shared the left Y with Pause (Touch, Cosmos) since
  959c505 moved it off the grip, and pointed at a non-existent left Y on Index.
  Reload is now the right B on all three, alongside Use (Matty asked for the
  door button); Y / Index left B is Pause only. The right stick click, which
  was Flashlight (GE:S has none), is left unbound. Binding names no longer
  say "Left 4 Dead 2".
* **Defaults for new installs** (dist config): TrackedWeapon=true (free aim),
  MenuWidthMeters=2.0 (Matty's; the menu scales with resolution).
* **Logs rotate**: vrmod_log.txt and %TEMP%\gesvr_boot.log move to *.old when
  over 8 MB at session start.
* **README.md rewritten** for players: features, install, controls, settings,
  known limits (texture detail High, 1440p cap, GE:S settings the mod changes).
* World scale moved to the Comfort tab to make room for Watch notices.
* `tools/Make-Release.ps1` builds packages/GESVR-<git describe --dirty>.zip:
  launcher, README, dist, licences (DXVK, OpenVR, MinHook, Source SDK).

## PICTURE QUALITY INVESTIGATION (2026-09-22, after v0.2-free-aim)

Where the softness comes from, measured:
* The headset asks for 2688x2880 per eye (log: `recommended RT 2688x2880`).
  On the working backbuffer path each eye is rendered at the WINDOW size,
  1920x1080, then stretched: 1.4x across, **2.7x up and down**. That is most
  of the blur. The window cannot exceed the desktop (1080p here) or the game
  does not boot (table further down).
* GE:S's own settings (HKCU\Software\Valve\Source\gesource\Settings):
  `mat_forceaniso 1` (no anisotropic), `mat_trilinear 0` (bilinear),
  `mat_antialias 0`, `mat_picmip 0` (High, fine), mat_hdr_level 2,
  MotionBlur 0, GPU 10DE:1E04 (RTX 2080 Ti). DXLevel_V1 reads 0; HDR and
  parallax mapping on means the DX9 path is live.
* In-map frames arrive ~13.9 ms apart = 72 Hz (a Quest's default): the game is
  keeping up with the headset.
* Multicore rendering is greyed in GE:S's options because the launcher passes
  `+mat_queue_mode 0`. Keep it: the eye capture and Present work assume the
  single render thread.

Done in this pass (all switchable, v0.2-free-aim is the fallback):
* **Texture filtering** `TextureFiltering=16` (VR Settings > Graphics):
  `mat_forceaniso 16` + `mat_trilinear 1` at map load and on change
  (`VR::ApplyGraphicsCvars`). Sampler state only, no texture reload. 0 leaves
  the game's setting. Texture DETAIL is not touched (picmip -1 hung the driver).
* **Bloom** `Bloom=true` (Graphics tab) -> `mat_disable_bloom`.
* **Launcher window = largest 16:9 that fits the primary desktop**, capped at
  3840x2160, measured DPI-aware (GetSystemMetrics after SetProcessDPIAware).
  1920x1080 on this desktop, exactly as before; `$gesResolution = "1920x1080"`
  pins it. This is what lets NVIDIA DSR raise eye resolution with no code
  risk: a 3840x2160 DSR desktop gives 3840x2160 eyes (0.75x the headset's
  height instead of 0.375x) at about 4x the pixel cost; 2880x1620 is 2.25x.

RESULT (2026-09-23): Matty's monitor is 4K and had been set to 1080p. With the
desktop at its native resolution the launcher's auto size took over: the
00:01 session ran at 2560x1440 (the desktop was 2560x1440 at launch), "looks
great", and held the headset's 72 Hz (13.6 ms median frame gap). The desktop
is now 3840x2160. The 3840x2160 run (00:13) CRASHED at character select
~25 s in -- vrmod_log just stops, the DXVK log is clean, no minidump -- and
its menus were huge (MenuScaleWithRes widens the menu by screen height / 1080:
2x at 4K). The launcher's auto size is now capped at 2560x1440. If 4K is ever
retried, suspect memory in this 32-bit process first (every framebuffer-sized
target, ours and the engine's, is 2.25x bigger than at 1440p). Note Windows also lists a
"Meta Virtual Monitor" (Quest Link) at 2560x1440; the launcher reads the
PRIMARY display (GetSystemMetrics, DPI-aware), which was the right one.

MEMORY (2026-09-23): hl2.exe is 32-bit and NOT large-address-aware (PE
characteristics 0x0102) -> 2 GB of address space for the whole game. DXVK
v1.10.1 (this fork) keeps a mapped staging copy of every MANAGED texture for
the session (D3D9DeviceEx::UnlockImage only flushes and tosses it when
d3d9.evictManagedOnUnlock is set). That fits both the Very High (picmip -1)
hang inside the driver during texture upload and the silent 4K crash.
Try 1: `dist\dxvk.conf` with `d3d9.evictManagedOnUnlock = True`, installed
once to the SDK folder (hl2.exe's working dir; DXVK logs "Found config file:
dxvk.conf" in hl2_d3d9.log). Cost: a texture the game re-locks is read back
from the GPU. Try 2, not done, needs Matty's OK: set LARGE_ADDRESS_AWARE on
hl2.exe (4 GB; Steam verify undoes it). DXVK 2.x has proper 32-bit memory
management but the VR interface would need porting.

TRY 1 RESULT: FAILED, now off (the line is commented out in both dxvk.conf
copies). With it on, applying Very High crashed, loading a map at Very High
crashed, and even applying High crashed -- Medium was fine. Every crash is the
same: Application Error, client.dll+0x28AC17, 0xC0000005 = GE:S's "Deleteing
panel: %s" loop reading a NULL VGUI interface (global 0x10504E10) while the
client is torn down, right after materialsystem prints "Reference Count for
Material ... != 0" for every material (CMaterial's destructor, i.e. the
material system shutting down). So the game begins an orderly shutdown ~5 s
into a texture reload and then crashes on the way out. Our watchdog is NOT it
(it uses TerminateProcess: no destructors, no such messages, and it logs
"forcing exit"). The reason for the shutdown was never printed, not even with
the new spew logging (vr_settings.cpp LogGameSpew: game warnings/errors now go
to vrmod_log as `GAME warning/ERROR:`). Unexplained: GE:S's options showed Very
High while the registry said mat_picmip 0. The 4K crash is different:
ucrtbase 0xC0000409 = a fail-fast abort, which is how DXVK dies on a failed
memory allocation. Texture detail stays High, and should not be changed while
the game is running until this is understood.

Not done yet, the real fix: `EyeRenderTargets=true` renders each eye into its
own target at the headset's size, free of the desktop. Its open bug (the
SECOND RenderView of a frame draws at the backbuffer's 1920x1080 viewport) is
described under PREVIOUS below; the mod's Push/PopRenderTargetAndViewport
hooks look innocent (their HUD redirect needs L4D2's IsSplitScreen /
PrePushRenderTarget sequence, which GE:S never produces). Next step there is
measurement per pass (render target dimensions and viewport), in the headset.

## v0.2-free-aim: TUNED POSITIONS SHIPPED, NUMPAD TUNING OFF BY DEFAULT (2026-09-22)

* Matty's numpad-tuned positions (his VR/weapons.txt) are now weapons.cpp's
  built-in table: tknife, knife, slappers, the three mines by name, grenade,
  zmg. First substring match wins, so `tknife` sits above `knife` and the
  named mines above `mine`. A player's own weapons.txt still overrides.
* `WeaponTuning=false` by default: ProcessTuneKeys still counts presses (so
  turning it on does not replay them) but ignores them. New settings tab
  **Weapons**: "Swing to attack" (SwingMelee) and "Adjust position"
  (WeaponTuning).
* This is the second line in the sand (tag `v0.2-free-aim`): free aim with
  barrel-true shots and dot, effects from the hand, face aim in true shape,
  classic reticle, Game HUD option, round time on the watch.

## FACE-AIM RETICLE DEPTH; WATCH SHOWS ROUND TIME (2026-09-22 late)

* **Face-aim reticle on surfaces.** It was drawn at the centre of each eye:
  identical in both, i.e. at infinity, so it read as passing through anything
  nearer. UpdateGunAim now runs in face aim too: trace from m_SetupOrigin (the
  game's eye, where shots leave) along the eye angles, and draw the dot at the
  hit point's projection in each eye, like free aim. No view-angle override
  and no "no gun" hiding in face aim.
* **Watch time = round time when there are rounds.** It took the first
  CGEGameTimer entity, which is the match timer (CGEMPRules creates
  m_hMatchTimer then m_hRoundTimer back to back, so the match timer has the
  lower index). Now both are found and it follows GE:S's CGEHudRoundTimer:
  round remaining while the round timer is started, else match remaining if
  round time is enabled, else none. Reads m_bStarted too. Log:
  `Game timers: match at N, round at M` and every 10 s `Game timers: showing ...`.

## FEEDBACK ROUND: FACE-AIM GUN, HUD, WEAPON SWITCH, DEATH CURTAIN (2026-09-22)

* **Face-aim gun moved out to the lower right** (`FaceAimBones`, replaces
  SquashedViewmodelBones). At the eye FOV the head-locked gun sat in front of
  your face, while GE:S's FormatViewModelAttachment still put flash/tracers
  where a narrow viewmodel FOV would: it scales an attachment's offset across
  the view by f = tan(fov/2)/tan(fovViewmodel/2) of the GAME's view (captured
  from RenderView's setup: g_gameFov/g_gameFovVM). The gun now moves rigidly
  by (f-1) x its muzzle's across/up offset (hand bone for arm rigs), eased 5%
  per frame so recoil is not amplified, times `FaceAimGunSpread` (1 default).
  In face aim the attachment hook applies the same shift instead of the FOV
  correction (`FaceAimMoveAttachment`), so the flash stays where it was and
  shells leave the moved gun. Log: `FACE AIM: game fov .. viewmodel fov ..`.
* **Reticle while zoomed** even with VRReticle off: dxvk `g_GESVR_ReticleForce`
  = scope held and the engine FOV below the learned base FOV.
* **Classic reticle** radius 6x the dot size (min 6 px); 16x was far too big.
* **Settings**: "Gun in hand (test)" is now "Aim mode": Face aim / Free aim
  (still TrackedWeapon=false/true). New Display row "Game HUD": Off / When hurt
  / Always (`GameHUD=off|hurt|always`; default from HudAlwaysVisible). The
  hurt pop-up only runs in When hurt and never at 0 health; the Show HUD button
  works in every mode.
* **Weapon switch**: `WeaponFastSwitch=true` sets `hud_fastswitch 1` (GE:S's
  default 0 highlights in a 2D list and switches on fire -- invisible in VR).
  The watch pops up for 2.5 s when the held weapon changes.
* **Death**: the red curtain is the viewmodel of GE:S's `gebloodscreen` entity,
  `models/VGUI/bloodanimation.mdl` (20 bones: a root and 17 drip columns; an
  80 x 62.5 u sheet ~50 u ahead). It hung off the game camera -- which rides
  the ragdoll's head on death -- and at the eye FOV covered only the middle.
  `BloodCurtainBones` places it on each eye, stretches it `BloodCurtainScale`
  (2.6) across and up, and pre-squashes it. The HUD's HudBloodScreen is only a
  fade to black + respawn text. `DeathCamFirstPerson=false` sets
  `ge_fp_ragdoll 0` (standard death camera). Both cvars are archived by GE:S.

## FACE AIM: HEAD-LOCKED GUN IN TRUE SHAPE (2026-09-22)

The per-eye pre-squash (`EyeSquash`, the fix for the tracked gun's stretch:
GE:S's DrawViewModels uses the window's 16:9 aspect, the eyes 0.964) now also
applies to first-person models left at the head when the gun is not in hand
(`SquashedViewmodelBones`). Squash only, no move. `FixViewmodelAspect=true`
(default) turns it on; g_squash is computed when either mode needs it. The
squash is identical for both eyes (shared up axis), and it corrects shape
whatever fovViewmodel is. Also fixed a leftover VT byte (0x0B: an old heredoc turned
the `\v` of `"\\v_"` into one) in the VIEWMODEL-seen log filter.

## TRACKED WEAPON, STEP 4: EFFECTS FROM THE HAND; KNIFE THROW GESTURE (2026-09-22)

* **Muzzle flash, its light, muzzle smoke, ejected shells, tracer start** all
  come from the viewmodel's attachments, and the viewmodel entity is still at
  the head (only its drawing is moved). C_BaseAnimating's attachment setup
  passes every attachment through `C_BaseViewModel::FormatViewModelAttachment`
  (client.dll vtable[183], RVA 0x14DF20; found as the only C_BaseViewModel
  override of an empty `ret 8` C_BaseAnimating virtual; code matches the SDK:
  MatrixGetColumn / ::FormatViewModelAttachment / MatrixSetColumn). Hooked by
  signature (`Offsets::FormatViewModelAttachment`). With TrackedWeapon on it
  replaces the FOV correction with the drawn bones' rigid move:
  hand * inverse(viewmodel pose). The hand is noted per viewmodel entity by
  TrackedWeaponBones (`NoteTrackedHand`, entity = info.pRenderable - 4). The
  viewmodel pose is read at that moment from IClientRenderable (entity+4)
  slots 1/2, GetRenderOrigin/GetRenderAngles, prefix-checked per vtable
  (`83 B9 A4 04 00 00 00 .. .. 80 79 50 17`), because the first shot swings
  the view angles to the barrel that same frame.
* GE:S tracers: `CGEWeapon::MakeTracer` -> `UTIL_ParticleTracer("tracer_standard",
  ..., weapon entindex, GetTracerAttachment())`; the client's GetTracerOrigin
  swaps a local player's weapon for its viewmodel and asks its attachment, so
  tracers follow the same hook.
* Particle render pass: parsed ge_muzzle_fx.pcf (binary DMX v2: short string
  count and short indices; scratchpad `pcf_vm.py`). No muzzle or tracer system
  sets "view model effect" except `muzzle_sniper_rifle` (an empty parent;
  its flash child is 0), so they draw in the world pass at the true position
  and need none of the gun's aspect squash.
* `Weapon_ShootPosition` (server and client) signatures do NOT match GE:S, so
  bullets and thrown knives leave from the eye; the aim code relies on that.
* **Throwing knife flick**: `/v_tknife.` joins the swing weapons. On a swing
  the OpenVR velocity (-z, -x, y, turned by m_RotationOffset) becomes
  `m_ThrowDir`; UpdateGunAim aims along it and holds the view angles on it
  for 700 ms (`m_ThrowAimUntil`) -- GE:S throws after its fire delay along
  the eye angles of that moment (CWeaponKnifeThrowing::ItemPreFrame ->
  ThrowKnife). `m_AttackAimUntil` now only ever extends. Log: `Throw ... dir=`.
Log: `C_BaseViewModel::FormatViewModelAttachment hooked`,
`Viewmodel GetRenderOrigin/GetRenderAngles verified`,
`TRACKED GUN: attachment N (...) -> (...)`.

## STEP 2, ROUND 3: BARREL AXIS, GE:S CROSSHAIR, CLASSIC RETICLE (2026-09-21 late)

Report (23:30 run): movement fixed, but the dot "goes wonky and disappears
until I shoot, then it lines up with the gun"; scoped weapons show a second
reticle that follows the head.

* **The barrel is not the model's +X.** Every GE:S v_*.mdl is built lying
  along -Y: the `muzzle` attachment on muzzle.bone1 points (0,-1,0) in the
  bind pose, and the sequences turn base.bone1 so it faces forward. So the
  barrel's direction depends on the animation pose (the end of the draw,
  idle sway, the fire anim's held last frame), while the dot went along the
  hand's +X. The dot now uses the live `muzzle` attachment: origin and X axis
  (studiohdr numlocalattachments +240 / index +244, mstudioattachment_t is 92
  bytes: name, flags, localbone, matrix3x4 relative to the bone), taken from
  the drawn bones once per frame (`g_stereoFrame`) and eased 25% per frame
  so a firing kick nudges it. `GESVR_MuzzleWorld(out, dir)`. Log:
  `TRACKED GUN: <model> muzzle bone N, attachment found axis=(...)`.
  scratchpad `mdlatt.py` prints any model's attachments.
* **Never read an attachment-only bone from the draw's bone array.** Draws set
  up only BONE_USED_BY_VERTEX bones; muzzle.bone1 is 0x200 (attachment only),
  so its matrix there is stale -- last computed when the muzzle flash asked for
  the attachment, i.e. the last shot. The 00:08 log showed it: start points
  2000 u away, the dot turning opposite to the head, snapping back on each
  shot. (That was also the original "wonky until I shoot".) The muzzle now
  rides on the nearest vertex-used ancestor (`MuzzleAnchor`: base.bone1 on
  every gun) with its bind offset, poseToBone(anchor) * inv(poseToBone(muzzle)).
  Log: `muzzle bone 1 on bone 0`.
* Throwing knife (`tknife` key) borrows the `knife` tuning until it has its own.
* **Diagnostics** in UpdateGunAim: `Gun aim CHANGE: ... why=L/R` on every
  change in whether each eye's dot shows (0 shown, 1 no gun drawn in 150 ms,
  2 hit behind the eye, 3 outside the view), plus a sample every 2 s:
  trace fraction, distance, start, direction, gun and head angles, per-eye
  NDC, fov, attack. If the dot still misbehaves, read these first.
* **The second reticle is GE:S's own crosshair**: CGEViewEffects::DrawCrosshair
  (ges-code game/client/ges/ge_vieweffects.cpp) draws `sprites/crosshair` as a
  3D sprite 1200 u down CurrentViewForward, only in aim mode (the aim button,
  our left grip). HudCrosshair is disabled in GE:S's HudLayout.res; that
  sprite is the only crosshair. In VR it is head-locked. VR::UpdateGameCrosshair
  sets MATERIAL_VAR_NO_DRAW on it while TrackedWeapon is on (1 Hz check,
  restores it when off). **IMaterialSystem::FindMaterial is vtable slot 70 in
  this engine** (sdk/material.h's L4D2 order puts it elsewhere), verified by
  disassembly (the function printing `material "%s" not found.`, ret 0x10) and
  by prologue `55 8B EC 83 EC 24 8B 45 08 53 56 8B D9 57 89 5D` at runtime.
  IMaterial slots 0 GetName, 29 SetMaterialVarFlag, 30 GetMaterialVarFlag,
  42 IsErrorMaterial match sdk.h (checked against CMaterial and
  CMaterial_QueueFriendly). sdk.h declared GetMaterialVarFlag with no argument;
  the real one takes the flag (ret 4) -- fixed. Log:
  `IMaterialSystem::FindMaterial verified at vtable[70]`,
  `GE:S crosshair (sprites/crosshair) hidden`.
* **Viewmodel FOV = the eye FOV with the gun in hand**, zoom included. It was
  the unzoomed FOV, so when the scope zoomed the gun was drawn at the wrong
  scale and position, off its own dot.
* **Classic reticle** (`VRReticleStyle=classic`, style 4, last in the Reticle
  style list): GE's crosshair from the decoded sprite -- a ring with four
  spikes tapering in to a clear centre, poking just past the ring. Radius 16x
  the dot's size setting (minimum 10 px, at most a quarter of the image
  height), in the chosen colour.

## TRACKED WEAPON, STEP 2: SHOTS DOWN THE BARREL + AIM DOT (2026-09-21)

`AimWithGun=true` (with TrackedWeapon). `VR::UpdateGunAim`, called from
dRenderView right after ApplyHeadAndIpd:

* Start = the muzzle bone (every GE:S gun has `muzzle.bone1`, the Moonraker
  `muzzle.bone01`; melee/throwables have none), noted in model space by
  TrackedWeaponBones and re-placed with the current hand pose
  (`GESVR_MuzzleWorld`). Direction = the gun frame's forward
  (GetRecommendedViewmodelAbsAngle), so numpad rotation tuning moves the aim
  with the visible barrel.
* Trace 8192 u with MASK_SHOT, skipping only the local player. **Source 2007
  TraceRay is IEngineTrace vtable slot 4** (sdk/trace.h says 5) and its Ray_t
  has NO m_pWorldAxisTransform (slot 5 reads m_IsSwept at +0x41). sdk/trace.h's
  Ray_t and CTraceFilterSkipNPCsAndPlayers (calls L4D2 C_BasePlayer slots) are
  L4D2 shapes -- do not use them. Ours: TraceRay2007 (static_assert isRay at
  0x40), SkipOneEntityFilter, prologue-verified function pointer
  (`55 8B EC 83 E4 F0 B8 D4 10 00 00`). Result read from a 256-byte buffer
  (fraction at +44).
* View angles = eye (m_SetupOrigin, where shots leave) -> hit point, via
  SetViewAngles after ApplyHeadAndIpd's head-aim call -- **only while
  attacking** (m_AttackAimUntil, 150 ms past release). Doing it every frame
  (first build) made the stick walk along the gun: Source moves along the
  view angles. On the first press ProcessInput holds +attack back one frame
  until m_AttackAimApplied says the barrel angles are in: a command queued in
  Present runs in the next frame's usercmd with the angles the last
  RenderView set, which is why the first shot used to go where you looked.
* Aim dot: dxvk `g_GESVR_ReticleUseAim` + per-eye U/V, projected with each
  eye's own fov/aspect (so it is right when scoped); FillEyeFromSurface draws
  the reticle there instead of the centre, hidden for melee/throwables.
Log: `IEngineTrace::TraceRay verified at vtable[4]`, `Gun aim: muzzle=1 ...`.

## NUMPAD WEAPON TUNING -> VR/weapons.txt (2026-09-21)

With Gun in hand on and in play, the numpad (Num Lock on) moves the HELD
weapon live: Move mode 8/2 fwd/back, 4/6 left/right, 9/3 up/down; 5 toggles
Rotate mode (same keys = pitch/yaw/roll); +/- step (0.1..2 u, 0.5..10 deg);
0 saves; . resets the held weapon. A head-locked toast (vr_toast.cpp, same
canvas/flip-overlay plumbing as the watch) shows mode, step and values.

* Keys are polled with GetAsyncKeyState on the MenuInput thread (USER32 stays
  off the render thread) into `MenuInput::g_tunePress[13]` counters;
  `VR::ProcessTuneKeys` applies them in the in-map branch of Update.
* Storage: Weapons::SetOverride/ClearOverride keyed by `Weapons::Key(model)`
  ("autosg", "slappers"...); GetOffset returns an override before the table.
  Saved as `key = fwd right up pitch yaw roll` in bin\VR\weapons.txt, loaded
  ONCE at startup (not on config reload, so unsaved tuning survives settings
  changes). The launcher never overwrites weapons.txt, like config.txt.
* Arm rigs now anchor at GetRecommendedViewmodelAbsPos() (controller minus the
  weapon's offset in the gun frame) so the same keys move the floating hand.
* Note: "autosg" matches no table key, so the auto shotgun used the generic
  default until tuned.

## FLOATING HAND FOR ARM RIGS; KNIFE SWING (2026-09-21)

Tested: swing-to-slap works (2.0-2.4 m/s chops logged). Alignment did not: the
flat-screen arm, pinned by its hand, ran back into the chest/face, and the knife
(still origin-placed) sat close to the body.

* Any first-person model with `R_FK_Hand_jnt` is an arm rig (slappers, knife,
  throwing knife; cached per model in ArmRigFor). All are hand-anchored.
* `MeleeHideArm` (default true): R_FK_Collar/Arm_null/Shoulder/Elbow are
  folded to a zero-scale matrix at the hand point after the move, leaving a
  floating hand + cuff. Checked against the VVD weights (scratchpad
  vvd_weights.py): no vertex mixes hand and arm bones in any of the three,
  81 vertices are arm-only, the forearm mostly rides wrist/sleeve bones.
* `MeleeAngleOffset` pitch,yaw,roll: extra hand tilt about the controller.
* Swing to attack now covers the hunting knife (`/v_knife.`), not v_tknife.
* weapons.cpp knife/throwing entries zeroed (hand anchoring replaces them).

## TRACKED GUN: SLAPPERS BY THE HAND, SWING TO SLAP (2026-09-21)

Tested after the squash: guns look right in the hand. The slappers did not --
v_slappers.mdl is a full arm rig (Root, Collar, Shoulder, Elbow,
**R_FK_Hand_jnt = bone 5**, fingers), so placing it by its origin put the
shoulder on the controller, and its weapons.cpp entry added odd rotations.

* Slappers are now anchored by the hand bone: in TrackedWeaponBones the live
  hand-bone position in model space (inverse(viewmodel) * bone) is averaged
  slowly (0.02 per call) and the model origin set so that point lands on
  GetRightControllerAbsPos(). The slow average keeps the slap animation
  visible as a swing. Bone found by name (FindStudioBone: boneindex +160,
  216-byte mstudiobone_t). weapons.cpp slapper/fist entries zeroed.
* Swing to slap (SwingMelee, SwingSpeed m/s, default 2.0): with the tracked
  gun on and a slapper model active, right-controller vVelocity above
  SwingSpeed holds +attack for 150 ms, 450 ms cooldown. Log: `Swing N m/s -> slap`.
* Bone lists for any GE:S model: scratchpad mdl_bones.py. The knife uses the
  same arm rig plus a `knife` bone (1), if it needs the same treatment.

## TRACKED GUN STRETCH = THE VIEWMODEL PASS ASPECT (2026-09-21)

First test of step 1: "very close to aligned" but the gun skewed/stretched as
it moved and rotated. The bone transform is rigid (verified), so the
distortion came later: GE:S's CViewRender::DrawViewModels (ges-code
viewrender.cpp) sets `viewModelSetup.m_flAspectRatio =
engine->GetScreenAspectRatio()` -- the window's 1.78 -- while the eyes use the
eye frustum's 0.964 (log: "Eye frusta: superset fov=108.00 aspect=0.964").
Same horizontal FOV, so the first-person pass is ~1.84x too tall and
displaced vertically. This is also the "high gun" noted in vr.h.

**Do not swap GetScreenAspectRatio.** The first fix returned the eye aspect
from it during the stereo pass and froze the game entering a map (22:28 run:
main thread stuck in D3D9Initializer::Flush -> DxvkSubmissionQueue::submit):
other client code sizes screen-effect render targets from that call and
rebuilt them every frame.

Fix now: nothing is hooked. The tracked gun's bones get one more transform,
a squash along the CURRENT eye's up axis centred on that eye,
`D = I + (s-1)uu^T` (+ translation), `s = eyeAspect / passAspect` (0.964/1.78 =
0.542). The pass's window-aspect projection stretches it back: tested offline
(scratchpad squash_test.cpp) to 1e-6 NDC across orientations and points.
passAspect comes from CALLING IVEngineClient::GetScreenAspectRatio -- **slot
88 in this engine.dll**, not ges-code's 95 (slot 95 here takes an argument);
E9 jmp to `sub esp,0Ch; mov eax,[imm]; movss xmm0,[eax+2Ch]...divss`, bytes
verified; window size is the fallback. The eye origin/angles are set around
each eye's RenderView in dRenderView. Log: `GetScreenAspectRatio verified at
vtable[88] (read only, not hooked)`, `TRACKED GUN: eye aspect .. pass aspect
.. -> squash ..`. Head-aim mode is unchanged; the same squash would fix its
"high gun" if wanted.

## TRACKED WEAPON, STEP 1: GUN DRAWN AT THE HAND (2026-09-21, after v0.1-playable)

`TrackedWeapon=true` (VR Settings > Aiming > "Gun in hand (test)"). Model only:
aim is still the head until step 2.

How: the client calls DrawModelSetup then DrawModelExecute through the
IVModelRender vtable, and DrawModelExecute's last argument is the bone-to-world
array DrawModelSetup built (world space, around the viewmodel origin at the
eye). `dDrawModelExecute` copies it, multiplies every bone by
`hand * inverse(viewmodel)` -- hand = GetRecommendedViewmodelAbsPos/Angle, the
controller pose with grip tilt and the per-weapon offset from weapons.cpp;
viewmodel = info.origin/angles as the engine passed them -- and draws with the
copy. Bone count from studiohdr (DrawModelState_t's first member; "IDST",
version 44..49, numbones at +156), read under SEH. The weapon entity, its
attachments and effects are untouched, so muzzle flash etc. still come from
the head-locked position (step 4).

Why this can work where HANDOFF's earlier attempts could not: writing
info.origin happens after the bones exist (no effect), and patching
GetRenderOrigin/Angles moved only part of the transform (skew). One rigid
transform applied to every bone keeps the model solid. The maths was checked
offline (scratchpad bone_test.cpp): moved bone == hand * local to 1e-5.

Log: `TRACKED GUN <model> bones=N viewmodel=(..) -> hand=(..)` (first 6).
Per-weapon grip offsets (weapons.cpp) are used automatically in tracked mode;
they were written for this and never tested -- step 3 tunes them.

## HEIGHT, SCOPE, QUIT-FROM-MAIN-MENU (2026-09-21 21:11 run)

* **World scale did nothing; standing felt short.** halfIpd was clamped to
  >= 1.8 u, so it was 1.8 every frame whatever VRScale said: ~3.6 u for a
  6.4 cm IPD = 56 u/m, so 64 u of eye height felt like ~1.15 m. Clamp is now a
  sanity range (0.5..4). Perceived standing eye height = 64 / VRScale m
  (40 -> 1.6 m). The player's bin\VR\config.txt had been left at VRScale=34
  while it was inert; reset to 40.
* **Scope never fired**: no `Scope held` line in any session -- the new Scope
  action is unbound in the player's cached SteamVR bindings. ScopeHeld() also
  accepts TwoHand (already on the left grip in the old bindings) when motion
  controls are off. `Scope action bound|NOT bound` is logged once.
* **Quit from the MAIN menu hung** in steamclient like the pause-menu one
  (inMap=0), and the watchdog only armed on pause-menu clicks. Now any menu
  click arms it; the steamclient stack check still protects map loads.
* **Recenter** removed from the panel: same function as the left stick click,
  and it only resets position, so from a menu it showed nothing.
* **Weapon names** are an exact table of every GE:S v_*.mdl (autosg, gl,
  tknife, tazerboy, tokens...), not substrings.

## MENU POINTER: CONTROLLER ONLY (2026-09-21)

The 17:49 run played in VR (VGUI signal works: vgui=0 in play, 1 in menus).
Reported: a second, head-tracked cursor on menus that had to be lined up with
the SteamVR laser to click. Three bugs made it:

1. `MenuAimSource=auto` parsed as HEAD: the parser only knew "controller"
   (`find("controller") ? 1 : 0`), so every AIMSRC line ever said `head`.
2. On every frame without a laser MouseMove -- i.e. whenever the laser held
   still -- the head ray took over the aim, moving the game cursor (where
   clicks land) to where you were looking.
3. Our marker was drawn at that head point on exactly those frames.

Now: `MenuAimSource` is gone; aim = SteamVR laser point while it is on the
panel (kept while still; ended by VREvent_FocusLeave, or 1 s silence with our
ray off the panel), else OUR ray from the pointing controller's **tip**
component (`GetPointerPose`: IVRRenderModels::GetComponentState "tip", i.e.
where SteamVR's laser starts, not the raw pose). Marker drawn only when the
laser is not on the panel, at that same aim. Clicks: one path, one 350 ms
cooldown (the laser ButtonDown and the trigger action used to both click),
and only with a valid aim. Laser state resets after any gap in menu frames.
The VR settings panel uses the same rules (its head fallback is gone).
Log: `AIMSRC laser|controller-ray|none`, `Menu click (laser|trigger)`,
`Pointer: device N model '...' tip found`.

Weapon name: `RefreshActiveWeapon` (10 Hz in map) reads the held weapon's
`m_iViewModelIndex` -> IModelInfo::GetModel -> GetModelName. The shotgun had
read "SLAPPERS" because GE:S draws the slapper hands after the gun and the old
source was "last v_ model drawn". That guess is now only a fallback until the
netvar path works. Log: `Active weapon models/... (viewmodel index N)`.

## IN-MAP MENU SIGNAL IS NOW VGUI, NOT THE WIN32 CURSOR (2026-09-21)

The 17:39 run (full VGUI guard back) was STILL locked in menu mode. The new
CURSOR line settled why: in map the Win32 cursor was visible on 118/118 polls
a second, never centred -- the Win32 cursor is simply not a usable signal in
this setup, guard or no guard. (Only the trigger worked because menu mode
posts it to the game as a mouse click; ProcessInput never runs there.)

`ComputeMenuMode` now asks VGUI: ISurface::IsCursorVisible, i.e.
`_currentCursor != dc_none`, which CalculateMouseVisible sets every frame
from "is any visible popup taking mouse input". Found by RTTI
(`.?AVCMatSystemSurface@@` -> COL -> vtable) and disassembly of GE:S's own
vguimatsurface.dll: **slot 51** = `33 C0 83 B9 90 02 00 00 01 0F 95 C0 C3`,
slot 50 = SetCursor writing the same field. The SDK header's layout would put
it at 52 -- wrong. The bytes are verified before the first call; if they do
not match, it falls back to the Win32 cursor and logs so. Log lines:
`VGUI IsCursorVisible verified at vtable slot 51`, and `CURSOR vgui=N | win32 ...`.
If vgui=1 during normal play, some GE:S popup really is taking the mouse and
that is the next thing to find.

Head-tap HUD removed entirely at the user's request (DetectHeadTap,
UpdateHudElements, the Foes/Ammo overlays, HudTap*/HudFoes*/HudAmmo* keys).
The wrist watch replaces it.

## STUCK IN MENU MODE (2026-09-21 17:31 run) -- VGUI guard restored

Whole map in menu mode: no GAME frame, no Menu mode 1 -> 0 line, only the 2D panel.
19th (Grok full VGUI guard): in-map stable. 17:15 (guard narrowed to menus): flicker.
17:31 (narrowed + 400 ms cursor hysteresis): locked on. Common factor is the narrowed
guard, so dVGui_Paint is back to `if (g_inStereoPass) return;`. Hysteresis kept but off
is now 250 ms. New `CURSOR polls= visible= nullImage= centred=` line every ~90 frames in
map shows the raw cursor signal behind menu mode -- read it before touching this again.

## FIRST TEST OF SETTINGS + WATCH (2026-09-21 17:15 run)

What worked: VR Settings opened from the GE:S menu (spew hook fired), saves
landed in config.txt, netvars resolved for the first time ever --
`CGEMPPlayer: health=136 armor=5328 maxHealth=5336 maxArmor=5332
activeWeapon=3344 ammo=3024`, weapon `clip1=2036 primaryAmmoType=2028`
(the armour-preferring class pick was needed: `CHL2MP_Player` came first
with armor=-1). Round timer is `CGEGameTimer` (props: m_bEnabled m_bStarted
m_bPaused m_flPauseTimeRemaining m_flLength m_flEndTime), entity index 15.

What went wrong, and the fix for each:

* **In-map menu mode flickered.** On character select the cursor-visible
  signal toggled between polls, and IsMenuMode() was re-evaluated separately
  in RenderView, Update and AfterPresent, so the headset swapped between menu
  mode (2D panel, black world, watch hidden) and game mode frame to frame.
  Fix: hysteresis in the MenuInput thread (on = cursor in 3 of the last 8
  polls; off = unseen 400 ms) and menu mode computed ONCE per frame at the top
  of Update (`ComputeMenuMode`, cached in `m_MenuMode`). Transitions now log
  as `Menu mode 0 -> 1 (...)`, max 4 per second.
* **Settings panel / watch flashed on redraw.** SetOverlayFromFile loads
  asynchronously and blanks the overlay meanwhile; hover changes and the
  watch's per-second timer redraw each caused a flash. Fix:
  `vr_flipoverlay.h`, two overlays per panel, load into the hidden one and
  swap on VREvent_ImageLoaded (or after 250 ms).
* **Freeze at 17:17:19**, ~1 s after spawning, following a 518 ms
  AfterPresent. Symbolised with `Release/d3d9.map` (script: scratchpad
  `symbolize.py`): D3D9DeviceEx::Present -> D3D9SwapChainEx::PresentImage ->
  DxvkDevice::waitForSubmission -> DxvkSubmissionQueue::synchronizeSubmission.
  DXVK's submission thread never finished the previous frame; the lock
  pairs were audited and are balanced. Not solved. The watchdog now also
  logs `THREAD <id>: ...` for every other thread with d3d9.dll on its stack
  (copied while suspended, resolved after resume), so the next freeze says
  where the submission thread is stuck. Symbolise those offsets with the
  map file of the SAME build.
* **Timer**: now honours m_bEnabled and m_flPauseTimeRemaining, and takes
  "now" from the player's m_flSimulationTime (the measured tick rate came
  out 1/67 s instead of 1/66.67 and would drift).

## AUDIT BEFORE FIRST TEST (2026-09-21)

* **Grok's `dVGui_Paint` guard was too wide.** `if (g_inStereoPass) return;`
  blocked every VGUI paint during the stereo RenderView, and the hooked engine
  `VGui_Paint` is also what paints the in-game HUD, scoreboard and chat there.
  Now gated on `g_stereoMenu` (IsMenuMode() at the start of the pass), so only
  character/team select is kept out of the eyes. If the HUD elements were
  blank since Grok's build, this was why.
* **Player netvars** now prefer a player class that also networks armour, in
  case `CBasePlayer` is listed before GE:S's own player class.
* **Spew hook** has a re-entrancy guard, so a second spew wrapper chained
  after ours cannot loop back into it.

## WRIST WATCH (2026-09-21)

`vr_watch.cpp`, replacing the old crop-the-HUD wrist overlays, which had
never once been visible: `UpdateWristHUD` bailed on `m_RenderedHud`, which is
only set on the `EyeRenderTargets=true` path. (`UpdateHurtHUD` still has that
gate, so the damage flash is still inert.)

* **Look.** Modelled on a fan "Q Watch" face: steel bezel, GoldenEye 64
  segmented health (left, red to pale yellow) and armour (right, blue to cyan)
  arcs, 12 blocks each, filled from the bottom; green LCD screen with weapon
  name, big clip count / reserve, and `TIME m:ss` in a timed round. Font is
  Bahnschrift SemiBold Condensed (ships with Windows 10), Arial Narrow fallback.
* **Plumbing.** Same as the settings panel: GDI on a draw thread via the
  shared `vr_canvas.cpp` (Canvas + PNG writer, now used by both), PNG to
  %TEMP%, `SetOverlayFromFile` on the render thread. Stats read at 10 Hz,
  redraw only on change.
* **Placement.** Device-relative to the off-hand controller (no lag), with
  the rotation recomputed each frame so the face points at the headset.
  `WatchOffset` (forward,left,up m), `WatchWidth`, `WatchAlwaysVisible`,
  plus the existing `WristLookMaxDistance` / `WristLookMinDot`.
* **Numbers.** `RecvPropStub` was 48 bytes; Source 2007 RecvProp is 60, so
  every netvar walk read garbage after the first prop. That is why
  `Player netvars on ...` never appeared in any log. Fixed; the walk now also
  finds armour, max health/armour, `m_hActiveWeapon`, `m_iAmmo`,
  `m_nTickBase`, and on the weapon classes `m_iClip1` / `m_iPrimaryAmmoType`.
  Weapon name comes from the viewmodel path (`m_ActiveWeaponModel`, which now
  only accepts `v_` models: ammo crates and dropped guns used to overwrite it).
* **Round timer is a guess.** Any class with "timer" in its name that has an
  end-time prop (`m_flTimerEndTime` etc.) is used; its entity is found by
  matching `GetClientClass()` (IClientNetworkable vtable slot 2); game time is
  tick base x a tick interval measured against the wall clock. Every class
  named *timer*/*gamerules*/*round* has its props logged as `NETVARS ...`,
  so if the time never shows, the log says what GE:S actually networks.
  Look for `Round timer candidate`, `Round timer entity at index`,
  `Round timer: remaining=`.
* **Preview:** `watch_preview.cpp` in the session scratchpad compiled
  `vr_watch.cpp` + `vr_canvas.cpp` with stubs and rendered sample states.

## VR SETTINGS PANEL (2026-09-21)

`vr_settings.cpp`. A "VR Settings" entry in the GE:S main/pause menu opens an
in-headset settings panel; left X toggles it in any menu as a fallback.

* **Menu entry.** `Launch-GESVR.ps1` inserts a block above Options in
  `gesource\resource\GameMenu.res` (backup: `GameMenu.res.gesvr-orig`). It runs
  `engine echo gesvr_vrsettings`; `VRSettings::InstallMenuHook` chains into
  tier0's `SpewOutputFunc` (exported, with `GetSpewOutputFunc`) and swallows
  that line. No ConCommand/ICvar ABI involved. Re-chained every 120 frames in
  case the engine installs its own spew function later.
  Log: `VRSettings: menu hook installed`, `VRSettings: opened`.
* **Panel.** Its own overlay (`GESVRSettingsKey`, sort 40, 1.2 m at 1.25 m).
  Drawn with GDI on a worker thread at 2x, filtered down, written as a
  stored-deflate PNG to %TEMP% and handed over with `SetOverlayFromFile` from
  the render thread. No D3D, no Vulkan queue, and IVROverlay stays on one
  thread. Redraws only on change (hover/click), capped ~30/s.
* **Input.** SteamVR laser mouse events on our overlay; our own controller
  ray, then head ray, via `ComputeOverlayIntersection` when no laser for
  400 ms. Click = laser button, A, trigger action or legacy trigger edge
  (300 ms cooldown). B / Y / left X close. While open `ProcessMenuInput` does
  not run and the game menu's queued laser events are drained, so nothing
  leaks through to the game. The game menu is dimmed to alpha 0.35 meanwhile.
* **Saving.** Every change sets the member immediately and queues a write to
  `bin\VR\config.txt` (all duplicate `key=` lines replaced, else appended;
  temp file + MoveFileEx so the hot-reload thread never reads half a file).
* **Reticle.** New styles ring / ringdot and `VRReticleColor`
  (yellow/white/green/red/cyan) in `d3d9_vr.cpp`. Ring styles need size 3+ to
  read as a ring; below that they collapse to a dot.
* **Preview without a headset:** the panel was checked by compiling
  `vr_settings.cpp` into a small exe with stubs for the VR/Game functions it
  calls, rendering each tab to PNG. Worth redoing after any layout change.

## SCOPE + CLEANUP OF GROK'S PASS (2026-09-19)

Grok's uncommitted pass (menu black-behind-panel, level menu panel, Quit
watchdog, GetLastPoses) was reviewed and kept, with these changes:

* **Scope on left grip.** GE:S aim mode is `+aimmode` (SHIFT on desktop);
  `+attack2` is not it -- MOUSE2 is `+aimdetonate`. New action
  `/actions/main/in/Scope`, bound to left grip on all three controllers
  alongside `TwoHand`. The eyes render at the HMD FOV and ignore the engine's
  zoomed `setup.fov`, so `ApplyHeadAndIpd` applies the engine's tan-ratio to the
  eye FOV (whole-view magnification). Log line: `Scope held=...`. If
  `engineFov` does not drop when scoped, GE:S zooms some other way and this
  needs rethinking. `ScopeZoom=false` disables it.
  **SteamVR caches bindings**: if the grip does nothing, reselect the default
  binding in SteamVR's controller settings for the app.
* **Quit watchdog narrowed.** It killed the process after ANY menu click
  followed by a 4s stall, which includes starting a map from the main menu.
  Now requires: click on the pause menu (in map + GameUI), still in map,
  5s stall, and the stuck thread's stack inside `steamclient.dll`.
* **Removed:** per-frame `r_drawvgui 0/1` ClientCmds (queued, so they ran
  back-to-back next frame and did nothing; VGUI is kept out of the eyes by the
  `g_inStereoPass` guard in `dVGui_Paint`), the never-set `g_stereoLeftRT`
  viewport overrides (leftover from the reverted resolution attempt), and the
  head-aim height hack in `dCalcViewModelView` (0x115360 is not the real
  function; see FULL REVIEW).
* **`HeightOffsetMeters`** (default 0): lifts the camera only. Shots still leave
  the engine eye, so they land that far below the reticle at every range.

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

## FULL REVIEW (2026-08-29)

### Healthy

- **Threading discipline holds.** Audited mechanically: no USER32 call reachable
  from any render-thread function, and no compositor Submit/WaitGetPoses outside
  `SubmitThreadBody` / the sanctioned once-per-frame site in `AfterPresent`.
  These two rules cost several sessions to learn; they are currently respected.
- **Config coverage is complete.** Every key in config.txt is parsed, including
  the Wrist/Hurt bounds via `CfgBounds` and `HurtHealthThreshold` via `CfgInt`.
  No setting silently does nothing.
- **Injection, stereo, aim, menus, resolution** all working and confirmed in play.

### Fixed in this pass

- **Duplicate `MenuDriveCursor` key** in config.txt. The parser is a map, so the
  second occurrence silently won - editing the first would have appeared to do
  nothing.

### Known deviations, deliberately left alone

- **`IsInMap()` / `IsGameUIVisible()` are called 4x per frame from D3D code**
  (3 in `VR::Update`, 1 in `AfterPresent`). HANDOFF lists "do not call
  IsInGame() from D3D code" as a landmine, so this is a standing deviation - but
  it has run for the entire project without a single attributable failure.
  Caching it is a real improvement and a real regression risk; not worth taking
  while the goal is playability.
- **~18 OpenVR IPC calls per frame while a menu is up** (8 in `ShowMenuPanel`,
  10 in `ProcessMenuInput`). Hoisting the sticky ones was tried and **broke
  clicking entirely** - overlay input routing is not as sticky as the cosmetic
  properties. Menu-only, so the cost is bounded. Do not "optimize" this again
  without a headset test proving clicks still land.
- **No shutdown path.** Overlays, textures and threads are never released; the
  process exit handles it. Acceptable for an injected mod, worth knowing.

### Dead code

Seven `VR::` functions have no call sites:
`CheckOverlayIntersectionForController`, `GetViewAngle`, `GetViewOriginLeft`,
`GetViewOriginRight`, `PresentStereo`, `SubmitVRTextures`, `VMatrixToHmdMatrix`.
`SubmitVRTextures` is already a warning stub. The rest are inherited from
l4d2vr, where `GetViewOrigin*` feed a render path this project does not use.
Harmless, but do not assume anything in them is running.

### The one real architectural limit

Weapon motion tracking is **not achievable through the render path**, and this
is now proven rather than suspected:

- `ModelRenderInfo_t::origin` does not place a viewmodel. The write fires at
  both `DrawModelExecute` (EXEC MOVE) and `DrawModelSetup` (VM MOVED), with the
  correct model name and a 27-unit displacement, and the gun does not move.
- Patching the renderable's `GetRenderOrigin`/`GetRenderAngles` DOES move it,
  but only the drawn mesh: the entity stays at the head, so the model skews
  between two transforms and muzzle effects spawn from the old position.

The weapon is placed by the **entity's** transform, which in Source is set by
`C_BaseViewModel::CalcViewModelView` calling SetLocalOrigin/SetLocalAngles. This
project has never had that function: the hook at 0x115360 was measured firing at
1.6/s with a unit-vector argument and a stack address for `this` - it is some
other function entirely. Finding the real one is the prerequisite for motion
weapons, and nothing else will substitute.

Head-aim mode sidesteps all of it, which is why it is the current default.

---

## RESOLUTION RULE: CANNOT EXCEED THE DESKTOP (2026-08-29, corrected)

In windowed mode Source cannot create a window larger than the desktop. It dies
during video init, waiting forever for client.dll. On a 1920x1080 desktop:

| Resolution | vs desktop | Boots |
|---|---|---|
| 1280x720  | fits          | yes |
| 1920x1080 | fits exactly  | yes |
| 1600x1200 | 1200 > 1080   | **NO** |
| 1280x1280 | 1280 > 1080   | **NO** |
| 2560x1440 | both exceed   | **NO** |

**An earlier version of this note blamed the ASPECT RATIO. That was wrong.** It
only looked that way because every taller resolution tested also happened to
exceed the desktop height. The real constraint is size, not shape - which also
finally explains the 1280x1280 failure that opened this whole project.

### Consequence

**1920x1080 is the ceiling on a 1080p desktop, and raising the window is a dead
end for sharpness.** The remaining route is `EyeRenderTargets=true` with
`EyeRenderScale`, which renders each eye at a MULTIPLE OF THE WINDOW internally
and is not bound by the desktop at all (1.5 gives 2880x1620 from a 1920x1080
window).

That account of the sliver was WRONG, and believing it cost real time. What
actually happened, established by measurement on 2026-08-30:

* The textures were allocated at one size while the viewport was set from
  another, so the scene drew into a rectangle that did not match its target.
  The size was never the problem.
* `RT_SIZE_NO_CHANGE` clamps a render target to the framebuffer, and the SDK
  header says it is "only allowed for render targets that don't want a depth
  buffer" -- ours ask for one. `RT_SIZE_LITERAL` is the mode that means what it
  says: "Don't clamp it to the frame buffer size. Really."
* The 2D path is solved: the HUD is stripped from the eye passes and drawn in
  its own backbuffer pass, so `CaptureForOverlay` still sees a full frame.

Measured facts, not inferences:

* Both eye targets render FULLY -- read back and checked, 2565x2661 of
  2565x2661. The engine honours a target larger than the framebuffer.
* Both eye texture handles are valid, distinct, and correctly sized.
* The crop bounds and the compositor path are correct. Mono proves this: it
  submits ONE texture with each eye's own bounds and looks right in both eyes.
* The material system MUST be flushed (`IMatRenderContext::Flush(true)`) before
  capturing. Source buffers draw calls and our capture talks to D3D directly,
  so without it we read an unfinished target. This took the right eye from
  ~20% to ~60% drawn, and it explains why the LEFT eye was always fine: the
  pass that followed it flushed its work, and the right pass had nothing
  following it.

**What remains unsolved:** the SECOND `RenderView` of a frame draws at the
BACKBUFFER's viewport rather than the target's -- 1080 of 1873 rows is the 60%.
Forcing a rebind through null did not change it, and neither did separate
targets. Do not guess at this a fourth time: `GetRenderTargetDimensions` is
slot 10 of `IMatRenderContext`, and logging it per pass will say plainly what
the engine thinks the viewport is.

Meanwhile `MonoEye=true` gives full resolution and a correct picture in both
eyes at the cost of stereo depth, and `EyeRenderTargets=false` is the working
stereo path at the old resolution. Both are one config line.

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
