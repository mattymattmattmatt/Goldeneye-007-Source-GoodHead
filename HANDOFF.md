# GESVR — GoldenEye: Source VR — Handoff

Last updated: **2026-08-28**, after the "crashes just after the menu pops up" session.
Owner: Matty. Headset: SteamVR. Target quality: HL2VR / HaloCEVR, not "2D in Theater".

---

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
