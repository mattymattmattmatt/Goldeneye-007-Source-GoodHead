#pragma once
#include "openvr.h"
#include "vector.h"
#include <chrono>

#define MAX_STR_LEN 256

class Game;
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
	vr::VROverlayHandle_t m_WristWatchHandle = 0;   // health/armor, Bond-watch on the off hand
	vr::VROverlayHandle_t m_WristAmmoHandle = 0;    // ammo, stacked with the watch
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
	float m_SbsWidthMeters = 3.17f;
	float m_SbsDistance = 1.0f;

	// --- Menu input ---------------------------------------------------------
	// Win32 messages to the "Valve001" window are vtable-independent and cannot
	// corrupt the stack. The IInputInternal path depends on a VGUI vtable whose
	// 2007 layout is unconfirmed, so it stays off unless explicitly enabled.
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
	float m_GunGripAngle = 45.0f;

	bool m_ShowWristHUD = true;
	float m_WristLookMaxDistance = 0.6f;
	float m_WristLookMinDot = 0.45f;
	float m_WristWatchWidth = 0.11f;
	float m_WristAmmoWidth = 0.12f;
	Vector m_WristOffset = { -0.15f, 0.10f, 0.04f };      // forward, left, up
	Vector m_WristRotationDeg = { 40.0f, 8.0f, 0.0f };    // roll, pitch, yaw
	Vector m_WristWatchFineOffset = { 0.0f, 0.0f, 0.02f };
	Vector m_WristAmmoFineOffset = { -0.02f, 0.0f, 0.05f };
	vr::VRTextureBounds_t m_WristWatchBounds = { 0.00f, 0.78f, 0.46f, 1.00f };
	vr::VRTextureBounds_t m_WristAmmoBounds = { 0.54f, 0.78f, 1.00f, 1.00f };
	vr::VRTextureBounds_t m_HurtHUDBounds = { 0.00f, 0.78f, 0.50f, 1.00f };
	float m_HurtHUDWidth = 0.55f;
	float m_HurtHUDDistance = 0.85f;
	float m_HurtHUDSeconds = 2.5f;
	int m_HurtHealthThreshold = 80;

	int m_HealthNetvar = -1;
	int m_ArmorNetvar = -1;
	int m_LastHealth = -1;
	std::chrono::steady_clock::time_point m_HurtUntil{};
	bool m_LookingAtWrist = false;

	VR() {};
	VR(Game *game);
	int SetActionManifest(const char *fileName);
	void InstallApplicationManifest(const char *fileName);
	static void MakeVRPath(char *out, size_t outCount, const char *relative);
	void Update();
	void CreateVRTextures();
	void SubmitVRTextures();
	void RepositionOverlays();
	void CreateWristOverlays();
	void HideWristOverlays();
	void UpdateWristHUD();
	void UpdateHurtHUD();
	void ResolvePlayerNetvars();
	int ReadLocalHealth();
	bool IsLookingAtOffhandWatch();
	void SubmitWristOverlay(vr::VROverlayHandle_t handle, vr::TrackedDeviceIndex_t handIndex,
		const Vector &right, const Vector &up, const Vector &backward,
		const Vector &offset, float width, const vr::VRTextureBounds_t &bounds);
	void GetPoses();
	void UpdatePosesAndActions();
	void GetViewParameters();
	void ProcessMenuInput();
	void ProcessInput();
	bool IsMenuMode();
	// Called after DXVK Present returns. Menu-only compositor tick so we never
	// WaitGetPoses/Submit on the same callstack as IDirect3DDevice9::Present.
	void AfterPresent();
	bool ComputeMenuPointer(int &x, int &y);
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
	bool PressedDigitalAction(vr::VRActionHandle_t &actionHandle, bool checkIfActionChanged = false);
	bool GetAnalogActionData(vr::VRActionHandle_t &actionHandle, vr::InputAnalogActionData_t &analogDataOut);
	void ResetPosition();
	void GetPoseData(vr::TrackedDevicePose_t &poseRaw, TrackedDevicePoseData &poseOut);
	void ParseConfigFile();
	void WaitForConfigUpdate();
};