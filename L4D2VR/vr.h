#pragma once
#include "openvr.h"
#include "vector.h"
#include <chrono>

#define MAX_STR_LEN 256

class Game;
struct WatchStats;
class IDirect3DTexture9;
class IDirect3DSurface9;
class ITexture;
class CViewSetup;


struct TrackedDevicePoseData 
{
	std::string TrackedDeviceName;
	Vector TrackedDevicePos;
	Vector TrackedDeviceVel;
	QAngle TrackedDeviceAng;
	QAngle TrackedDeviceAngVel;
};

struct SharedTextureHolder 
{
	// These MUST be zero-initialized. VR is built with a user-provided
	// constructor, so members without initializers hold heap garbage, and a
	// garbage m_VRTexture.handle passes every "is it ready?" null check and
	// gets handed straight to OpenVR. That was the deterministic crash the
	// moment the main menu overlay was first submitted.
	vr::VRVulkanTextureData_t m_VulkanData{};
	vr::Texture_t m_VRTexture{};
};


class VR
{
public:
	Game *m_Game = nullptr;

	vr::IVRSystem *m_System = nullptr;
	vr::IVRInput *m_Input = nullptr;
	vr::IVROverlay *m_Overlay = nullptr;

	vr::VROverlayHandle_t m_MainMenuHandle = 0;
	vr::VROverlayHandle_t m_HUDHandle = 0;
	vr::VROverlayHandle_t m_WorldHandle = 0;
	vr::VROverlayHandle_t m_HurtHUDHandle = 0;      // health bars in front of the HMD when damaged

	float m_HorizontalOffsetLeft = 0.0f;
	float m_VerticalOffsetLeft = 0.0f;
	float m_HorizontalOffsetRight = 0.0f;
	float m_VerticalOffsetRight = 0.0f;

	uint32_t m_RenderWidth = 0;
	uint32_t m_RenderHeight = 0;
	float m_Aspect = 1.0f;
	float m_Fov = 90.0f;

	// Per-eye asymmetric frustum crop, derived from GetProjectionRaw. Both eyes
	// are rendered once at a symmetric superset FOV (m_Fov); these bounds pull
	// each eye's real sub-rectangle back out at submit time.
	vr::VRTextureBounds_t m_TextureBounds[2] = { { 0.0f, 0.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } };
	bool m_HaveTextureBounds = false;
	vr::TrackedDevicePose_t m_Poses[vr::k_unMaxTrackedDeviceCount]{};

	Vector m_EyeToHeadTransformPosLeft = { 0,0,0 };
	Vector m_EyeToHeadTransformPosRight = { 0,0,0 };

	Vector m_HmdForward = { 1,0,0 };
	Vector m_HmdRight = { 0,-1,0 };
	Vector m_HmdUp = { 0,0,1 };

	Vector m_HmdPosLocalInWorld = { 0,0,0 };

	Vector m_LeftControllerForward = { 1,0,0 };
	Vector m_LeftControllerRight = { 0,-1,0 };
	Vector m_LeftControllerUp = { 0,0,1 };

	Vector m_RightControllerForward = { 1,0,0 };
	Vector m_RightControllerRight = { 0,-1,0 };
	Vector m_RightControllerUp = { 0,0,1 };

	Vector m_ViewmodelForward = { 1,0,0 };
	Vector m_ViewmodelRight = { 0,-1,0 };
	Vector m_ViewmodelUp = { 0,0,1 };

	Vector m_HmdPosAbs = { 0,0,0 };
	Vector m_HmdPosAbsPrev = { 0,0,0 };
	QAngle m_HmdAngAbs = { 0,0,0 };

	Vector m_HmdPosCorrectedPrev = { 0,0,0 };
	Vector m_HmdPosLocalPrev = { 0,0,0 };

	Vector m_SetupOrigin = { 0,0,0 };
	Vector m_SetupOriginPrev = { 0,0,0 };
	Vector m_CameraAnchor = { 0,0,0 };
	Vector m_SetupOriginToHMD = { 0,0,0 };

	float m_HeightOffset = 0.0;
	bool m_RoomscaleActive = false;

	Vector m_LeftControllerPosAbs = { 0,0,0 };
	QAngle m_LeftControllerAngAbs = { 0,0,0 };
	Vector m_RightControllerPosAbs = { 0,0,0 };
	QAngle m_RightControllerAngAbs = { 0,0,0 };

	Vector m_ViewmodelPosOffset = { 0,0,0 };
	QAngle m_ViewmodelAngOffset = { 0,0,0 };

	float m_Ipd = 0.0f;
	bool m_HaveSeatPose = false;
	Vector m_SeatHmdPos = { 0, 0, 0 };																	
	float m_EyeZ = 0.0f;

	Vector m_IntendedPositionOffset = { 0,0,0 };

	enum TextureID
	{
		Texture_None = -1,
		Texture_LeftEye,
		Texture_RightEye,
		Texture_HUD,
		Texture_Blank
	};

	ITexture *m_LeftEyeTexture = nullptr;
	ITexture *m_RightEyeTexture = nullptr;
	ITexture *m_HUDTexture = nullptr;
	ITexture *m_BlankTexture = nullptr;

	IDirect3DSurface9 *m_D9LeftEyeSurface = nullptr;
	IDirect3DSurface9 *m_D9RightEyeSurface = nullptr;
	IDirect3DSurface9 *m_D9HUDSurface = nullptr;
	IDirect3DSurface9 *m_D9BlankSurface = nullptr;

	SharedTextureHolder m_VKLeftEye;
	SharedTextureHolder m_VKRightEye;
	SharedTextureHolder m_VKBackBuffer;
	SharedTextureHolder m_VKHUD;
	SharedTextureHolder m_VKWorld;
	SharedTextureHolder m_VKBlankTexture;

	bool m_IsVREnabled = false;
	bool m_IsInitialized = false;
	bool m_RenderedNewFrame = false;
	// WaitGetPoses blocks until the compositor's running start. It must happen
	// exactly once per frame -- CViewRender::Render and CViewRender::RenderView
	// both used to call it, so the second call parked the render thread for a
	// whole frame period. Cleared at the end of each VR::Update (Present).
	bool m_PosesThisFrame = false;
	bool m_RenderedHud = false;
	bool m_CreatedVRTextures = false;
	TextureID m_CreatingTextureID = Texture_None;

	bool m_PressedTurn = false;
	bool m_PushingThumbstick = false;
	bool m_ReloadGestureLatched = false;
	bool m_TwoHanded = false;

	// action set
	vr::VRActionSetHandle_t m_ActionSet = vr::k_ulInvalidActionSetHandle;
	vr::VRActiveActionSet_t m_ActiveActionSet{};

	// actions
	vr::VRActionHandle_t m_ActionJump = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionPrimaryAttack = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionSecondaryAttack = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionReload = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionTwoHand = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionScope = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionWalk = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionTurn = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionUse = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionNextItem = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionPrevItem = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionResetPosition = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionCrouch = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionFlashlight = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionActivateVR = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuSelect = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuBack = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuUp = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuDown = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuLeft = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuRight = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_Spray = vr::k_ulInvalidActionHandle; 
	vr::VRActionHandle_t m_Scoreboard = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ShowHUD = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_Pause = vr::k_ulInvalidActionHandle;

	TrackedDevicePoseData m_HmdPose;
	TrackedDevicePoseData m_LeftControllerPose;
	TrackedDevicePoseData m_RightControllerPose;

	float m_RotationOffset = 0;
	std::chrono::steady_clock::time_point m_PrevFrameTime;

	float m_TurnSpeed = 0.3;
	bool m_SnapTurning = false;
	float m_SnapTurnAngle = 45.0;
	bool m_LeftHanded = false;
	float m_VRScale = 43.2;
	float m_IpdScale = 1.0;
	bool m_HideArms = false;
	float m_HudDistance = 1.3;
	float m_HudSize = 1.1;
	bool m_HudAlwaysVisible = false;
	bool m_EnableNetVR = true;
	// false = look and shoot with the HMD. true = guns follow the right controller.
	bool m_MotionControls = true;

	// --- Display path -------------------------------------------------------
	// compositor : real per-eye Submit with asymmetric frustum crop. This is
	//              what makes the world sit at true scale around you.
	// sbs        : legacy head-locked side-by-side quad. Stereo, but it reads
	//              as a giant 3D television because one flat rectangle cannot
	//              reproduce two asymmetric eye frusta.
	// both       : compositor + the quad on top, for A/B comparison.
	enum DisplayModeID { Display_Compositor = 0, Display_SBS = 1, Display_Both = 2 };
	int m_DisplayMode = Display_Compositor;
	bool m_UseTextureBounds = true;
	// Horizontal crop is unambiguous. Vertical depends on OpenVR's top/bottom
	// sign convention against D3D's v-down texture origin -- if the image sits
	// too high or too low, turn this off and only the u crop is applied.
	bool m_UseVerticalCrop = true;
	// Multiplier on the WINDOW size for the eye render targets, used instead of
	// the HMD's recommended size. 1.5 gives 1920x1080 from a 1280x720 window --
	// sharper than native without going anywhere near the 2496x2688 that
	// crashes. Only applies when EyeRenderTargets is on.
	float m_EyeRenderScale = 1.5f;

	// Render each eye into its own engine render target instead of drawing both
	// into the backbuffer one on top of the other. The backbuffer approach is
	// what produces "a stereoscopic view with another wonky frame on top": the
	// second eye overwrites the first in the same buffer. The l4d2vr reference
	// does SetRenderTarget(m_LeftEyeTexture) around each eye pass, and its
	// IMatRenderContext vtable is byte-identical to ours, so the call is as safe
	// here as there. Also renders at HMD resolution rather than window size,
	// which is where the softness comes from.
	// ON by default now. Every log has shown eyeRT=0 size=1280x720: each eye
	// captured from the window backbuffer and upscaled to a ~2496x2688 panel,
	// which is the jaggies. Renders each eye into its own RT at true HMD
	// resolution instead. Costs performance; EyeRenderTargets=false reverts.
	// OFF: this path renders sharply but the RIGHT eye is only ~60% drawn.
	//
	// The old note here blamed the sliver on using the HMD's recommended size.
	// That was WRONG and it cost real time. What actually happened:
	//   - the textures were allocated at one size and the viewport set from
	//     another, so the scene drew into a rectangle that did not match its
	//     target. Size was never the problem.
	//   - RT_SIZE_NO_CHANGE clamps a target to the framebuffer, and the SDK
	//     says it is only valid for targets with no depth buffer. RT_SIZE_LITERAL
	//     is the one that means what it says.
	//   - the 2D path is handled: the HUD draws in its own backbuffer pass.
	//
	// Measured, not assumed:
	//   - both eye targets render FULLY (read back and checked, 2565x2661 of
	//     2565x2661), so the engine honours a target larger than the framebuffer
	//   - both texture handles are valid, distinct and correctly sized
	//   - the crop bounds and compositor path are correct, which mono proves by
	//     submitting ONE texture with each eye's own bounds and looking right
	//   - the material system must be flushed before capturing or the capture
	//     reads an unfinished target; that took the right eye 20% -> 60%
	//
	// What remains: the SECOND RenderView of a frame draws at the BACKBUFFER's
	// viewport instead of the target's (1080 of 1873 rows = the 60%). Neither a
	// forced rebind through null nor separate targets changed that. The next
	// thing to try is asking the engine directly -- GetRenderTargetDimensions is
	// slot 10 of IMatRenderContext -- rather than reasoning about it again.
	bool m_UseEyeRenderTargets = false;
	// Size of the superset-frustum eye render targets. Computed in Init from the
	// HMD's recommended per-eye size divided by how much of the superset image
	// each eye actually uses, so that AFTER the texture-bounds crop each eye
	// still has roughly native resolution. The texture and the viewport MUST be
	// these same numbers -- them disagreeing is what rendered a sliver.
	uint32_t m_EyeRTWidth = 0;
	uint32_t m_EyeRTHeight = 0;
	// Draw the 2D HUD in its own backbuffer pass instead of into the eye
	// targets, where it lands at the wrong size and only in one eye.
	// Set false if the extra pass costs more than it is worth.
	bool m_EyeHudPass = true;

	// Send the left eye's image to both eyes. No stereo depth, but a correct
	// and sharp picture while the right eye's black frame is unsolved.
	bool m_MonoEye = true;
	// Which texture feeds both eyes in mono. 0 = left, 1 = right. Setting 1 is
	// the test for the right eye's black frame: it shows the right texture on
	// its own, separating a bad texture from a bad two-texture submit.
	int m_MonoEyeSource = 0;
	// Render both eyes into ONE target, capturing each before the next pass
	// overwrites it. Saves a colour and a depth buffer with no loss of
	// resolution, which matters in a 32-bit process.
	// NOTE: when this is on, m_RightEyeTexture is the SAME pointer as
	// m_LeftEyeTexture, not a second texture. Nothing releases them today, but
	// any future cleanup must not free both.
	bool m_SharedEyeTarget = true;
	// TRUE keeps the original vertical crop convention.
	//
	// The arithmetic argues the other way: horizontal uses 0.5 + 0.5*left/tan
	// while vertical used 0.5 - 0.5*bottom/tan, and with the measured frustum
	// that crops the bottom 84% where the eye appears to need the top 84%.
	// Flipping it was tried and the result looked wrong -- BUT two other things
	// changed in the same build, so that is NOT a clean result and the theory
	// is unproven either way. If you revisit it, change this alone.
	//
	// The high gun is also still unexplained. ViewmodelFov=70 did not fix it and
	// made the weapon disagree with world-space muzzle effects, so it is back at
	// 0 (world FOV). Do not assume either cause without testing it on its own.
	bool m_EyeCropLegacyV = true;

	// IVModelRender vtable index of DrawModelExecute. Measured, not guessed:
	// a naked per-slot counter showed [18]=128689 and [19]=128291 as the two
	// hottest slots, differing by 398 -- Source's DrawModelSetup/DrawModelExecute
	// pair. Change only if a future engine build moves it.
	int m_ModelDrawExecuteSlot = 19;
	// The other half of the pair. Moving the weapon must happen HERE, before
	// bone matrices are built -- see dDrawModelSetup.
	int m_ModelDrawSetupSlot = 18;
	// OFF by default. Hooking slot 18 with a guessed DrawModelSetup signature
	// crashed on the first stereo pass -- the argument list does not match, so
	// calling through corrupts the stack instantly. Measure the real signature
	// (SetupProbe) before turning this on again.
	// ON. The crash was arity, not the slot: slot 18's first argument was
	// confirmed to be a ModelRenderInfo_t (resolved to a real model path),
	// and the probe showed FOUR arguments where the typedef declared three.
	// The motion trace separately proved the whole hand->world chain is
	// correct and that the Execute-time write fires but is ignored, so this
	// is the only remaining place the weapon can be moved.
	bool m_WeaponSetupHook = true;
	// Redirect the viewmodel's GetRenderOrigin/GetRenderAngles to our pose.
	// Writing ModelRenderInfo_t::origin provably does not move the gun; the
	// root transform comes from these accessors via SetupBones.
	// OFF -- froze on map load. Patching slots 1/2 of pRenderable's vtable is
	// unsafe: ModelRenderInfo_t::pRenderable is an IClientRenderable*, but
	// that vtable is shared by every entity deriving from the same class, so
	// the patch is nowhere near as narrow as intended -- and if the pointer
	// is a multiple-inheritance sub-object, slots 1/2 are not the accessors
	// at all. Needs the vtable identified before this can be retried.
	// ON, now on evidence rather than assumption: VMVT proved the viewmodel
	// and prop renderable vtables are DISTINCT (29416664 vs 293FC5B4), so the
	// patch is narrow, and slots 1/2 resolve to adjacent small client.dll
	// functions, the shape of GetRenderOrigin/GetRenderAngles. The earlier
	// freeze was unrelated -- VmRenderable never logged, so it never applied.
	// C_BaseAnimating::SetupBones builds the root transform from these two
	// accessors, so every bone follows: no IK work needed.
	bool m_ViewmodelRenderablePatch = true;
	// true  = override only during the draw call (cleaner, but the gun snaps
	//         back to the head on any draw path we do not intercept, e.g.
	//         while firing)
	// false = override latched on permanently (gun stays on the hand, but the
	//         engine also gets our pose for culling/attachments, which is what
	//         made it warp)
	// Neither is correct: both are symptoms of a RENDER-ONLY override. The
	// real fix is to move the viewmodel entity itself.
	bool m_ViewmodelScopedPose = true;
	// Safe naked capture of slot 18's actual arguments.
	bool m_SetupProbe = true;
	// Motion trace: samples the ENTIRE hand->weapon chain 4x/second while in a
	// map, so a scripted set of arm movements can be correlated against the
	// numbers and each stage verified independently.
	bool m_MotionDebug = true;
	// Re-run the slot counter (conflicts with the real hook; for diagnosis only).
	bool m_VtableProbe = false;
	// Floating menu panel. The in-game character/level menu was reported as
	// too big and too close; these make it placeable without a rebuild.
	float m_MenuWidthMeters = 2.4f;
	// Pregame (create-server) menu distance. In-map uses m_InGameMenuDistance.
	float m_MenuDistanceMeters = 1.6f;
	// Route in-map VGUI panels (character/team/level select) onto the flat menu
	// panel instead of leaving them inside the 3D view. Detected via the OS
	// cursor becoming visible. Set false to restore the old behaviour.
	bool m_InGameMenuPanel = true;
	// Refresh the overlay capture every in-game frame regardless of HUD
	// settings. That capture also applies the menu alpha fix.
	bool m_AlwaysCaptureOverlay = false;

	// Console commands run once per map, from the ExtraCvars config key.
	// Exists so renderer settings can be bisected without a rebuild.
	std::string m_ExtraCvars;
	bool m_ExtraCvarsDone = false;
	unsigned m_InMapSinceMs = 0;
	// The in-map panel wants to sit further back than the create-server menu,
	// which wants to stay close enough to read. They used to share one distance.
	float m_InGameMenuDistance = 2.4f;
	// Keep the menu the same apparent size as resolution changes. See
	// VR::EffectiveMenuGeometry -- Source's GameUI is laid out in fixed pixels.
	bool m_MenuScaleWithRes = true;

	float m_SbsWidthMeters = 3.17f;
	float m_SbsDistance = 1.0f;

	// --- Menu input ---------------------------------------------------------
	// Win32 messages to the "Valve001" window are vtable-independent and cannot
	// corrupt the stack. The IInputInternal path depends on a VGUI vtable whose
	// 2007 layout is unconfirmed, so it stays off unless explicitly enabled.
	// Menus are pointed at with the CONTROLLER only (the pointing hand: right,
	// or left with LeftHanded). SteamVR's laser point is used while its laser
	// is on the panel; elsewhere (in-map menus, where SteamVR shows no laser)
	// our own ray from the same controller tip. There used to be a head
	// pointer too, and the config parser turned "auto" into "head", so a head
	// cursor took over on every frame the laser held still.
	int m_MenuAimX = -1, m_MenuAimY = -1;   // last aim on the game menu, window px
	bool m_MenuLaserOnPanel = false;         // SteamVR's laser is on the game menu
	// Our marker is the only cursor in the headset when SteamVR's laser is not
	// on the panel: Source's cursor is a hardware cursor, composited by Windows
	// and never in the captured frame. It is drawn at the controller aim point,
	// the same point clicks go to, and never while SteamVR's own dot is there.
	bool m_DrawMenuCursor = true;
	bool m_MenuUseWin32 = true;
	// Kill switch for all OS-level cursor driving (SetCursorPos /
	// SetForegroundWindow / PostMessage). Turn off to rule the whole
	// subsystem out without a rebuild.
	bool m_MenuDriveCursor = true;
	// Menu compositor cadence, ms. OpenVR's Vulkan Submit synchronizes against
	// the graphics queue we hand it, so its cost scales with how much game work
	// piled up since the previous submit. At the old 500ms keepalive the engine
	// free-ran at ~225fps, leaving ~110 frames to drain -- measured at 83-150ms
	// of hard stall every half second. Display cadence keeps the backlog to a
	// couple of frames. Set 500 to restore the old behaviour.
	// 33ms (30Hz) is a deliberate middle ground: it cuts the queue backlog ~15x
	// (83ms stall -> ~5ms, imperceptible) without returning to the every-frame
	// cadence that was reported to deadlock against a VGUI click. Lower it to 11
	// if 33 proves stable; raise to 500 to restore the old stuttering behaviour.
	int m_MenuKeepaliveMs = 33;
	// SteamVR overlay properties are sticky and every setter is an IPC
	// round-trip to vrserver; these track "already configured / already shown"
	// so only the texture is pushed per frame.
	bool m_MenuOverlayConfigured = false;
	bool m_MenuOverlayShown = false;
	int m_MenuCfgW = 0;
	int m_MenuCfgH = 0;

	// --- Compositor submit thread ------------------------------------------
	// vr::VRCompositor() Submit/WaitGetPoses must NEVER run on the D3D Present
	// callstack: both DXVK's present and OpenVR's Vulkan submit drive the same
	// graphics queue, and doing them from one thread deadlocks. Proven by the
	// 13:26 log -- MENU f=5 logged, AfterPresent tick #5 never did, watchdog then
	// counted 22s with the process alive.
	//
	// Division of labour: the render thread does all D3D work (capture, blit) and
	// publishes a ready texture; this thread does nothing but WaitGetPoses and
	// Submit. No D3D call ever happens on the submit thread.
	SharedTextureHolder m_SubmitBlack{};
	bool m_BlackPrepared = false;
	void PrepareBlackTexture();
	void SubmitThreadBody();
	// SteamVR's desktop mirror window competes for Win32 focus and costs GPU
	// time, and a VR mod gains nothing from it. Source throttles rendering and
	// mutes audio whenever its own window is not the active app.
	bool m_ShowMirrorWindow = false;
	// 0 = suppress Theater every frame (old behaviour).
	int m_TheaterHideThrottleMs = 2000;
	bool m_MenuUseVguiInternal = false;

	// --- Motion-controlled weapons -----------------------------------------
	// A tracked controller's forward axis points out the front of the device,
	// which is NOT where a gun barrel points when you hold one naturally. Both
	// L4D2VR and HaloCEVR rotate the grip down ~45 degrees to compensate.
	// UpdateTracking() used to do this for GE:S but is never called, so the
	// correction is applied where the viewmodel basis is actually built.
	// DEFAULT 0. Aim direction was reported correct apart from the pitch
	// inversion, so once that is fixed any grip offset would tilt aim off by
	// that many degrees. Raise it only if the gun MODEL sits at a wrong angle
	// in your hand -- it now offsets pitch directly and no longer touches yaw.
	// 45 degrees, confirmed by play: with 0, pointing straight ahead shot ~45
	// degrees HIGH. A tracked controller's -Z device axis sits well above the
	// line you intuitively aim along, which is what this corrects. Source
	// pitch is positive-DOWN, so +45 brings the shot down onto your point of
	// aim. This is now a direct pitch offset -- it no longer round-trips
	// through VectorAngles, which is what inverted pitch previously.
	// Viewmodel FOV. 0 = use the world FOV (currently ~106 for the HMD's
	// superset frustum), which is very wide and pushes the weapon toward the
	// centre of view. A narrower value puts it back where Source normally
	// draws it. Try 54-75.
	float m_ViewmodelFov = 0.0f;
	float m_GunGripAngle = 45.0f;
	// User tweak ADDED on top of the per-weapon table value, so setting
	// ViewmodelOffset in config no longer erases the per-weapon pose.
	Vector m_ViewmodelUserOffset = { 0.0f, 0.0f, 0.0f };
	// Angle tweak applied to the RENDERED WEAPON ONLY (pitch, yaw, roll in
	// degrees). Kept separate from GunGripAngle, which corrects AIM: aim is
	// already reported good, so model alignment must be tunable without
	// disturbing where the bullets go.
	Vector m_ViewmodelAngleOffset = { 0.0f, 0.0f, 0.0f };
	// Per-weapon poses come from Weapons::GetOffset. That table was unreachable
	// (its only caller, UpdateTracking(), has no call sites), so every gun used
	// one generic pose. Set false to go back to that.
	// DEFAULT OFF. The offset table in weapons.cpp is inherited from l4d2vr and
	// assumes L4D2 viewmodel origins. Its generic fallback pushes the gun 18
	// units BACK along the barrel from your hand, which on GE:S models may put
	// it inside or behind you. With this off the weapon sits exactly at the
	// controller, which is predictable and visible. Turn on to start tuning.
	bool m_PerWeaponOffsets = false;
	// Off-hand near the barrel aims along both hands. Also stranded in
	// UpdateTracking() until now.
	// HaloCEVR-style: HOLD the off-hand grip to go two-handed, instead of
	// guessing from hand distance. Distance alone fired whenever your hands
	// happened to pass near each other.
	bool m_TwoHandedGrip = true;
	bool m_TwoHandedNeedsGrip = true;

	// Scope (left grip = +aimmode). GE:S zooms by narrowing the engine FOV, which
	// the eyes ignore because they use the HMD's FOV; this carries the zoom
	// ratio across. m_ScopeBaseFov is the engine FOV when not zoomed.
	bool m_ScopeZoom = true;
	float m_ScopeBaseFov = 0.0f;
	int m_ScopeReleasedFrames = 0;

	// Tracked weapon: the first-person gun is drawn at the right controller
	// (see dDrawModelExecute). Step 1 is the model only; aim is still the head.
	bool m_TrackedWeapon = false;
	// Undo the stretch GE:S's viewmodel pass gets from drawing at the window's
	// aspect, for the head-locked gun too (the gun in hand always gets it).
	bool m_FixViewmodelAspect = true;
	// Face aim: how far the head-locked gun sits out towards the lower right,
	// as a share of where GE:S's FOV correction puts its muzzle flash (1 =
	// there, 0 = straight in front of your face).
	float m_FaceAimGunSpread = 1.0f;
	// How far the death curtain is stretched across the view.
	float m_BloodCurtainScale = 2.6f;
	// Switch weapons as you press (hud_fastswitch 1): GE:S's pick-then-fire
	// list is a 2D HUD panel the headset never shows.
	bool m_WeaponFastSwitch = true;
	// Numpad adjustment of where the held weapon sits in the hand. Off unless
	// someone wants to re-place a weapon (VR Settings > Weapons).
	bool m_WeaponTuning = false;
	// Texture filtering forced on the game (mat_forceaniso N + mat_trilinear 1);
	// 0 leaves GE:S's own setting alone. GE:S was on bilinear with no
	// anisotropic filtering, which blurs floors and walls seen at an angle --
	// in a headset, most of what you look at.
	int m_TextureFiltering = 16;
	// GE:S's bloom (mat_disable_bloom).
	bool m_Bloom = true;
	// Set when either changes in VR Settings; applied on the next in-map frame.
	bool m_GraphicsDirty = false;
	void ApplyGraphicsCvars();
	// GE:S's first-person death camera rides the ragdoll's head (ge_fp_ragdoll).
	bool m_DeathCamFirstPerson = false;
	// The game HUD in the headset: 0 off, 1 flash it when hurt, 2 always.
	// The Show HUD button works in every mode.
	int m_GameHudMode = 1;
	// Tracked gun aim: shots go where the barrel points (a trace down the
	// barrel, then view angles from the eye to its hit point).
	bool m_AimWithGun = true;
	Vector m_GunAimPoint = { 0.0f, 0.0f, 0.0f };
	// The view angles swing to the barrel only while attacking (the game also
	// walks along them). m_AttackAimUntil: keep the barrel angles until then.
	// m_AttackAimApplied: the last RenderView set them, so a shot sent now
	// goes down the barrel.
	unsigned long long m_AttackAimUntil = 0;
	bool m_AttackAimApplied = false;
	// Throwing knife, thrown with a flick of the right controller: the flick's
	// direction in the game world, and until when the view angles hold it --
	// GE:S lets the knife go a moment after the press, along the eye angles
	// of that moment.
	Vector m_ThrowDir = { 1.0f, 0.0f, 0.0f };
	unsigned long long m_ThrowAimUntil = 0;
	// Slappers with the tracked weapon: a fast swing of the gun hand slaps.
	bool m_SwingMelee = true;
	float m_SwingSpeed = 2.0f;   // metres per second of hand speed
	// Arm-rig weapons (slappers, knives): show only the hand, not the arm, and
	// an extra pitch,yaw,roll for the hand around the controller.
	bool m_MeleeHideArm = true;
	Vector m_MeleeAngleOffset = { 0.0f, 0.0f, 0.0f };

	// Lifts the camera, in metres, for playing standing. Only the camera: the
	// engine's eye (where shots come from) and the weapon stay put.
	float m_HeightOffsetMeters = 0.0f;

	// Wrist watch (vr_watch.cpp). Shown when you look at the off hand, or always.
	bool m_ShowWristHUD = true;
	bool m_WatchAlwaysVisible = false;
	float m_WristLookMaxDistance = 0.6f;
	float m_WristLookMinDot = 0.45f;
	float m_WatchWidth = 0.10f;
	Vector m_WatchOffset = { -0.13f, 0.0f, 0.04f };      // forward, left, up, metres
	vr::VRTextureBounds_t m_HurtHUDBounds = { 0.00f, 0.78f, 0.50f, 1.00f };
	float m_HurtHUDWidth = 0.55f;
	float m_HurtHUDDistance = 0.85f;
	float m_HurtHUDSeconds = 2.5f;
	int m_HurtHealthThreshold = 80;

	// Networked-variable offsets, found by walking the client class tables.
	int m_HealthNetvar = -1;
	int m_ArmorNetvar = -1;
	int m_MaxHealthNetvar = -1;
	int m_MaxArmorNetvar = -1;
	int m_ActiveWeaponNetvar = -1;
	int m_AmmoNetvar = -1;
	int m_TickBaseNetvar = -1;
	int m_Clip1Netvar = -1;
	int m_PrimaryAmmoTypeNetvar = -1;
	int m_ViewModelIndexNetvar = -1;
	// Set once the held weapon's name comes from its own m_iViewModelIndex;
	// from then on the DrawModelExecute guess (last v_ model drawn) is ignored.
	bool m_WeaponFromNetvar = false;
	int m_LastHealth = -1;
	std::chrono::steady_clock::time_point m_HurtUntil{};
	bool m_LookingAtWrist = false;

	VR() {};
	VR(Game *game);
	int SetActionManifest(const char *fileName);
	void InstallApplicationManifest(const char *fileName);
	static void MakeVRPath(char *out, size_t outCount, const char *relative);
	void Update();
	void ComputeEyeRTSize();
	void CreateVRTextures();
	void SubmitVRTextures();
	void RepositionOverlays();
	void CreateWristOverlays();
	void UpdateHurtHUD();
	void ResolvePlayerNetvars();
	int ReadLocalHealth();
	void ReadWatchStats(WatchStats &out);
	bool ScopeHeld();
	void RefreshActiveWeapon();
	void UpdateGameCrosshair();
	void ProcessTuneKeys();
	void UpdateGunAim(const CViewSetup &left, const CViewSetup &right);
	int ReadRoundTimeLeft(void *player);
	bool IsLookingAtOffhandWatch();
	void GetPoses();
	void UpdatePosesAndActions();
	void GetViewParameters();
	void ProcessMenuInput();
	// Edge-triggered console command. Source's command buffer is a fixed-size
	// queue; ProcessInput was issuing +forward/-back/+duck/-duck and friends on
	// EVERY frame (32 call sites, several unconditional if/else pairs), which at
	// 200+ fps is over a thousand commands a second and can overflow the buffer.
	// This sends a given +cmd/-cmd only when its state actually changes.
	void MoveCmd(const char *cmd);
		void ProcessInput();
	// Decided once per frame at the top of Update, then read by everything
	// else (AfterPresent, RenderView) so they cannot disagree mid-frame.
	bool IsMenuMode() const { return m_MenuMode; }
	bool ComputeMenuMode();
	bool m_MenuMode = false;
	int m_VguiCursor = -1;   // VGUI's "a panel needs the mouse": 1/0, -1 unknown
	// Called after DXVK Present returns. Menu-only compositor tick so we never
	// WaitGetPoses/Submit on the same callstack as IDirect3DDevice9::Present.
	void AfterPresent();
	bool ComputeMenuPointer(int &x, int &y);
	bool GetPointerPose(vr::HmdMatrix34_t &out);
	void EffectiveMenuGeometry(float &widthM, float &distM) const;
	void ApplyExtraCvars();
	void ShowMenuPanel();
	void HideMenuPanel();
	void ShowWorldStereoOverlay();
	void SubmitStereoToCompositor();
	bool TextureReady(const SharedTextureHolder &tex) const;
	void PresentStereo();
	void HideWorldOverlay();
	void SubmitBlackEyes();
	void PlaceMenuPanelInFront();
	VMatrix VMatrixFromHmdMatrix(const vr::HmdMatrix34_t &hmdMat);
	vr::HmdMatrix34_t VMatrixToHmdMatrix(const VMatrix &vMat);
	vr::HmdMatrix34_t GetControllerTipMatrix(vr::ETrackedControllerRole controllerRole);
	bool CheckOverlayIntersectionForController(vr::VROverlayHandle_t overlayHandle, vr::ETrackedControllerRole controllerRole);
	QAngle GetRightControllerAbsAngle();
	Vector GetRightControllerAbsPos();
	QAngle GetLeftControllerAbsAngle();
	Vector GetLeftControllerAbsPos();
	Vector GetRecommendedViewmodelAbsPos();
	QAngle GetRecommendedViewmodelAbsAngle();
	void UpdateTracking();
	Vector GetViewAngle();
	Vector GetViewOriginLeft();
	Vector GetViewOriginRight();
	void ApplyHeadAndIpd(CViewSetup &left, CViewSetup &right, const CViewSetup &setup);
	// Reads the trigger off the device directly, bypassing the action manifest.
	bool LegacyTriggerDown(float *outValue = nullptr);
		bool PressedDigitalAction(vr::VRActionHandle_t &actionHandle, bool checkIfActionChanged = false);
	bool GetAnalogActionData(vr::VRActionHandle_t &actionHandle, vr::InputAnalogActionData_t &analogDataOut);
	void ResetPosition();
	void GetPoseData(vr::TrackedDevicePose_t &poseRaw, TrackedDevicePoseData &poseOut);
	void ParseConfigFile();
	void WaitForConfigUpdate();
};