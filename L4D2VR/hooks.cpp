#include "hooks.h"
#include "game.h"
#include "texture.h"
#include "sdk.h"
#include "sdk_server.h"
#include "vr.h"
#include "offsets.h"
#include "weapons.h"
#include "vrnet.h"
#include "d3d9_vr.h"
#include <iostream>
#include <string>
#include <cstddef>
#include <cstring>

// Defined with the first-person aspect notes further down.
static void FindScreenAspectFunction(void *engineClient);

template<typename T>
static bool EnableIfCreated(Hook<T> &hk)
{
	if (!hk.pTarget)
		return false;
	hk.enableHook();
	return true;
}

Hooks::Hooks(Game *game)
{
	if (MH_Initialize() != MH_OK)
	{
		Game::errorMsg("Failed to init MinHook");
	}

	m_Game = game;
	m_VR = m_Game->m_VR;

	m_PushHUDStep = -999;
	m_PushedHud = false;
	m_ViewmodelIndexThisFrame = 0;

	initSourceHooks();

	EnableIfCreated(hkGetRenderTarget);
	EnableIfCreated(hkCalcViewModelView);
	EnableIfCreated(hkFormatViewModelAttachment);
	EnableIfCreated(hkServerFireTerrorBullets);
	EnableIfCreated(hkClientFireTerrorBullets);
	EnableIfCreated(hkProcessUsercmds);
	EnableIfCreated(hkReadUsercmd);
	EnableIfCreated(hkWriteUsercmdDeltaToBuffer);
	EnableIfCreated(hkWriteUsercmd);
	EnableIfCreated(hkAdjustEngineViewport);
	EnableIfCreated(hkViewport);
	EnableIfCreated(hkGetViewport);
	EnableIfCreated(hkCreateMove);
	EnableIfCreated(hkTestMeleeSwingCollisionClient);
	EnableIfCreated(hkTestMeleeSwingCollisionServer);
	EnableIfCreated(hkDoMeleeSwingServer);
	EnableIfCreated(hkStartMeleeSwingServer);
	EnableIfCreated(hkPrimaryAttackServer);
	EnableIfCreated(hkItemPostFrameServer);
	EnableIfCreated(hkGetPrimaryAttackActivity);
	EnableIfCreated(hkEyePosition);
	EnableIfCreated(hkDrawModel);
	EnableIfCreated(hkDrawModelExecute);
	EnableIfCreated(hkDrawModelSetup);
	EnableIfCreated(hkRenderView);
	EnableIfCreated(hkViewRenderRender);
	Game::logMsg("RenderView hook enabled=%d target=%p orig=%p",
	             (int)hkRenderView.isEnabled, hkRenderView.pTarget, hkRenderView.fOriginal);
	Game::logMsg("Render hook enabled=%d target=%p orig=%p",
	             (int)hkViewRenderRender.isEnabled, hkViewRenderRender.pTarget, hkViewRenderRender.fOriginal);
	Game::logMsg("CalcViewModelView hook enabled=%d target=%p orig=%p",
	             (int)hkCalcViewModelView.isEnabled, hkCalcViewModelView.pTarget, hkCalcViewModelView.fOriginal);
	Game::logMsg("DrawModel hook enabled=%d target=%p orig=%p",
	             (int)hkDrawModel.isEnabled, hkDrawModel.pTarget, hkDrawModel.fOriginal);
	EnableIfCreated(hkPushRenderTargetAndViewport);
	EnableIfCreated(hkPopRenderTargetAndViewport);
	EnableIfCreated(hkVgui_Paint);
	EnableIfCreated(hkIsSplitScreen);
	EnableIfCreated(hkPrePushRenderTarget);
	EnableIfCreated(hkWeaponShootPosition);
	EnableIfCreated(hkWeaponShootPositionClient);
}

Hooks::~Hooks()
{
	if (MH_Uninitialize() != MH_OK)
	{
		Game::errorMsg("Failed to uninitialize MinHook");
	}
}


template<typename T>
static void CreateIfFound(Hook<T> &hk, const Offset &off, LPVOID detour)
{
	if (!off.found || !off.address)
		return;
	hk.createHook(reinterpret_cast<LPVOID>(off.address), detour);
}

int Hooks::initSourceHooks()
{
	CreateIfFound(hkRenderView, m_Game->m_Offsets->RenderView, &dRenderView);
	if (m_Game->m_Offsets->RenderView.found)
	{
		Game::logMsg("Hooking CViewRender::RenderView at 0x%X target=%p orig=%p enabled=%d",
		             m_Game->m_Offsets->RenderView.offset,
		             hkRenderView.pTarget, hkRenderView.fOriginal, (int)hkRenderView.isEnabled);
	}
	CreateIfFound(hkViewRenderRender, m_Game->m_Offsets->ViewRenderRender, &dViewRenderRender);
	if (m_Game->m_Offsets->ViewRenderRender.found)
	{
		Game::logMsg("Hooking CViewRender::Render at 0x%X",
		             m_Game->m_Offsets->ViewRenderRender.offset);
	}
	CreateIfFound(hkCalcViewModelView, m_Game->m_Offsets->CalcViewModelView, &dCalcViewModelView);
	if (m_Game->m_Offsets->CalcViewModelView.found)
		Game::logMsg("Hooking C_BaseViewModel::CalcViewModelView at 0x%X",
		             m_Game->m_Offsets->CalcViewModelView.offset);
	CreateIfFound(hkFormatViewModelAttachment, m_Game->m_Offsets->FormatViewModelAttachment, &dFormatViewModelAttachment);
	Game::logMsg("C_BaseViewModel::FormatViewModelAttachment %s (muzzle flash / tracers from the gun in hand)",
	             m_Game->m_Offsets->FormatViewModelAttachment.found ? "hooked" : "NOT FOUND -- effects stay at the head");
	CreateIfFound(hkServerFireTerrorBullets, m_Game->m_Offsets->ServerFireTerrorBullets, &dServerFireTerrorBullets);
	CreateIfFound(hkClientFireTerrorBullets, m_Game->m_Offsets->ClientFireTerrorBullets, &dClientFireTerrorBullets);
	CreateIfFound(hkProcessUsercmds, m_Game->m_Offsets->ProcessUsercmds, &dProcessUsercmds);
	CreateIfFound(hkReadUsercmd, m_Game->m_Offsets->ReadUserCmd, &dReadUsercmd);
	CreateIfFound(hkWriteUsercmdDeltaToBuffer, m_Game->m_Offsets->WriteUsercmdDeltaToBuffer, &dWriteUsercmdDeltaToBuffer);
	CreateIfFound(hkWriteUsercmd, m_Game->m_Offsets->WriteUsercmd, &dWriteUsercmd);
	CreateIfFound(hkAdjustEngineViewport, m_Game->m_Offsets->AdjustEngineViewport, &dAdjustEngineViewport);
	CreateIfFound(hkViewport, m_Game->m_Offsets->Viewport, &dViewport);
	CreateIfFound(hkGetViewport, m_Game->m_Offsets->GetViewport, &dGetViewport);
	CreateIfFound(hkTestMeleeSwingCollisionClient, m_Game->m_Offsets->TestMeleeSwingClient, &dTestMeleeSwingCollisionClient);
	CreateIfFound(hkTestMeleeSwingCollisionServer, m_Game->m_Offsets->TestMeleeSwingServer, &dTestMeleeSwingCollisionServer);
	CreateIfFound(hkDoMeleeSwingServer, m_Game->m_Offsets->DoMeleeSwingServer, &dDoMeleeSwingServer);
	CreateIfFound(hkStartMeleeSwingServer, m_Game->m_Offsets->StartMeleeSwingServer, &dStartMeleeSwingServer);
	CreateIfFound(hkPrimaryAttackServer, m_Game->m_Offsets->PrimaryAttackServer, &dPrimaryAttackServer);
	CreateIfFound(hkItemPostFrameServer, m_Game->m_Offsets->ItemPostFrameServer, &dItemPostFrameServer);
	CreateIfFound(hkGetPrimaryAttackActivity, m_Game->m_Offsets->GetPrimaryAttackActivity, &dGetPrimaryAttackActivity);
	if (m_Game->m_ModelRender)
	{
		void **vt = *reinterpret_cast<void ***>(m_Game->m_ModelRender);
		if (vt && vt[0])
		{
			hkDrawModel.createHook(vt[0], &dDrawModel);
			Game::logMsg("Hooking IModelRender::DrawModel vtable[0]=%p", vt[0]);
		}

		// DrawModelExecute by VTABLE SLOT, not by byte signature.
		//
		// The signature in offsets.h is an L4D2 pattern GE:S's engine does not
		// match ("Optional signature not found" in every boot log), so this hook
		// has never existed -- which is why nothing could ever move the weapon.
		// DrawModelExecute is a virtual on VEngineModel016, so the index is all
		// we need, and this project's IVModelRender stub declares only two
		// entries so the index was unknown.
		//
		// Measured with a naked per-slot counter over ~90s in game:
		//     [17]=85877  [18]=128689  [19]=128291  [21]=49336
		// 18 and 19 are the two hottest and differ by only 398 calls -- the
		// signature of Source's DrawModelSetup / DrawModelExecute pair, where
		// setup runs for every model and execute is skipped for the few that
		// fail setup. So 18 = Setup, 19 = Execute.
		const int slot = m_VR ? m_VR->m_ModelDrawExecuteSlot : 19;
		if (vt && slot > 0 && slot < 64 && vt[slot])
		{
			hkDrawModelExecute.createHook(vt[slot], &dDrawModelExecute);
			Game::logMsg("Hooking IVModelRender::DrawModelExecute vtable[%d]=%p", slot, vt[slot]);
		}

		// DrawModelSetup -- the OTHER half of the hot pair ([18]=128689 vs
		// [19]=128291). This is where the viewmodel must actually be moved:
		// the 00:24 log shows the gun arriving at DrawModelExecute with
		// origin EXACTLY equal to the view origin, and changing it there does
		// nothing because the bone matrices are already built. Setup runs
		// first and takes pInfo by non-const reference, so a change here is
		// picked up by bone setup AND carried into Execute (same object).
		const int setupSlot = m_VR ? m_VR->m_ModelDrawSetupSlot : 18;
		if (m_VR && m_VR->m_WeaponSetupHook && vt && setupSlot > 0 && setupSlot < 64 && vt[setupSlot])
		{
			hkDrawModelSetup.createHook(vt[setupSlot], &dDrawModelSetup);
			Game::logMsg("Hooking IVModelRender::DrawModelSetup vtable[%d]=%p", setupSlot, vt[setupSlot]);
		}
	}
	FindScreenAspectFunction(m_Game->m_EngineClient);
	CreateIfFound(hkPushRenderTargetAndViewport, m_Game->m_Offsets->PushRenderTargetAndViewport, &dPushRenderTargetAndViewport);
	CreateIfFound(hkPopRenderTargetAndViewport, m_Game->m_Offsets->PopRenderTargetAndViewport, &dPopRenderTargetAndViewport);
	CreateIfFound(hkVgui_Paint, m_Game->m_Offsets->VGui_Paint, &dVGui_Paint);
	CreateIfFound(hkIsSplitScreen, m_Game->m_Offsets->IsSplitScreen, &dIsSplitScreen);
	CreateIfFound(hkPrePushRenderTarget, m_Game->m_Offsets->PrePushRenderTarget, &dPrePushRenderTarget);
	CreateIfFound(hkWeaponShootPosition, m_Game->m_Offsets->Weapon_ShootPosition, &dWeaponShootPosition);
	CreateIfFound(hkWeaponShootPositionClient, m_Game->m_Offsets->Weapon_ShootPositionClient, &dWeaponShootPosition);

	if (m_Game->m_Offsets->CreateMove.found)
		Game::logMsg("CreateMove candidate at 0x%X left unhooked", m_Game->m_Offsets->CreateMove.offset);

	return 1;
}


ITexture *__fastcall Hooks::dGetRenderTarget(void *ecx, void *edx)
{
	ITexture *result = hkGetRenderTarget.fOriginal(ecx);
	return result;
}

static void CopyViewSetup(CViewSetup &dst, const CViewSetup &src)
{
	memset(&dst, 0, sizeof(dst));
	memcpy(&dst, &src, offsetof(CViewSetup, _tail));
}

static bool g_inStereoPass = false;
// Counts stereo passes, so per-frame work in per-draw hooks runs once a frame
// rather than once per eye.
static unsigned g_stereoFrame = 0;

// --- First-person pass aspect ------------------------------------------------
//
// GE:S's CViewRender::DrawViewModels (ges-code viewrender.cpp) builds the
// viewmodel pass with
//     viewModelSetup.fov             = view.fovViewmodel;
//     viewModelSetup.m_flAspectRatio = engine->GetScreenAspectRatio();
// -- the WINDOW's 1920/1080 = 1.78 -- while the eyes render at the eye
// frustum's aspect (0.964). Same horizontal FOV, different vertical scale:
// everything in that pass comes out ~1.84x taller and pushed away from the
// centre vertically. Head-locked, that was the unexplained "high gun"; on the
// hand it made the gun stretch and skew as it moved through the view.
//
// Swapping GetScreenAspectRatio for the eye aspect during the stereo pass
// FROZE the game entering a map (22:28 run: stuck in D3D9Initializer::Flush):
// other client code sizes screen-effect render targets from it and rebuilt
// them every frame. So nothing is swapped. Instead the tracked gun's bones are
// pre-squashed along each eye's up axis by eyeAspect / passAspect, and the
// pass's too-tall projection stretches them back to true shape.
//
// The pass aspect comes from the engine's own function, CALLED, never hooked:
// vtable slot 88 in this engine.dll (NOT the 95 of ges-code's header -- slot
// 95 here takes an argument). An E9 jmp to a function that returns an
// override from +0x2C, else width/height; the bytes are verified first.
typedef float(__thiscall *tGetScreenAspectRatio)(void *);
static tGetScreenAspectRatio g_screenAspectFn = nullptr;
static void *g_engineClientForAspect = nullptr;

static void FindScreenAspectFunction(void *engineClient)
{
	if (!engineClient)
		return;
	void **vt = *reinterpret_cast<void ***>(engineClient);
	const int kSlot = 88;
	const unsigned char *fn = static_cast<const unsigned char *>(vt[kSlot]);
	const unsigned char *body = fn;
	if (fn[0] == 0xE9)   // jmp rel32 to the real body
		body = fn + 5 + *reinterpret_cast<const int *>(fn + 1);
	// sub esp,0Ch / mov eax,[imm32] / movss xmm0,[eax+2Ch]
	const bool match = body[0] == 0x83 && body[1] == 0xEC && body[2] == 0x0C && body[3] == 0xA1 &&
	                   body[8] == 0xF3 && body[9] == 0x0F && body[10] == 0x10 && body[11] == 0x40 && body[12] == 0x2C;
	if (!match)
	{
		Game::logMsg("GetScreenAspectRatio NOT recognised at vtable[%d] -- using the window size instead", kSlot);
		return;
	}
	g_screenAspectFn = reinterpret_cast<tGetScreenAspectRatio>(vt[kSlot]);
	g_engineClientForAspect = engineClient;
	Game::logMsg("GetScreenAspectRatio verified at vtable[%d] (read only, not hooked)", kSlot);
}

// The aspect the first-person pass will use this frame.
static float FirstPersonPassAspect()
{
	if (g_screenAspectFn)
	{
		const float a = g_screenAspectFn(g_engineClientForAspect);
		if (a > 0.1f && a < 10.0f)
			return a;
	}
	int w = 0, h = 0;
	if (Hooks::m_Game && Hooks::m_Game->m_EngineClient)
		Hooks::m_Game->m_EngineClient->GetScreenSize(w, h);
	return (w > 0 && h > 0) ? (float)w / (float)h : 16.0f / 9.0f;
}

// The eye being rendered right now (set around each eye's RenderView), for the
// per-eye squash: it is centred on that eye and follows its up axis.
static bool g_eyeValid = false;
static Vector g_eyeOrigin;
static QAngle g_eyeAngles;
static float g_squash = 1.0f;   // eyeAspect / passAspect for this frame
// The game's own view this frame (the flat-screen camera): its FOV and its
// viewmodel FOV. GE:S's attachment FOV correction works from these.
static float g_gameFov = 0.0f, g_gameFovVM = 0.0f;

// Counts how many times the weapon reposition actually executed, so the
// motion trace can say whether the write is even happening.
static volatile long g_execMoves = 0;
long GESVR_ExecMoveCount() { return g_execMoves; }
long GESVR_RenderOriginCalls();

// ===========================================================================
// IVModelRender vtable probe
// ---------------------------------------------------------------------------
// We have no working hook on the model draw path. vtable[0] ("DrawModel") is
// never called (DRAWMODEL alive: 0 hits across 9720 stereo passes), because
// modern Source draws through DrawModelExecute instead -- and the byte
// signature for that is an L4D2 pattern the GE:S engine does not match.
// DrawModelExecute is a virtual on VEngineModel016, which we DO resolve, so the
// only unknown is its vtable index, and this project's IVModelRender stub only
// declares two entries.
//
// So measure it. Each stub is __declspec(naked): it increments a counter and
// jumps straight to the original, touching no registers and no stack, which
// makes it safe on a slot of ANY signature. DrawModelExecute will stand out by
// orders of magnitude -- hundreds of calls per frame against near-zero.
// ===========================================================================

// ===========================================================================
// Slot 18 argument probe
// ---------------------------------------------------------------------------
// Hooking slot 18 with a guessed DrawModelSetup signature crashed on the first
// stereo pass: the argument list does not match, so calling the original with
// our stack frame corrupts it instantly. Rather than guess again, capture what
// slot 18 is ACTUALLY called with.
//
// The stub is __declspec(naked): it copies ecx and the first four stack
// arguments into globals and jumps straight through. It changes no register and
// no stack slot, so it is safe regardless of the real signature or arity.
// ===========================================================================
static IModelInfo *m_GameStaticModelInfo = nullptr;


// ===========================================================================
// Viewmodel renderable patch
// ---------------------------------------------------------------------------
// Writing pInfo.origin does NOT move a viewmodel. Proven twice: the write fires
// at DrawModelExecute (EXEC MOVE) and at DrawModelSetup (VM MOVED), with a real
// model name and a 27-unit displacement, and the gun does not budge.
// pInfo.origin feeds lighting and culling.
//
// What actually places an animated model is C_BaseAnimating::SetupBones, which
// builds the root transform from the renderable's GetRenderOrigin() and
// GetRenderAngles() -- IClientRenderable vtable slots 1 and 2. ModelRenderInfo_t
// hands us pRenderable, so we can point those at our own values.
//
// The vtable belongs to the viewmodel class, so only viewmodels are affected,
// and we only patch after confirming the model name is a v_ model.
// ===========================================================================
// Read-only check of the safety claim I got wrong last round: is the
// viewmodel's renderable vtable actually distinct from a world prop's? If they
// share one, patching slots 1/2 would corrupt every entity in the map and that
// approach is dead. If the viewmodel's is unique, the patch is as narrow as I
// claimed. Answer this before touching anything.
namespace VmVtable
{
	static void *g_vmVt = nullptr;
	static void *g_propVt = nullptr;
	static char  g_vmModel[96] = {};
	static char  g_propModel[96] = {};
	static bool  g_reported = false;

	static void Describe(void *addr, char *out, size_t outSz)
	{
		MEMORY_BASIC_INFORMATION mbi{};
		if (!addr || !VirtualQuery(addr, &mbi, sizeof(mbi)) || !mbi.AllocationBase)
		{
			_snprintf_s(out, outSz, _TRUNCATE, "%p <unmapped>", addr);
			return;
		}
		char path[MAX_PATH] = {};
		GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH);
		const char *base = strrchr(path, '\\');
		base = base ? base + 1 : path;
		_snprintf_s(out, outSz, _TRUNCATE, "%s+0x%X", base,
		            (unsigned)((DWORD_PTR)addr - (DWORD_PTR)mbi.AllocationBase));
	}

	static void Note(void *renderable, const char *model, bool isViewmodel)
	{
		if (g_reported || !renderable)
			return;
		void **vt = *reinterpret_cast<void ***>(renderable);
		if (!vt)
			return;
		if (isViewmodel && !g_vmVt)
		{
			g_vmVt = vt;
			_snprintf_s(g_vmModel, sizeof(g_vmModel), _TRUNCATE, "%s", model ? model : "?");
		}
		else if (!isViewmodel && !g_propVt)
		{
			g_propVt = vt;
			_snprintf_s(g_propModel, sizeof(g_propModel), _TRUNCATE, "%s", model ? model : "?");
		}

		if (g_vmVt && g_propVt)
		{
			g_reported = true;
			char s0[160], s1[160], s2[160], s3[160];
			void **vmvt = reinterpret_cast<void **>(g_vmVt);
			Describe(vmvt[0], s0, sizeof(s0));
			Describe(vmvt[1], s1, sizeof(s1));
			Describe(vmvt[2], s2, sizeof(s2));
			Describe(vmvt[3], s3, sizeof(s3));
			Game::logMsg("VMVT viewmodel vt=%p (%s)", g_vmVt, g_vmModel);
			Game::logMsg("VMVT prop      vt=%p (%s)", g_propVt, g_propModel);
			Game::logMsg("VMVT SHARED=%s  -> patching is %s",
			             (g_vmVt == g_propVt) ? "YES" : "no",
			             (g_vmVt == g_propVt) ? "UNSAFE, would hit every entity"
			                                  : "narrow, viewmodel class only");
			Game::logMsg("VMVT slots: [0]=%s [1]=%s [2]=%s [3]=%s", s0, s1, s2, s3);
		}
	}
}

namespace VmRenderable
{
	static Vector g_origin = { 0, 0, 0 };
	static QAngle g_angles = { 0, 0, 0 };
	static bool   g_havePose = false;
	static bool   g_patched = false;
	static void  *g_origGetOrigin = nullptr;
	static void  *g_origGetAngles = nullptr;

	// const Vector& GetRenderOrigin() -- a const-ref return is a pointer return.
	// __thiscall with no args and __fastcall(ecx, edx) agree on both registers
	// and stack cleanup (ret 0), so this is a safe direct vtable replacement.
	static volatile long g_originCalls = 0;
	static volatile long g_anglesCalls = 0;

	static const Vector *__fastcall GetRenderOrigin(void *ecx, void *edx)
	{
		g_originCalls = g_originCalls + 1;
		if (g_havePose)
			return &g_origin;
		typedef const Vector *(__fastcall *fn)(void *, void *);
		return reinterpret_cast<fn>(g_origGetOrigin)(ecx, edx);
	}

	static const QAngle *__fastcall GetRenderAngles(void *ecx, void *edx)
	{
		g_anglesCalls = g_anglesCalls + 1;
		if (g_havePose)
			return &g_angles;
		typedef const QAngle *(__fastcall *fn)(void *, void *);
		return reinterpret_cast<fn>(g_origGetAngles)(ecx, edx);
	}

	static void Patch(void *renderable)
	{
		if (g_patched || !renderable)
			return;
		void **vt = *reinterpret_cast<void ***>(renderable);
		if (!vt || !vt[1] || !vt[2])
			return;
		DWORD old = 0;
		if (!VirtualProtect(&vt[1], sizeof(void *) * 2, PAGE_READWRITE, &old))
		{
			Game::logMsg("VmRenderable: VirtualProtect failed (%lu)", GetLastError());
			return;
		}
		g_origGetOrigin = vt[1];
		g_origGetAngles = vt[2];
		vt[1] = (void *)&GetRenderOrigin;
		vt[2] = (void *)&GetRenderAngles;
		VirtualProtect(&vt[1], sizeof(void *) * 2, old, &old);
		g_patched = true;
		Game::logMsg("VmRenderable: patched GetRenderOrigin=%p GetRenderAngles=%p",
		             g_origGetOrigin, g_origGetAngles);
	}
}

namespace SetupProbe
{
	static void *g_orig = nullptr;
	static volatile DWORD g_ecx = 0;
	static volatile DWORD g_a1 = 0, g_a2 = 0, g_a3 = 0, g_a4 = 0;
	static volatile long  g_hits = 0;
	static bool g_installed = false;

	static char g_name[128] = {};
	static volatile long g_named = 0;

	// Resolve WHILE the frame is alive. The previous version stored raw
	// stack pointers and dereferenced them seconds later, by which time the
	// frame was long gone -- which is why every argument read as garbage.
	static void __cdecl Capture(DWORD a1, DWORD a2, DWORD a3)
	{
		if (g_named > 40 || !m_GameStaticModelInfo)
			return;
		const DWORD cand[3] = { a1, a2, a3 };
		for (int i = 0; i < 3; ++i)
		{
			if (cand[i] < 0x10000)
				continue;
			__try
			{
				const ModelRenderInfo_t *mi = reinterpret_cast<const ModelRenderInfo_t *>(cand[i]);
				if (!mi->pModel)
					continue;
				const char *n = m_GameStaticModelInfo->GetModelName(mi->pModel);
				if (n && n[0] == 'm' && n[1] == 'o')
				{
					_snprintf_s(g_name, sizeof(g_name), _TRUNCATE, "arg%d -> %s", i + 1, n);
					g_named = g_named + 1;
					return;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
	}

	static __declspec(naked) void Stub()
	{
		__asm {
			pushad
			pushfd
			mov eax, [esp + 48]
			push eax
			mov eax, [esp + 48]
			push eax
			mov eax, [esp + 48]
			push eax
			call Capture
			add esp, 12
			popfd
			popad
			lock inc g_hits
			jmp dword ptr [g_orig]
		}
	}

	static void Install(void *iface, int slot)
	{
		if (g_installed || !iface || slot <= 0 || slot >= 64)
			return;
		void **vt = *reinterpret_cast<void ***>(iface);
		if (!vt || !vt[slot])
			return;
		DWORD old = 0;
		if (!VirtualProtect(&vt[slot], sizeof(void *), PAGE_READWRITE, &old))
			return;
		g_orig = vt[slot];
		vt[slot] = (void *)&Stub;
		VirtualProtect(&vt[slot], sizeof(void *), old, &old);
		g_installed = true;
		Game::logMsg("SetupProbe: capturing args of vtable[%d]=%p", slot, g_orig);
	}


	static void Report()
	{
		if (!g_installed)
			return;
		static DWORD s_last = 0;
		const DWORD now = GetTickCount();
		if (s_last != 0 && (now - s_last) < 4000)
			return;
		s_last = now;
		Game::logMsg("SETUPARGS hits=%ld ecx=%08X a1=%08X a2=%08X a3=%08X a4=%08X",
		             g_hits, g_ecx, g_a1, g_a2, g_a3, g_a4);
		Game::logMsg("SETUPARGS resolved: %s", g_name[0] ? g_name : "<none of arg1..3 is a ModelRenderInfo_t>");
	}
}

namespace VtProbe
{
	static const int kSlots = 28;
	static void        *g_orig[kSlots] = {};
	static volatile long g_hits[kSlots] = {};
	static bool          g_installed = false;

#define GESVR_VTSTUB(N) \
	static __declspec(naked) void Stub##N() \
	{ \
		__asm { lock inc dword ptr [g_hits + (N * 4)] } \
		__asm { jmp dword ptr [g_orig + (N * 4)] } \
	}

	GESVR_VTSTUB(0)  GESVR_VTSTUB(1)  GESVR_VTSTUB(2)  GESVR_VTSTUB(3)
	GESVR_VTSTUB(4)  GESVR_VTSTUB(5)  GESVR_VTSTUB(6)  GESVR_VTSTUB(7)
	GESVR_VTSTUB(8)  GESVR_VTSTUB(9)  GESVR_VTSTUB(10) GESVR_VTSTUB(11)
	GESVR_VTSTUB(12) GESVR_VTSTUB(13) GESVR_VTSTUB(14) GESVR_VTSTUB(15)
	GESVR_VTSTUB(16) GESVR_VTSTUB(17) GESVR_VTSTUB(18) GESVR_VTSTUB(19)
	GESVR_VTSTUB(20) GESVR_VTSTUB(21) GESVR_VTSTUB(22) GESVR_VTSTUB(23)
	GESVR_VTSTUB(24) GESVR_VTSTUB(25) GESVR_VTSTUB(26) GESVR_VTSTUB(27)
#undef GESVR_VTSTUB

	static void *const kStubs[kSlots] = {
		Stub0,  Stub1,  Stub2,  Stub3,  Stub4,  Stub5,  Stub6,  Stub7,
		Stub8,  Stub9,  Stub10, Stub11, Stub12, Stub13, Stub14, Stub15,
		Stub16, Stub17, Stub18, Stub19, Stub20, Stub21, Stub22, Stub23,
		Stub24, Stub25, Stub26, Stub27
	};

	static void Install(void *iface)
	{
		if (g_installed || !iface)
			return;
		void **vt = *reinterpret_cast<void ***>(iface);
		if (!vt)
			return;
		DWORD old = 0;
		if (!VirtualProtect(vt, kSlots * sizeof(void *), PAGE_READWRITE, &old))
		{
			Game::logMsg("VtProbe: VirtualProtect failed (%lu)", GetLastError());
			return;
		}
		for (int i = 0; i < kSlots; ++i)
		{
			g_orig[i] = vt[i];
			vt[i] = kStubs[i];
		}
		VirtualProtect(vt, kSlots * sizeof(void *), old, &old);
		g_installed = true;
		Game::logMsg("VtProbe: counting %d IVModelRender vtable slots", kSlots);
	}

	static void Report()
	{
		if (!g_installed)
			return;
		static DWORD s_last = 0;
		const DWORD now = GetTickCount();
		if (s_last != 0 && (now - s_last) < 5000)
			return;
		s_last = now;
		char line[512];
		int off = _snprintf_s(line, sizeof(line), _TRUNCATE, "VTHITS");
		for (int i = 0; i < kSlots; ++i)
		{
			if (g_hits[i] == 0)
				continue;
			off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " [%d]=%ld", i, g_hits[i]);
			if (off > 420)
				break;
		}
		Game::logMsg("%s", line);
	}
}



void __fastcall Hooks::dViewRenderRender(void *ecx, void *edx, void *rect)
{
	static int s_calls = 0;
	if (s_calls < 8)
	{
		void **vt = ecx ? *reinterpret_cast<void ***>(ecx) : nullptr;
		Game::logMsg("CViewRender::Render #%d this=%p rect=%p vtable[6]=%p",
		             s_calls, ecx, rect, vt ? vt[6] : nullptr);
		++s_calls;
	}
	// CalcViewModelView runs inside Render(), before RenderView. Refresh
	// controller poses first so motion guns are not a frame behind / skipped.
	// Title/main menu: VR::Update owns WaitGetPoses+Submit — do not double-block.
	if (m_VR && m_VR->m_IsInitialized)
	{
		const bool menuOnly = g_Game && g_Game->IsGameUIVisible() && !g_Game->IsInMap();
		if (!menuOnly)
		{
			m_VR->UpdatePosesAndActions();
			m_VR->GetPoses();
		}
		m_ViewmodelIndexThisFrame = 0;
	}
	if (hkViewRenderRender.fOriginal)
		hkViewRenderRender.fOriginal(ecx, rect);
}

extern void GESVR_NoteStereoPass();

// Source's CViewRender clear flags.
enum GESVRClearFlags
{
	VIEW_CLEAR_COLOR       = 0x01,
	VIEW_CLEAR_DEPTH       = 0x02,
	VIEW_CLEAR_FULL_TARGET = 0x04,
	VIEW_NO_DRAW           = 0x08,
};

// Source's RenderView 'what to draw' bits.
enum GESVRRenderViewInfo
{
	RENDERVIEW_DRAWVIEWMODEL = 0x01,
	RENDERVIEW_DRAWHUD       = 0x02,
};

void __fastcall Hooks::dRenderView(void *ecx, void *edx, CViewSetup &setup, int nClearFlags, int whatToDraw)
{
	// Install the vtable probe lazily -- it lives below the hook-setup code,
	// and by the time we render, m_ModelRender is definitely live.
	if (m_Game && m_Game->m_ModelRender)
	{
		m_GameStaticModelInfo = m_Game->m_ModelInfo;
		if (m_VR && m_VR->m_SetupProbe)
		{
			SetupProbe::Install(m_Game->m_ModelRender, m_VR->m_ModelDrawSetupSlot);
			SetupProbe::Report();
		}
		if (m_VR && m_VR->m_VtableProbe)
		{
			VtProbe::Install(m_Game->m_ModelRender);
			VtProbe::Report();
		}
	}
	static int s_calls = 0;
	const int call = s_calls++;
	if (call < 8)
	{
		Game::logMsg("RenderView #%d size=%dx%d fov=%.1f origin=(%.1f,%.1f,%.1f)",
		             call, setup.width, setup.height, setup.fov,
		             setup.origin.x, setup.origin.y, setup.origin.z);
	}

	if (!hkRenderView.fOriginal)
		return;

	// Main menu only (not on a map): one 2D frame for the floating overlay.
	// Character select / pause still have a 3D world — those must stay stereo.
	if (g_Game && g_Game->IsGameUIVisible() && !g_Game->IsInMap())
	{
		hkRenderView.fOriginal(ecx, setup, nClearFlags, whatToDraw);
		return;
	}

	// Nested RenderView (water/mirrors) or tiny views: pass through.
	if (g_inStereoPass || setup.width < 640 || !m_VR || !m_VR->m_IsInitialized || !g_D3DVR9)
	{
		hkRenderView.fOriginal(ecx, setup, nClearFlags, whatToDraw);
		return;
	}

	g_inStereoPass = true;
	++g_stereoFrame;
	g_squash = 1.0f;
	if (m_VR && (m_VR->m_TrackedWeapon || m_VR->m_FixViewmodelAspect) && m_VR->m_Aspect > 0.1f)
	{
		const float pass = FirstPersonPassAspect();
		g_squash = m_VR->m_Aspect / pass;
		static int s_logged = 0;
		if (s_logged < 3)
		{
			Game::logMsg("FIRST-PERSON PASS: eye aspect %.3f, pass aspect %.3f -> squash %.3f",
			             m_VR->m_Aspect, pass, g_squash);
			++s_logged;
		}
	}

	// VGUI is kept out of the eyes by the g_inStereoPass guard in dVGui_Paint.
	const bool overlayMenu = m_VR && m_VR->IsMenuMode();

	// Separate counter: s_calls above counts every RenderView, menu frames
	// included, so it is in the hundreds before a map ever loads. Bracketing
	// each half of the stereo pass means a hang inside the engine's own
	// RenderView shows up as a "begin" with no matching "L ok" / "R ok".
	static int s_stereoPass = 0;
	const int pass = s_stereoPass++;
	const bool traceStereo = (pass < 10);
	GESVR_NoteStereoPass();
	if (traceStereo)
		Game::logMsg("stereo pass #%d BEGIN size=%dx%d fov=%.1f", pass,
		             setup.width, setup.height, setup.fov);

	// Poses already refreshed in CViewRender::Render this frame. A second
	// WaitGetPoses here is a full compositor period and was the in-game hitch.
	m_ViewmodelIndexThisFrame = 0;
	m_VR->m_SetupOrigin = setup.origin;
	g_gameFov = setup.fov;
	g_gameFovVM = setup.fovViewmodel;

	CViewSetup leftEyeView;
	CViewSetup rightEyeView;
	CopyViewSetup(leftEyeView, setup);
	CopyViewSetup(rightEyeView, setup);
	m_VR->ApplyHeadAndIpd(leftEyeView, rightEyeView, setup);
	m_VR->UpdateGunAim(leftEyeView, rightEyeView);

	IMatRenderContext *rndrContext = nullptr;
	if (m_VR->m_UseEyeRenderTargets && m_Game && m_Game->m_MaterialSystem)
	{
		if (!m_VR->m_CreatedVRTextures)
			m_VR->CreateVRTextures();
		if (m_VR->m_CreatedVRTextures && m_VR->m_LeftEyeTexture && m_VR->m_RightEyeTexture
		    && m_VR->m_EyeRTWidth > 0 && m_VR->m_EyeRTHeight > 0)
		{
			rndrContext = m_Game->m_MaterialSystem->GetRenderContext();
			leftEyeView.x = 0;  leftEyeView.y = 0;
			rightEyeView.x = 0; rightEyeView.y = 0;
			leftEyeView.width  = rightEyeView.width  = (int)m_VR->m_EyeRTWidth;
			leftEyeView.height = rightEyeView.height = (int)m_VR->m_EyeRTHeight;
		}
	}
	if (traceStereo)
		Game::logMsg("stereo pass #%d eyeRT=%d size=%dx%d", pass,
		             (int)(rndrContext != nullptr), leftEyeView.width, leftEyeView.height);

	// Each eye target must be cleared in full.
	//
	// These are dedicated targets now, not the shared backbuffer, so nothing
	// else clears them. Without a colour clear, anything the scene does not
	// cover shows whatever was in the texture -- the white blobs. Without a
	// depth clear the second eye inherits the first eye's depth and rejects
	// geometry that should be visible. FULL_TARGET because the target is
	// larger than the viewport the engine would otherwise clear.
	const int eyeClear = rndrContext
		? (nClearFlags | VIEW_CLEAR_COLOR | VIEW_CLEAR_DEPTH | VIEW_CLEAR_FULL_TARGET)
		: nClearFlags;

	// Bind through null so the bind is never treated as redundant.
	//
	// Setting a render target resets the viewport to that target's size. With a
	// shared target the second eye passes the SAME pointer, so the material
	// system skips the call as a no-op -- and with it the viewport reset. The
	// second pass then drew at the backbuffer's 1920x1080 inside a 1806x1873
	// target: full width, top 58% only, black underneath, which is exactly what
	// the right eye showed. Going via null forces a real rebind both times.
	if (rndrContext)
	{
		rndrContext->SetRenderTarget(nullptr);
		rndrContext->SetRenderTarget(m_VR->m_LeftEyeTexture);
	}
	// Keep the 2D HUD out of the eye targets.
	//
	// The HUD is laid out in WINDOW pixels, and the eye targets are a different
	// size and aspect, so drawing it there puts it up and to the left and
	// scatters artefacts. It only lands in one eye because the engine draws it
	// once per frame, which is why the right eye was the damaged one.
	//
	// The viewmodel bit stays: the weapon belongs in the eyes.
	// Strip 2D VGUI from the stereo eyes. Character select is painted into
	// the 3D view AND captured for the floating overlay -- the eye copy is
	// frustum-cropped into the giant stretched menu behind the good panel.
	const int eyeDraw = (rndrContext && (m_VR->m_EyeHudPass || overlayMenu))
		? (whatToDraw & ~RENDERVIEW_DRAWHUD)
		: whatToDraw;

	if (traceStereo) Game::logMsg("stereo pass #%d L render... view=%dx%d rt=%p clear=0x%X draw=0x%X",
	                              pass, leftEyeView.width, leftEyeView.height,
	                              (void*)m_VR->m_LeftEyeTexture, eyeClear, eyeDraw);
	g_eyeOrigin = leftEyeView.origin; g_eyeAngles = leftEyeView.angles; g_eyeValid = true;
	hkRenderView.fOriginal(ecx, leftEyeView, eyeClear, eyeDraw);
	if (traceStereo) Game::logMsg("stereo pass #%d L rendered, capturing", pass);
	// Force the material system to submit its queued work before we capture.
	//
	// Source BUFFERS draw calls; our capture bypasses the material system and
	// talks to the D3D device directly. Without this the capture can read a
	// target the engine has not finished drawing into -- which is a partially
	// black, flickering eye, and is the most likely reason the right eye came
	// back black while the left (whose work the following pass flushed) did not.
	if (rndrContext) rndrContext->Flush(true);
	HRESULT hl = g_D3DVR9->CaptureCurrentRT(0, &m_VR->m_VKLeftEye);
	if (traceStereo) Game::logMsg("stereo pass #%d L ok hr=0x%08X", pass, (unsigned)hl);

	if (rndrContext)
	{
		rndrContext->SetRenderTarget(nullptr);
		rndrContext->SetRenderTarget(m_VR->m_RightEyeTexture);
	}
	if (traceStereo) Game::logMsg("stereo pass #%d R render... view=%dx%d rt=%p",
	                              pass, rightEyeView.width, rightEyeView.height,
	                              (void*)m_VR->m_RightEyeTexture);
	g_eyeOrigin = rightEyeView.origin; g_eyeAngles = rightEyeView.angles; g_eyeValid = true;
	hkRenderView.fOriginal(ecx, rightEyeView, eyeClear, eyeDraw);
	g_eyeValid = false;
	if (traceStereo) Game::logMsg("stereo pass #%d R rendered, capturing", pass);
	if (rndrContext) rndrContext->Flush(true);
	HRESULT hr = g_D3DVR9->CaptureCurrentRT(1, &m_VR->m_VKRightEye);
	if (traceStereo) Game::logMsg("stereo pass #%d R ok hr=0x%08X", pass, (unsigned)hr);

	m_VR->m_RenderedNewFrame = SUCCEEDED(hl) && SUCCEEDED(hr)
		&& m_VR->m_VKLeftEye.m_VRTexture.handle
		&& m_VR->m_VKRightEye.m_VRTexture.handle;

	if (traceStereo)
		Game::logMsg("RenderView stereo capture L=0x%08X R=0x%08X ok=%d Lorig=(%.1f,%.1f,%.1f) Rorig=(%.1f,%.1f,%.1f)",
		             (unsigned)hl, (unsigned)hr, (int)m_VR->m_RenderedNewFrame,
		             leftEyeView.origin.x, leftEyeView.origin.y, leftEyeView.origin.z,
		             rightEyeView.origin.x, rightEyeView.origin.y, rightEyeView.origin.z);

	// Hand the backbuffer back, or the HUD/menu would draw into the eye
	// texture and the desktop window would go black.
	if (rndrContext) rndrContext->SetRenderTarget(nullptr);

	// Now draw the 2D layer to the backbuffer, at window size, where it belongs.
	// VIEW_NO_DRAW asks the engine to skip the 3D view and do only the 2D pass;
	// if this engine ignores that bit the cost is a third scene render, so it is
	// behind EyeHudPass. The menu overlay captures the backbuffer, so the in-map
	// menu depends on this drawing somewhere.
	if (rndrContext && (overlayMenu || (m_VR->m_EyeHudPass && (whatToDraw & RENDERVIEW_DRAWHUD))))
	{
		if (traceStereo) Game::logMsg("stereo pass #%d HUD pass to backbuffer", pass);
		hkRenderView.fOriginal(ecx, setup, VIEW_NO_DRAW, whatToDraw | RENDERVIEW_DRAWHUD);
	}
	g_inStereoPass = false;
}

bool __fastcall Hooks::dCreateMove(void *ecx, void *edx, float flInputSampleTime, CUserCmd *cmd)
{
	if (!cmd || !cmd->command_number)
		return hkCreateMove.fOriginal ? hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd) : false;

	if (m_VR->m_IsVREnabled)
	{
		// While firing, aim with the motion controller so bullets match the gun.
		// Otherwise keep HMD yaw so locomotion follows your head.
		const bool attacking = (cmd->buttons & IN_ATTACK) != 0;
		if (m_VR->m_MotionControls && attacking)
		{
			QAngle gun = m_VR->GetRightControllerAbsAngle();
			cmd->viewangles = gun;
		}

		if (m_VR->m_RoomscaleActive)
		{
			Vector setupOriginToHMD = m_VR->m_SetupOriginToHMD;
			setupOriginToHMD.z = 0;
			float distance = VectorLength(setupOriginToHMD);
			if (distance > 1)
			{
				float forwardSpeed = DotProduct2D(setupOriginToHMD, m_VR->m_HmdForward);
				float sideSpeed = DotProduct2D(setupOriginToHMD, m_VR->m_HmdRight);
				cmd->forwardmove += distance * forwardSpeed;
				cmd->sidemove += distance * sideSpeed;
			}
		}
	}

	if (hkCreateMove.fOriginal)
		hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);
	return false;
}

void __fastcall Hooks::dEndFrame(void *ecx, void *edx)
{
	return hkEndFrame.fOriginal(ecx);
}

void __fastcall Hooks::dCalcViewModelView(void *ecx, void *edx, void *owner, const Vector &eyePosition, const QAngle &eyeAngles)
{
	Vector vecNewOrigin = eyePosition;
	QAngle vecNewAngles = eyeAngles;

	// The value we treat as a world eye position has magnitude ~1.0, which no
	// world position in a Source map can have. Either this is not
	// CalcViewModelView, or the argument list is shifted. Log BOTH candidate
	// arguments and the CALL RATE: per-frame says we are on the right
	// function with a wrong signature; ~0.3/s says we are hooked elsewhere.
	{
		static long s_n = 0;
		static DWORD s_first = 0;
		const DWORD now = GetTickCount();
		if (s_first == 0) s_first = now;
		++s_n;
		if (s_n <= 6 || (s_n % 600) == 0)
		{
			const float secs = (now - s_first) / 1000.0f;
			Game::logMsg("VMHOOK #%ld rate=%.1f/s stereo=%d owner=%p argA=(%.2f,%.2f,%.2f) |A|=%.2f argB=(%.2f,%.2f,%.2f)",
			             s_n, secs > 0.1f ? (s_n / secs) : 0.0f, (int)g_inStereoPass, owner,
			             eyePosition.x, eyePosition.y, eyePosition.z,
			             VectorLength(eyePosition),
			             eyeAngles.x, eyeAngles.y, eyeAngles.z);
		}
	}

	// Called from CalcView, before RenderView, so g_inStereoPass is usually 0.
	// GE:S passes a tiny local offset, not a world origin.
	if (m_VR && m_VR->m_IsVREnabled && m_VR->m_MotionControls && hkCalcViewModelView.fOriginal)
	{
		const float mag = VectorLength(eyePosition);
		Vector delta = eyePosition - m_VR->m_SetupOrigin;
		const float dist = VectorLength(delta);
		if (mag < 16.0f || dist < 96.0f)
		{
			const int vm = m_ViewmodelIndexThisFrame++;
			if (vm == 0)
			{
				vecNewOrigin = m_VR->GetRecommendedViewmodelAbsPos();
				vecNewAngles = m_VR->GetRecommendedViewmodelAbsAngle();
			}
			else
			{
				vecNewOrigin = m_VR->GetLeftControllerAbsPos();
				vecNewAngles = m_VR->GetLeftControllerAbsAngle();
			}
			static int s_applied = 0;
			if (s_applied < 8)
			{
				Game::logMsg("viewmodel motion #%d mag=%.1f dist=%.1f -> (%.0f,%.0f,%.0f)",
				             vm, mag, dist, vecNewOrigin.x, vecNewOrigin.y, vecNewOrigin.z);
				++s_applied;
			}
		}
	}

	if (hkCalcViewModelView.fOriginal)
		hkCalcViewModelView.fOriginal(ecx, owner, vecNewOrigin, vecNewAngles);
}

Vector *__fastcall Hooks::dWeaponShootPosition(void *ecx, void *edx, Vector *out)
{
	Vector *result = nullptr;
	if (hkWeaponShootPosition.fOriginal)
		result = hkWeaponShootPosition.fOriginal(ecx, out);
	else if (hkWeaponShootPositionClient.fOriginal)
		result = hkWeaponShootPositionClient.fOriginal(ecx, out);

	if (!out)
		return result;

	const int local = m_Game->m_EngineClient ? m_Game->m_EngineClient->GetLocalPlayer() : 0;
	const int slot = VRNet::SlotOk(m_Game->m_CurrentUsercmdID) ? m_Game->m_CurrentUsercmdID : local;

	if (VRNet::SlotOk(slot) && m_Game->m_PlayersVRInfo[slot].isUsingVR)
	{
		*out = m_Game->m_PlayersVRInfo[slot].controllerPos;
		return out;
	}

	if (m_VR && m_VR->m_IsVREnabled && slot == local)
	{
		*out = m_VR->GetRightControllerAbsPos();
		return out;
	}
	return result;
}

int Hooks::dServerFireTerrorBullets(int playerId, const Vector &vecOrigin, const QAngle &vecAngles, int a4, int a5, int a6, float a7)
{
	Vector vecNewOrigin = vecOrigin;
	QAngle vecNewAngles = vecAngles;

	// Server host
	if (m_VR->m_IsVREnabled && playerId == m_Game->m_EngineClient->GetLocalPlayer())
	{
		vecNewOrigin = m_VR->GetRightControllerAbsPos();
		vecNewAngles = m_VR->GetRightControllerAbsAngle();
	}
	// Clients
	else if (m_Game->m_PlayersVRInfo[playerId].isUsingVR)
	{
		vecNewOrigin = m_Game->m_PlayersVRInfo[playerId].controllerPos;
		vecNewAngles = m_Game->m_PlayersVRInfo[playerId].controllerAngle;
	}

	return hkServerFireTerrorBullets.fOriginal(playerId, vecNewOrigin, vecNewAngles, a4, a5, a6, a7);
}

int Hooks::dClientFireTerrorBullets(int playerId, const Vector &vecOrigin, const QAngle &vecAngles, int a4, int a5, int a6, float a7)
{
	Vector vecNewOrigin = vecOrigin;
	QAngle vecNewAngles = vecAngles;
	
	if (m_VR->m_IsVREnabled && playerId == m_Game->m_EngineClient->GetLocalPlayer())
	{
		vecNewOrigin = m_VR->GetRightControllerAbsPos();
		vecNewAngles = m_VR->GetRightControllerAbsAngle();
	}

	return hkClientFireTerrorBullets.fOriginal(playerId, vecNewOrigin, vecNewAngles, a4, a5, a6, a7);
}


float __fastcall Hooks::dProcessUsercmds(void *ecx, void *edx, edict_t *player, void *buf, int numcmds, int totalcmds, int dropped_packets, bool ignore, bool paused)
{
	int index = 0;
	if (m_Game->m_Offsets->CBaseEntity_entindex.found && player)
	{
		typedef int(__thiscall *tEntindex)(void *thisptr);
		static tEntindex oEntindex = (tEntindex)(m_Game->m_Offsets->CBaseEntity_entindex.address);

		IServerUnknown *pUnknown = player->m_pUnk;
		if (pUnknown)
		{
			Server_BaseEntity *pPlayer = (Server_BaseEntity *)pUnknown->GetBaseEntity();
			if (pPlayer && oEntindex)
				index = oEntindex(pPlayer);
		}
	}

	m_Game->m_CurrentUsercmdID = VRNet::SlotOk(index) ? index : 0;

	float result = hkProcessUsercmds.fOriginal(ecx, player, buf, numcmds, totalcmds, dropped_packets, ignore, paused);

	if (!VRNet::SlotOk(index))
		return result;

	m_Game->m_PlayersVRInfo[index].prevControllerAngle = m_Game->m_PlayersVRInfo[index].controllerAngle;
	return result;
}

int Hooks::dReadUsercmd(void *buf, CUserCmd *move, CUserCmd *from)
{
	hkReadUsercmd.fOriginal(buf, move, from);

	const int i = m_Game->m_CurrentUsercmdID;
	if (!VRNet::SlotOk(i) || !move)
		return 1;

	if (!m_Game->m_EnableNetVR)
	{
		m_Game->m_PlayersVRInfo[i].isUsingVR = false;
		return 1;
	}

	VRNet::Pose pose{};
	if (VRNet::Decode(move, pose))
	{
		auto &info = m_Game->m_PlayersVRInfo[i];
		if (!info.isUsingVR)
			Game::logMsg("VR client on slot %d (gun pose streaming)", i);

		info.isUsingVR = true;
		info.isMeleeing = pose.swinging;
		info.controllerAngle = pose.ang;
		info.controllerPos = pose.pos;

		// Hitscan follows the gun, not the face. Movement cmds without
		// attack keep HMD yaw so walking still matches look.
		if (move->buttons & IN_ATTACK)
			move->viewangles = pose.ang;
	}
	else
	{
		m_Game->m_PlayersVRInfo[i].isUsingVR = false;
	}
	return 1;
}

void __fastcall Hooks::dWriteUsercmdDeltaToBuffer(void *ecx, void *edx, int a1, void *buf, int from, int to, bool isnewcommand) 
{
	return hkWriteUsercmdDeltaToBuffer.fOriginal(ecx, a1, buf, from, to, isnewcommand);
}

int Hooks::dWriteUsercmd(void *buf, CUserCmd *to, CUserCmd *from)
{
	if (!m_VR->m_IsVREnabled || !m_Game->m_EnableNetVR || !to)
		return hkWriteUsercmd.fOriginal(buf, to, from);

	const int originalCommandNum = to->command_number;
	const float xAngle = to->viewangles.x;
	const int originalTick = to->tick_count;
	const float originalUp = to->upmove;
	const float originalRoll = to->viewangles.z;

	VRNet::Pose pose;
	pose.pos = m_VR->GetRightControllerAbsPos();
	pose.ang = m_VR->GetRightControllerAbsAngle();
	pose.swinging = VectorLength(m_VR->m_RightControllerPose.TrackedDeviceVel) > 1.1f;
	VRNet::Encode(to, pose);

	hkWriteUsercmd.fOriginal(buf, to, from);

	to->viewangles.x = xAngle;
	to->tick_count = originalTick;
	to->viewangles.z = originalRoll;
	to->upmove = originalUp;
	to->command_number = originalCommandNum;

	// Recalc prediction checksum so MP gunfire doesn't desync.
	if (m_Game->m_Offsets->g_pppInput.found && m_Game->m_Offsets->g_pppInput.address)
	{
		CInput *m_Input = **(CInput ***)(m_Game->m_Offsets->g_pppInput.address);
		if (m_Input)
		{
			CVerifiedUserCmd *pVerifiedCommands = *reinterpret_cast<CVerifiedUserCmd **>(reinterpret_cast<uintptr_t>(m_Input) + 0xF0);
			if (pVerifiedCommands)
			{
				CVerifiedUserCmd *pVerified = &pVerifiedCommands[originalCommandNum % 150];
				pVerified->m_cmd = *to;
				pVerified->m_crc = to->GetChecksum();
			}
		}
	}
	return 1;
}

void Hooks::dAdjustEngineViewport(int &x, int &y, int &width, int &height)
{
	hkAdjustEngineViewport.fOriginal(x, y, width, height);
}

void Hooks::dViewport(void *ecx, void *edx, int x, int y, int width, int height)
{
	hkViewport.fOriginal(ecx, x, y, width, height);
}

void Hooks::dGetViewport(void *ecx, void *edx, int &x, int &y, int &width, int &height)
{
	hkGetViewport.fOriginal(ecx, x, y, width, height);
}

int Hooks::dTestMeleeSwingCollisionClient(void *ecx, void *edx, Vector const &vec)
{
	return hkTestMeleeSwingCollisionClient.fOriginal(ecx, vec);
}

int Hooks::dTestMeleeSwingCollisionServer(void *ecx, void *edx, Vector const &vec)
{
	return hkTestMeleeSwingCollisionServer.fOriginal(ecx, vec);
}

void Hooks::dDoMeleeSwingServer(void *ecx, void *edx)
{
	return hkDoMeleeSwingServer.fOriginal(ecx);
}

void Hooks::dStartMeleeSwingServer(void *ecx, void *edx, void *player, bool a3)
{
	return hkStartMeleeSwingServer.fOriginal(ecx, player, a3);
}

int Hooks::dPrimaryAttackServer(void *ecx, void *edx)
{
	return hkPrimaryAttackServer.fOriginal(ecx);
}

void Hooks::dItemPostFrameServer(void *ecx, void *edx)
{
	hkItemPostFrameServer.fOriginal(ecx);
}

int Hooks::dGetPrimaryAttackActivity(void *ecx, void *edx, void *meleeInfo)
{
	return hkGetPrimaryAttackActivity.fOriginal(ecx, meleeInfo);
}

Vector *Hooks::dEyePosition(void *ecx, void *edx, Vector *eyePos)
{
	Vector *result = hkEyePosition.fOriginal(ecx, eyePos);
	const int i = m_Game->m_CurrentUsercmdID;
	if (!result || !VRNet::SlotOk(i))
		return result;

	auto &info = m_Game->m_PlayersVRInfo[i];
	if (info.isUsingVR && (m_Game->m_PerformingMelee || info.isMeleeing))
		*result = info.controllerPos;

	return result;
}

int __fastcall Hooks::dDrawModel(void *ecx, void *edx, int flags, void *pRenderable, int instance, int entity_index, const void *model, const Vector &origin, const QAngle &angles, int skin, int body, int hitboxset, const matrix3x4_t *modelToWorld, const matrix3x4_t *pLightingOffset)
{
	const Vector *useOrigin = &origin;
	const QAngle *useAngles = &angles;
	Vector vmOrigin;
	QAngle vmAngles;

	const char *name = nullptr;
	if (model && m_Game && m_Game->m_ModelInfo)
		name = m_Game->m_ModelInfo->GetModelName(const_cast<void *>(model));

	static int s_n = 0;
	if (s_n < 24)
	{
		Game::logMsg("DrawModel #%d stereo=%d idx=%d origin=(%.0f,%.0f,%.0f) %s",
		             s_n, (int)g_inStereoPass, entity_index,
		             origin.x, origin.y, origin.z, name ? name : "?");
		++s_n;
	}
	else if (name && (strstr(name, "/v_") || strstr(name, "\\v_") || strstr(name, "/vm_")))
	{
		static int s_v = 0;
		if (s_v < 12)
		{
			Game::logMsg("DrawModel v_ #%d stereo=%d %s", s_v, (int)g_inStereoPass, name);
			++s_v;
		}
	}

	bool isViewmodel = false;
	if (name && (strstr(name, "/v_") || strstr(name, "\\v_") || strstr(name, "/vm_")
		|| strstr(name, "v_hands") || strstr(name, "v_arms")))
		isViewmodel = true;

	if (isViewmodel && m_VR && m_VR->m_IsVREnabled && m_VR->m_MotionControls && !modelToWorld)
	{
		const int vm = m_ViewmodelIndexThisFrame++;
		if (vm == 0)
		{
			vmOrigin = m_VR->GetRecommendedViewmodelAbsPos();
			vmAngles = m_VR->GetRecommendedViewmodelAbsAngle();
		}
		else
		{
			vmOrigin = m_VR->GetLeftControllerAbsPos();
			vmAngles = m_VR->GetLeftControllerAbsAngle();
		}
		useOrigin = &vmOrigin;
		useAngles = &vmAngles;
		static int s_vm = 0;
		if (s_vm < 8)
		{
			Game::logMsg("viewmodel -> controller #%d %s", vm, name ? name : "?");
			++s_vm;
		}
	}

	if (!hkDrawModel.fOriginal)
		return 0;
	return hkDrawModel.fOriginal(ecx, flags, pRenderable, instance, entity_index, model,
	                             *useOrigin, *useAngles, skin, body, hitboxset, modelToWorld, pLightingOffset);
}


// Moves the first-person weapon onto the motion controller.
//
// This runs BEFORE bone matrices are computed, which is the whole point: the
// 00:24 log showed the viewmodel reaching DrawModelExecute with origin exactly
// equal to the view origin, and rewriting it there changed nothing because the
// pose was already baked into the bones. Setup takes pInfo by non-const
// reference and the caller passes the SAME object on to Execute, so a change
// here lands in both the bone setup and the draw.
bool __fastcall Hooks::dDrawModelSetup(void *ecx, void *edx, ModelRenderInfo_t &info, void *pState, void *pCustomBoneToWorld, void *ppBoneToWorldOut)
{
	bool applyPose = false;
	if (m_VR && m_VR->m_IsVREnabled && m_VR->m_MotionControls
	    && info.pModel && m_Game && m_Game->m_ModelInfo)
	{
		const char *mn = m_Game->m_ModelInfo->GetModelName(info.pModel);
		const bool isVm = mn && (strstr(mn, "/v_") || strstr(mn, "\\v_") || strstr(mn, "/vm_"));
		VmVtable::Note(info.pRenderable, mn, isVm);
		if (isVm)
		{
			// Only the local first-person model: it is drawn at the view origin.
			const Vector delta = info.origin - m_VR->m_SetupOrigin;
			if (VectorLength(delta) < 80.0f)
			{
				info.origin = m_VR->GetRecommendedViewmodelAbsPos();
				info.angles = m_VR->GetRecommendedViewmodelAbsAngle();

				// The origin write above does not actually move the model --
				// point the renderable's transform accessors at our pose instead.
				VmRenderable::g_origin = info.origin;
				VmRenderable::g_angles = info.angles;
				applyPose = true;
				if (m_VR->m_ViewmodelRenderablePatch)
					VmRenderable::Patch(info.pRenderable);

				static int s_moved = 0;
				if (s_moved < 12)
				{
					Game::logMsg("VM MOVED -> (%.0f,%.0f,%.0f) ang=(%.0f,%.0f,%.0f) %s",
					             info.origin.x, info.origin.y, info.origin.z,
					             info.angles.x, info.angles.y, info.angles.z, mn);
					++s_moved;
				}
			}
		}
	}

	// Scope the override to THIS draw only.
	//
	// g_havePose used to be latched true forever, so the patched accessors
	// returned the controller pose for every call the engine ever made on that
	// class -- culling, attachment lookups, bounds, everything -- not just the
	// bone setup we care about. That is the "warpy, all over the place" motion:
	// the weapon's transform was being answered with our hand pose in contexts
	// that had nothing to do with drawing it.
	if (hkDrawModelSetup.fOriginal)
	{
		if (applyPose)
			VmRenderable::g_havePose = true;
		const bool r = hkDrawModelSetup.fOriginal(ecx, info, pState, pCustomBoneToWorld, ppBoneToWorldOut);
		// Latched mode keeps the pose active outside this call: the gun then
		// stays on the hand through draw paths we do not intercept (firing),
		// at the cost of the engine also seeing it for culling/attachments.
		if (!m_VR || m_VR->m_ViewmodelScopedPose)
			VmRenderable::g_havePose = false;
		return r;
	}
	VmRenderable::g_havePose = false;
	return false;
}

// --- Tracked weapon -----------------------------------------------------------
//
// DrawModelExecute's last argument is the bone-to-world matrices that
// DrawModelSetup just built for this draw: world space, laid out around the
// viewmodel's own origin at the eye. Moving every one of them by the same rigid
// transform -- hand pose times inverse(viewmodel pose) -- draws the whole gun,
// animation and all, in the hand. The weapon entity itself is untouched.
//
// That is the difference from the attempts recorded in HANDOFF: writing
// info.origin comes after the bones are built (no effect), and patching
// GetRenderOrigin/Angles moved only some of the transform (the gun skewed).
// Here every bone gets the one transform, so the model stays rigid.
struct BoneMat { float m[3][4]; };
static BoneMat g_handBones[256];

// Source's AngleMatrix: columns are forward, left, up; column 3 the origin.
static BoneMat PoseMatrix(const QAngle &ang, const Vector &pos)
{
	Vector f, r, u;
	QAngle::AngleVectors(ang, &f, &r, &u);
	BoneMat b;
	b.m[0][0] = f.x; b.m[0][1] = -r.x; b.m[0][2] = u.x; b.m[0][3] = pos.x;
	b.m[1][0] = f.y; b.m[1][1] = -r.y; b.m[1][2] = u.y; b.m[1][3] = pos.y;
	b.m[2][0] = f.z; b.m[2][1] = -r.z; b.m[2][2] = u.z; b.m[2][3] = pos.z;
	return b;
}

static BoneMat InvertRigid(const BoneMat &a)
{
	BoneMat b;
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			b.m[i][j] = a.m[j][i];
	for (int i = 0; i < 3; ++i)
		b.m[i][3] = -(b.m[i][0] * a.m[0][3] + b.m[i][1] * a.m[1][3] + b.m[i][2] * a.m[2][3]);
	return b;
}

static BoneMat Concat(const BoneMat &a, const BoneMat &b)
{
	BoneMat c;
	for (int i = 0; i < 3; ++i)
	{
		for (int j = 0; j < 3; ++j)
			c.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
		c.m[i][3] = a.m[i][0] * b.m[0][3] + a.m[i][1] * b.m[1][3] + a.m[i][2] * b.m[2][3] + a.m[i][3];
	}
	return c;
}

// studiohdr_t, read through DrawModelState_t (whose first member it is):
// id "IDST", version 44..49, numbones at +156. Guarded: state is the engine's.
static int ReadStudioBoneCount(void *state)
{
	__try
	{
		const unsigned char *hdr = *reinterpret_cast<const unsigned char *const *>(state);
		if (!hdr)
			return -1;
		const int id = *reinterpret_cast<const int *>(hdr);
		const int version = *reinterpret_cast<const int *>(hdr + 4);
		const int bones = *reinterpret_cast<const int *>(hdr + 156);
		if (id != 0x54534449 || version < 44 || version > 49 || bones < 1 || bones > 256)
			return -1;
		return bones;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return -1;
	}
}

// Index of a bone by name in the studio header behind DrawModelState_t
// (boneindex at +160, mstudiobone_t is 216 bytes, name offset first). -1 if
// absent or unreadable.
static int FindStudioBone(void *state, const char *wanted)
{
	__try
	{
		const unsigned char *hdr = *reinterpret_cast<const unsigned char *const *>(state);
		const int count = *reinterpret_cast<const int *>(hdr + 156);
		const int boneIndex = *reinterpret_cast<const int *>(hdr + 160);
		for (int i = 0; i < count && i < 256; ++i)
		{
			const unsigned char *bone = hdr + boneIndex + i * 216;
			const char *name = reinterpret_cast<const char *>(bone + *reinterpret_cast<const int *>(bone));
			if (strcmp(name, wanted) == 0)
				return i;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
	return -1;
}

// Arm-rig models, cached by model: the hand bone and the arm bones to fold.
struct ArmRig
{
	const void *model;
	int hand;
	int arm[4];          // collar, arm null, shoulder, elbow (-1 if absent)
	float local[3];      // hand position in model space, slowly averaged
	bool haveLocal;
};

static ArmRig *ArmRigFor(void *state, const void *model, const char *modelName)
{
	static ArmRig s_rigs[16];
	static int s_count = 0;
	for (int i = 0; i < s_count; ++i)
		if (s_rigs[i].model == model)
			return s_rigs[i].hand >= 0 ? &s_rigs[i] : nullptr;
	ArmRig r{};
	r.model = model;
	r.hand = FindStudioBone(state, "R_FK_Hand_jnt");
	const char *armBones[4] = { "R_FK_Collar_jnt", "R_FK_Arm_null", "R_FK_Shoulder_jnt", "R_FK_Elbow_jnt" };
	for (int i = 0; i < 4; ++i)
		r.arm[i] = (r.hand >= 0) ? FindStudioBone(state, armBones[i]) : -1;
	if (r.hand >= 0)
		Game::logMsg("TRACKED GUN: %s is an arm rig -- hand bone %d, arm bones %d %d %d %d",
		             modelName, r.hand, r.arm[0], r.arm[1], r.arm[2], r.arm[3]);
	if (s_count < 16)
		s_rigs[s_count++] = r;
	else
		return nullptr;
	return r.hand >= 0 ? &s_rigs[s_count - 1] : nullptr;
}

// Where the held gun's barrel is in its model: the "muzzle" attachment's
// origin and the axis it fires along, from the live bones of the last gun
// drawn. VR::UpdateGunAim traces from there, along there.
//
// The axis used to be assumed: the model's +X, i.e. the hand's forward. But
// GE:S guns are built lying along -Y (every v_*.mdl's muzzle attachment points
// down -Y in the bind pose) and only turned to face forward by their
// sequences, so wherever an animation holds the gun at an angle -- the end of
// the draw, idle sway -- the dot left the drawn barrel. Reading the axis off
// the live attachment keeps the dot on the barrel you can see, whatever the
// pose. Model space, re-placed with the current hand pose every frame so it
// does not lag the hand; eased over a few frames so a firing kick nudges the
// dot rather than throwing it.
//
// NOT from the muzzle bone's own matrix, though. The engine sets up only the
// bones the mesh uses when it draws (BONE_USED_BY_VERTEX); muzzle.bone1 is
// flagged attachment-only (0x200), so its entry in the draw's bone array is
// whatever was last computed -- which is when the muzzle flash asked for the
// attachment, i.e. your last shot. Every frame in between, that stale world
// pose was read against the current viewmodel pose: the dot swung opposite to
// your head, drifted as you walked, started thousands of units away after a
// respawn, and snapped back onto the gun each time you fired (00:08 log).
// So the muzzle is carried by the nearest ancestor the mesh does use (base,
// for every GE:S gun), with the muzzle's bind-pose offset from it.
static float g_muzzleLocal[3] = { 0.0f, 0.0f, 0.0f };
static float g_muzzleAxisLocal[3] = { 1.0f, 0.0f, 0.0f };
static ULONGLONG g_muzzleSeenUntil = 0;
static const void *g_muzzleModel = nullptr;
static unsigned g_muzzleFrame = 0;

struct MuzzleInfo
{
	const void *model;
	int bone;              // muzzle.bone1 / muzzle.bone01, -1 if none
	int anchor;            // nearest ancestor the mesh uses (kept current), -1 if none
	bool haveAttachment;
	float axis[3];         // barrel direction in the anchor's frame
	float offset[3];       // muzzle origin in the anchor's frame
};

// The bone the muzzle can ride on, and the muzzle's rest transform in that
// bone's frame: poseToBone(anchor) * inverse(poseToBone(muzzle)). mstudiobone_t
// (216 bytes): parent +4, poseToBone matrix3x4 +96, flags +160; any
// BONE_USED_BY_VERTEX_LODn bit (0x3FC00) means draws keep it current.
static int MuzzleAnchor(void *state, int bone, BoneMat &rel)
{
	__try
	{
		const unsigned char *hdr = *reinterpret_cast<const unsigned char *const *>(state);
		const int count = *reinterpret_cast<const int *>(hdr + 156);
		const int boneIndex = *reinterpret_cast<const int *>(hdr + 160);
		auto boneAt = [&](int i) { return hdr + boneIndex + i * 216; };
		auto poseToBone = [&](int i) {
			BoneMat m;
			memcpy(m.m, boneAt(i) + 96, sizeof(m.m));
			return m;
		};
		int anchor = bone;
		for (int guard = 0; anchor >= 0 && anchor < count && guard < 256; ++guard)
		{
			if (*reinterpret_cast<const int *>(boneAt(anchor) + 160) & 0x0003FC00)
				break;
			anchor = *reinterpret_cast<const int *>(boneAt(anchor) + 4);
		}
		if (anchor < 0 || anchor >= count)
			return -1;
		rel = Concat(poseToBone(anchor), InvertRigid(poseToBone(bone)));
		return anchor;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
	return -1;
}

// The attachment on 'bone': studiohdr_t numlocalattachments at +240, index at
// +244; mstudioattachment_t is 92 bytes -- name offset, flags, localbone, then
// its matrix3x4_t relative to that bone. The X column is the direction a
// muzzle flash fires, i.e. down the barrel.
static bool FindAttachmentOnBone(void *state, int bone, float axis[3], float offset[3])
{
	__try
	{
		const unsigned char *hdr = *reinterpret_cast<const unsigned char *const *>(state);
		const int count = *reinterpret_cast<const int *>(hdr + 240);
		const int index = *reinterpret_cast<const int *>(hdr + 244);
		for (int i = 0; i < count && i < 64; ++i)
		{
			const unsigned char *att = hdr + index + i * 92;
			if (*reinterpret_cast<const int *>(att + 8) != bone)
				continue;
			const float *m = reinterpret_cast<const float *>(att + 12);   // rows of 4
			axis[0] = m[0]; axis[1] = m[4]; axis[2] = m[8];
			offset[0] = m[3]; offset[1] = m[7]; offset[2] = m[11];
			const float len = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
			if (len < 0.5f || len > 2.0f)
				return false;
			for (int k = 0; k < 3; ++k)
				axis[k] /= len;
			return true;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
	return false;
}

static const MuzzleInfo &MuzzleFor(void *state, const void *model, const char *modelName)
{
	static MuzzleInfo s_info[32];
	static int s_count = 0;
	for (int i = 0; i < s_count; ++i)
		if (s_info[i].model == model)
			return s_info[i];
	MuzzleInfo m{};
	m.model = model;
	m.bone = FindStudioBone(state, "muzzle.bone1");
	if (m.bone < 0)
		m.bone = FindStudioBone(state, "muzzle.bone01");
	m.anchor = -1;
	m.haveAttachment = m.bone >= 0 && FindAttachmentOnBone(state, m.bone, m.axis, m.offset);
	if (!m.haveAttachment)
	{
		m.axis[0] = 1.0f; m.axis[1] = 0.0f; m.axis[2] = 0.0f;
		m.offset[0] = m.offset[1] = m.offset[2] = 0.0f;
	}
	BoneMat rel;
	if (m.bone >= 0)
		m.anchor = MuzzleAnchor(state, m.bone, rel);
	if (m.anchor >= 0)
	{
		// Attachment (in the muzzle bone's frame) -> the anchor's frame.
		float a[3], o[3];
		for (int k = 0; k < 3; ++k)
		{
			a[k] = rel.m[k][0] * m.axis[0] + rel.m[k][1] * m.axis[1] + rel.m[k][2] * m.axis[2];
			o[k] = rel.m[k][0] * m.offset[0] + rel.m[k][1] * m.offset[1] + rel.m[k][2] * m.offset[2] + rel.m[k][3];
		}
		for (int k = 0; k < 3; ++k)
		{
			m.axis[k] = a[k];
			m.offset[k] = o[k];
		}
	}
	if (m.bone >= 0)
		Game::logMsg("TRACKED GUN: %s muzzle bone %d on bone %d, attachment %s, axis=(%.2f,%.2f,%.2f) offset=(%.1f,%.1f,%.1f)",
		             modelName, m.bone, m.anchor, m.haveAttachment ? "found" : "MISSING (using the model's forward)",
		             m.axis[0], m.axis[1], m.axis[2], m.offset[0], m.offset[1], m.offset[2]);
	if (s_count < 32)
	{
		s_info[s_count] = m;
		return s_info[s_count++];
	}
	static MuzzleInfo s_spare;
	s_spare = m;
	return s_spare;
}

// The muzzle and the barrel direction in the world with the current hand
// pose, if a gun was drawn in the last 150 ms. Melee weapons and throwables
// have no muzzle bone.
bool GESVR_MuzzleWorld(Vector &out, Vector &dir)
{
	VR *vr = Hooks::m_VR;
	if (!vr || GetTickCount64() > g_muzzleSeenUntil)
		return false;
	const BoneMat hand = PoseMatrix(vr->GetRecommendedViewmodelAbsAngle(), vr->GetRecommendedViewmodelAbsPos());
	const float *p = g_muzzleLocal, *a = g_muzzleAxisLocal;
	out.x = hand.m[0][0] * p[0] + hand.m[0][1] * p[1] + hand.m[0][2] * p[2] + hand.m[0][3];
	out.y = hand.m[1][0] * p[0] + hand.m[1][1] * p[1] + hand.m[1][2] * p[2] + hand.m[1][3];
	out.z = hand.m[2][0] * p[0] + hand.m[2][1] * p[1] + hand.m[2][2] * p[2] + hand.m[2][3];
	dir.x = hand.m[0][0] * a[0] + hand.m[0][1] * a[1] + hand.m[0][2] * a[2];
	dir.y = hand.m[1][0] * a[0] + hand.m[1][1] * a[1] + hand.m[1][2] * a[2];
	dir.z = hand.m[2][0] * a[0] + hand.m[2][1] * a[1] + hand.m[2][2] * a[2];
	const float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
	if (len < 0.001f)
		return false;
	dir.x /= len; dir.y /= len; dir.z /= len;
	return true;
}

// --- Tracked weapon, step 4: effects from the gun in the hand -----------------
//
// The muzzle flash (GE:S muzzle_* particles following the "muzzle"
// attachment), its light, the muzzle smoke, ejected shells and the start of
// your own tracers (GetTracerOrigin uses the local player's viewmodel
// attachment) all ask the VIEWMODEL where its attachments are -- and the
// viewmodel entity is still at your head; only its drawing was moved. Every
// one of those positions is made in C_BaseAnimating's attachment setup, which
// passes each through C_BaseViewModel::FormatViewModelAttachment (normally a
// correction for the viewmodel FOV). With the gun in hand the attachment gets
// the same rigid move the drawn bones do instead: hand * inverse(viewmodel
// pose). No FOV correction: the tracked gun is drawn at the eye's own FOV.
// GE:S's muzzle particles are world effects (none but the sniper rifle's empty
// parent is a "view model effect" in ge_muzzle_fx.pcf), so they render in the
// normal pass, right where they now are.
//
// The viewmodel pose is read at that moment through its own GetRenderOrigin /
// GetRenderAngles -- the pair SetupBones builds the bones from -- because on
// the frame you fire the view angles have just swung to the barrel, and a pose
// remembered from the last draw would put the flash off by that swing. The
// hand is the one the last tracked draw of that viewmodel used (placed by the
// hand bone for the slappers and knives).
struct TrackedHand
{
	const void *entity;
	BoneMat hand;
	ULONGLONG until;
};
static TrackedHand g_trackedHands[4];

static void NoteTrackedHand(const void *entity, const BoneMat &hand)
{
	int slot = -1, oldest = 0;
	for (int i = 0; i < 4; ++i)
	{
		if (g_trackedHands[i].entity == entity)
		{
			slot = i;
			break;
		}
		if (g_trackedHands[i].until < g_trackedHands[oldest].until)
			oldest = i;
	}
	if (slot < 0)
		slot = oldest;
	g_trackedHands[slot].entity = entity;
	g_trackedHands[slot].hand = hand;
	g_trackedHands[slot].until = GetTickCount64() + 250;
}

// IClientRenderable (entity + 4) slots 1 and 2, GetRenderOrigin and
// GetRenderAngles: in this client.dll both start
// "cmp [ecx+4A4h],0 / j.. / cmp byte [ecx+50h],17h" (return the followed
// entity's pose, else the abs pose), shared by C_BaseViewModel and
// C_PredictedViewModel. Checked per vtable before the first call.
typedef const float *(__thiscall *tRenderPoseGetter)(void *renderable);

static bool ViewmodelRenderPose(void *entity, Vector &origin, QAngle &angles)
{
	static void **s_goodVt = nullptr, **s_badVt = nullptr;
	void *renderable = static_cast<char *>(entity) + 4;
	void **vt = *reinterpret_cast<void ***>(renderable);
	if (!vt || vt == s_badVt)
		return false;
	if (vt != s_goodVt)
	{
		bool ok = false;
		__try
		{
			static const unsigned char kHead[] = { 0x83, 0xB9, 0xA4, 0x04, 0x00, 0x00, 0x00 };
			static const unsigned char kFollow[] = { 0x80, 0x79, 0x50, 0x17 };
			const unsigned char *o = static_cast<const unsigned char *>(vt[1]);
			const unsigned char *a = static_cast<const unsigned char *>(vt[2]);
			ok = memcmp(o, kHead, 7) == 0 && memcmp(o + 9, kFollow, 4) == 0 &&
			     memcmp(a, kHead, 7) == 0 && memcmp(a + 9, kFollow, 4) == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			ok = false;
		}
		Game::logMsg("Viewmodel GetRenderOrigin/GetRenderAngles %s (vtable %p)",
		             ok ? "verified" : "NOT recognised -- effects stay at the head", vt);
		if (!ok)
		{
			s_badVt = vt;
			return false;
		}
		s_goodVt = vt;
	}
	const float *o = reinterpret_cast<tRenderPoseGetter>(vt[1])(renderable);
	const float *a = reinterpret_cast<tRenderPoseGetter>(vt[2])(renderable);
	if (!o || !a)
		return false;
	origin = Vector(o[0], o[1], o[2]);
	angles = QAngle(a[0], a[1], a[2]);
	return true;
}

static bool FaceAimMoveAttachment(void *entity, BoneMat &m);   // below, with FaceAimBones

void __fastcall Hooks::dFormatViewModelAttachment(void *ecx, void *edx, int nAttachment, void *attachmentToWorld)
{
	if (m_VR && m_VR->m_TrackedWeapon && ecx && attachmentToWorld)
	{
		const ULONGLONG now = GetTickCount64();
		for (const TrackedHand &t : g_trackedHands)
		{
			if (t.entity != ecx || now > t.until)
				continue;
			Vector origin;
			QAngle angles;
			if (!ViewmodelRenderPose(ecx, origin, angles))
				break;
			BoneMat &m = *static_cast<BoneMat *>(attachmentToWorld);
			const Vector before(m.m[0][3], m.m[1][3], m.m[2][3]);
			m = Concat(Concat(t.hand, InvertRigid(PoseMatrix(angles, origin))), m);
			static int s_logged = 0;
			if (s_logged < 8)
			{
				Game::logMsg("TRACKED GUN: attachment %d (%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f) hand=(%.1f,%.1f,%.1f)",
				             nAttachment, before.x, before.y, before.z, m.m[0][3], m.m[1][3], m.m[2][3],
				             t.hand.m[0][3], t.hand.m[1][3], t.hand.m[2][3]);
				++s_logged;
			}
			return;
		}
	}
	// Face aim: move with the gun (FaceAimBones), not GE:S's FOV correction --
	// the gun was moved to where that correction puts the muzzle, so the
	// flash stays put and the rest (shells) now leave the gun as drawn.
	if (m_VR && !m_VR->m_TrackedWeapon && ecx && attachmentToWorld &&
	    FaceAimMoveAttachment(ecx, *static_cast<BoneMat *>(attachmentToWorld)))
		return;
	hkFormatViewModelAttachment.fOriginal(ecx, nAttachment, attachmentToWorld);
}

// Pre-squash along this eye's up axis, centred on the eye: D = I + (s-1)uu^T
// plus the matching translation. The first-person pass's window aspect
// stretches it back, so first-person models land in true shape where the world
// does. The same for both eyes (they share the up axis and differ only along
// right). False when there is nothing to undo.
static bool EyeSquash(BoneMat &squash)
{
	if (!g_eyeValid || fabsf(g_squash - 1.0f) <= 0.001f || g_squash <= 0.2f || g_squash >= 5.0f)
		return false;
	Vector f, r, u;
	QAngle::AngleVectors(g_eyeAngles, &f, &r, &u);
	const float k = g_squash - 1.0f;
	const float uv[3] = { u.x, u.y, u.z };
	const float e[3] = { g_eyeOrigin.x, g_eyeOrigin.y, g_eyeOrigin.z };
	const float ue = uv[0] * e[0] + uv[1] * e[1] + uv[2] * e[2];
	for (int i = 0; i < 3; ++i)
	{
		for (int j = 0; j < 3; ++j)
			squash.m[i][j] = (i == j ? 1.0f : 0.0f) + k * uv[i] * uv[j];
		squash.m[i][3] = -k * uv[i] * ue;
	}
	return true;
}

// Face aim (gun not in hand): the head-locked viewmodel.
//
// Squash: pre-squashed like the gun in hand. GE:S's DrawViewModels draws at
// the window's aspect, so without it the gun came out ~1.8x too tall and
// pulled away from the centre vertically ("a little squished and skewed").
//
// Shift: drawn at the eye's FOV, in its true place, the gun sits right in
// front of your face -- viewmodels are built for a narrow viewmodel FOV that
// spreads them out to the lower right. GE:S's own attachment FOV correction
// (C_BaseViewModel::FormatViewModelAttachment) still put the muzzle flash and
// tracers out there: it scales an attachment's offset across the view by
// tan(fov/2) / tan(fovViewmodel/2) of the game's view. So the whole gun is
// moved, rigidly, by that same amount measured at its muzzle (at the hand
// for the slappers and knives): gun, flash and tracers meet where GE:S puts
// its effects, and the gun keeps its true size and shape. FaceAimGunSpread
// scales it (1 = there, 0 = straight in front). The attachment hook moves
// every attachment by the same shift, so shells come out of the moved gun.
struct FaceAimShift
{
	const void *entity;
	const void *model;
	float ref[3];          // reference point (muzzle / hand) in the viewmodel frame, eased
	float shift[3];        // the move, in the viewmodel frame (forward, left, up)
	unsigned frame;
	ULONGLONG until;
};
static FaceAimShift g_faceAim[4];

static FaceAimShift &FaceAimSlot(const void *entity)
{
	int oldest = 0;
	for (int i = 0; i < 4; ++i)
	{
		if (g_faceAim[i].entity == entity)
			return g_faceAim[i];
		if (g_faceAim[i].until < g_faceAim[oldest].until)
			oldest = i;
	}
	g_faceAim[oldest] = FaceAimShift{};
	g_faceAim[oldest].entity = entity;
	return g_faceAim[oldest];
}

static void *FaceAimBones(void *state, const ModelRenderInfo_t &info, void *bones, const char *modelName, VR *vr)
{
	const int count = ReadStudioBoneCount(state);
	if (count < 0)
		return bones;
	const BoneMat vm = PoseMatrix(info.angles, info.origin);
	const BoneMat inv = InvertRigid(vm);
	const BoneMat *src = static_cast<const BoneMat *>(bones);
	const ULONGLONG now = GetTickCount64();
	FaceAimShift &s = FaceAimSlot(static_cast<const char *>(info.pRenderable) - 4);

	float k = 0.0f;
	if (g_gameFov > 1.0f && g_gameFovVM > 1.0f && g_gameFov < 179.0f && g_gameFovVM < 179.0f)
	{
		const float d2r = 3.14159265f / 180.0f;
		const float f = tanf(g_gameFov * 0.5f * d2r) / tanf(g_gameFovVM * 0.5f * d2r);
		if (f > 0.2f && f < 8.0f)
			k = (f - 1.0f) * vr->m_FaceAimGunSpread;
		static bool s_logged = false;
		if (!s_logged)
		{
			s_logged = true;
			Game::logMsg("FACE AIM: game fov %.1f, viewmodel fov %.1f -> spread x%.2f (FaceAimGunSpread %.2f)",
			             g_gameFov, g_gameFovVM, f, vr->m_FaceAimGunSpread);
		}
	}
	if (s.frame != g_stereoFrame)
	{
		s.frame = g_stereoFrame;
		float p[3];
		bool haveRef = false;
		const MuzzleInfo &mz = MuzzleFor(state, info.pModel, modelName);
		if (mz.anchor >= 0 && mz.anchor < count)
		{
			const BoneMat local = Concat(inv, src[mz.anchor]);
			for (int r = 0; r < 3; ++r)
				p[r] = local.m[r][0] * mz.offset[0] + local.m[r][1] * mz.offset[1] + local.m[r][2] * mz.offset[2] + local.m[r][3];
			haveRef = true;
		}
		else if (ArmRig *rig = ArmRigFor(state, info.pModel, modelName))
		{
			if (rig->hand < count)
			{
				const BoneMat local = Concat(inv, src[rig->hand]);
				for (int r = 0; r < 3; ++r)
					p[r] = local.m[r][3];
				haveRef = true;
			}
		}
		if (haveRef)
		{
			// Slow, so a firing kick is not multiplied into the whole gun.
			const float t = (s.model != info.pModel || now > s.until) ? 1.0f : 0.05f;
			for (int r = 0; r < 3; ++r)
				s.ref[r] += (p[r] - s.ref[r]) * t;
		}
		else
			s.ref[0] = s.ref[1] = s.ref[2] = 0.0f;
		s.model = info.pModel;
	}
	s.shift[0] = 0.0f;
	s.shift[1] = k * s.ref[1];
	s.shift[2] = k * s.ref[2];
	s.until = now + 250;

	// The shift in the world: PoseMatrix's columns are forward, left, up.
	float w[3];
	for (int r = 0; r < 3; ++r)
		w[r] = vm.m[r][1] * s.shift[1] + vm.m[r][2] * s.shift[2];
	BoneMat squash;
	const bool squashed = EyeSquash(squash);
	if (!squashed && fabsf(w[0]) + fabsf(w[1]) + fabsf(w[2]) < 0.001f)
		return bones;
	for (int i = 0; i < count; ++i)
	{
		BoneMat b = src[i];
		for (int r = 0; r < 3; ++r)
			b.m[r][3] += w[r];
		g_handBones[i] = squashed ? Concat(squash, b) : b;
	}
	return g_handBones;
}

// An attachment of a face-aim viewmodel, moved by that viewmodel's shift at
// its current pose. False if FaceAimBones has not drawn it lately.
static bool FaceAimMoveAttachment(void *entity, BoneMat &m)
{
	const ULONGLONG now = GetTickCount64();
	for (const FaceAimShift &s : g_faceAim)
	{
		if (s.entity != entity || now > s.until)
			continue;
		Vector origin;
		QAngle angles;
		if (!ViewmodelRenderPose(entity, origin, angles))
			return false;
		const BoneMat vm = PoseMatrix(angles, origin);
		for (int r = 0; r < 3; ++r)
			m.m[r][3] += vm.m[r][1] * s.shift[1] + vm.m[r][2] * s.shift[2];
		return true;
	}
	return false;
}

// GE:S's death curtain: models/VGUI/bloodanimation.mdl, the viewmodel of the
// gebloodscreen entity -- an 80 x 62.5 unit sheet of blood about 50 units in
// front of the game camera, whose drips slide down on death. In VR it hung off
// the game camera, which on death rides the ragdoll's head, and at the eye's
// FOV it covered only the middle of the view: "a red square bouncing around".
// Now each eye wears it: placed on that eye, stretched across the view
// (BloodCurtainScale, 2.6 by default, overfills the headset's FOV) and
// pre-squashed for the pass aspect like everything else in that pass.
static void *BloodCurtainBones(void *state, const ModelRenderInfo_t &info, void *bones, VR *vr)
{
	const int count = ReadStudioBoneCount(state);
	if (count < 0 || !g_eyeValid)
		return bones;
	BoneMat stretch = PoseMatrix(QAngle(0.0f, 0.0f, 0.0f), Vector(0.0f, 0.0f, 0.0f));
	const float k = (vr->m_BloodCurtainScale > 0.5f && vr->m_BloodCurtainScale < 10.0f) ? vr->m_BloodCurtainScale : 2.6f;
	stretch.m[1][1] = k;   // across
	stretch.m[2][2] = k;   // up and down
	BoneMat total = Concat(PoseMatrix(g_eyeAngles, g_eyeOrigin),
	                       Concat(stretch, InvertRigid(PoseMatrix(info.angles, info.origin))));
	BoneMat squash;
	if (EyeSquash(squash))
		total = Concat(squash, total);
	const BoneMat *src = static_cast<const BoneMat *>(bones);
	for (int i = 0; i < count; ++i)
		g_handBones[i] = Concat(total, src[i]);
	static bool s_logged = false;
	if (!s_logged)
	{
		s_logged = true;
		Game::logMsg("Death curtain: placed on each eye, stretched %.1fx", k);
	}
	return g_handBones;
}

// Returns the bones to draw with: ours, moved to the hand, or the originals.
static void *TrackedWeaponBones(void *state, const ModelRenderInfo_t &info, void *bones,
                                const char *modelName, VR *vr)
{
	const int count = ReadStudioBoneCount(state);
	if (count < 0)
	{
		static bool s_warned = false;
		if (!s_warned)
		{
			s_warned = true;
			Game::logMsg("TRACKED GUN: could not read the studio header for %s; drawing it where it was", modelName);
		}
		return bones;
	}
	const BoneMat vm = PoseMatrix(info.angles, info.origin);
	BoneMat hand = PoseMatrix(vr->GetRecommendedViewmodelAbsAngle(), vr->GetRecommendedViewmodelAbsPos());
	const BoneMat *src = static_cast<const BoneMat *>(bones);

	// Arm rigs (slappers, knives: any first-person model with R_FK_Hand_jnt)
	// are an arm, not a gun. Placed by origin, the shoulder end sat on the
	// controller; anchored by the hand, the arm then ran back into your face
	// and chest -- a flat-screen arm reaching from beside the camera. So the
	// hand bone goes on the controller and (MeleeHideArm) the arm bones are
	// folded into the wrist, leaving a floating hand. Where the hand sits in
	// the model comes from the live bones, averaged slowly so the attack
	// animation still reads as a swing instead of being pinned flat.
	ArmRig *rig = ArmRigFor(state, info.pModel, modelName);
	if (rig)
	{
		const QAngle off(vr->m_MeleeAngleOffset.x, vr->m_MeleeAngleOffset.y, vr->m_MeleeAngleOffset.z);
		hand = Concat(hand, PoseMatrix(off, Vector(0.0f, 0.0f, 0.0f)));   // tilt the hand about itself
		const BoneMat local = Concat(InvertRigid(vm), src[rig->hand]);   // hand in model space
		const float a = rig->haveLocal ? 0.02f : 1.0f;
		for (int k = 0; k < 3; ++k)
			rig->local[k] += (local.m[k][3] - rig->local[k]) * a;
		rig->haveLocal = true;
		// The controller, shifted by the weapon's tuned offset (numpad /
		// weapons.txt) in the gun frame -- exactly where a gun's anchor goes.
		const Vector c = vr->GetRecommendedViewmodelAbsPos();
		const float cv[3] = { c.x, c.y, c.z };
		for (int i = 0; i < 3; ++i)
			hand.m[i][3] = cv[i] - (hand.m[i][0] * rig->local[0] + hand.m[i][1] * rig->local[1] + hand.m[i][2] * rig->local[2]);
	}
	NoteTrackedHand(static_cast<const char *>(info.pRenderable) - 4, hand);   // for its attachments
	const BoneMat move = Concat(hand, InvertRigid(vm));

	// Guns (not arm rigs): note the barrel for the trace. Once a frame, not
	// per eye, so the easing runs at frame rate.
	if (!rig)
	{
		const MuzzleInfo &mz = MuzzleFor(state, info.pModel, modelName);
		if (mz.anchor >= 0 && mz.anchor < count && g_muzzleFrame != g_stereoFrame)
		{
			g_muzzleFrame = g_stereoFrame;
			const BoneMat local = Concat(InvertRigid(vm), src[mz.anchor]);   // anchor bone in model space
			float p[3], a[3];
			for (int k = 0; k < 3; ++k)
			{
				p[k] = local.m[k][0] * mz.offset[0] + local.m[k][1] * mz.offset[1] + local.m[k][2] * mz.offset[2] + local.m[k][3];
				a[k] = local.m[k][0] * mz.axis[0] + local.m[k][1] * mz.axis[1] + local.m[k][2] * mz.axis[2];
			}
			// A new gun, or the first sighting after a gap: take it as is.
			const ULONGLONG now = GetTickCount64();
			const float t = (g_muzzleModel != info.pModel || now > g_muzzleSeenUntil) ? 1.0f : 0.25f;
			float len = 0.0f;
			for (int k = 0; k < 3; ++k)
			{
				g_muzzleLocal[k] += (p[k] - g_muzzleLocal[k]) * t;
				g_muzzleAxisLocal[k] += (a[k] - g_muzzleAxisLocal[k]) * t;
				len += g_muzzleAxisLocal[k] * g_muzzleAxisLocal[k];
			}
			len = sqrtf(len);
			if (len > 0.001f)
				for (int k = 0; k < 3; ++k)
					g_muzzleAxisLocal[k] /= len;
			g_muzzleModel = info.pModel;
			g_muzzleSeenUntil = now + 150;
		}
	}
	// Pre-squash for the first-person pass's aspect (see EyeSquash).
	BoneMat squash = PoseMatrix(QAngle(0.0f, 0.0f, 0.0f), Vector(0.0f, 0.0f, 0.0f));
	EyeSquash(squash);
	const BoneMat total = Concat(squash, move);
	for (int i = 0; i < count; ++i)
		g_handBones[i] = Concat(total, src[i]);

	// Floating hand: fold every arm bone to a point at the hand, so the upper
	// arm and forearm shrink into the wrist and only the hand and cuff remain.
	// Bone matrices are absolute here, so the hand and fingers are unaffected.
	if (rig && vr->m_MeleeHideArm)
	{
		const BoneMat &h = g_handBones[rig->hand];
		for (int b : rig->arm)
		{
			if (b < 0 || b >= count)
				continue;
			for (int i = 0; i < 3; ++i)
			{
				for (int j = 0; j < 3; ++j)
					g_handBones[b].m[i][j] = 0.0f;
				g_handBones[b].m[i][3] = h.m[i][3];
			}
		}
	}

	static int s_logged = 0;
	if (s_logged < 6)
	{
		const Vector h = vr->GetRecommendedViewmodelAbsPos();
		Game::logMsg("TRACKED GUN %s bones=%d viewmodel=(%.1f,%.1f,%.1f) -> hand=(%.1f,%.1f,%.1f)",
		             modelName, count, info.origin.x, info.origin.y, info.origin.z, h.x, h.y, h.z);
		++s_logged;
	}
	return g_handBones;
}

void Hooks::dDrawModelExecute(void *ecx, void *edx, void *state, const ModelRenderInfo_t &info, void *pCustomBoneToWorld)
{
	if (m_Game->m_SwitchedWeapons)
		m_Game->m_CachedArmsModel = false;

	// Signature check for the newly-hooked vtable slot. If slot 19 is not
	// DrawModelExecute, info is garbage and the model name will be junk or
	// unreadable -- so validate before trusting it, and say so plainly.
	{
		static long s_drawCalls = 0;
		++s_drawCalls;
		// Slot 19 is CONFIRMED DrawModelExecute (sensible model paths, 316k
		// calls). Sampling 1-in-4000 can never catch the viewmodel though:
		// there is one viewmodel draw against ~100 props every frame. Log any
		// first-person model specifically instead.
		const char *mn = (info.pModel && m_Game->m_ModelInfo)
		                   ? m_Game->m_ModelInfo->GetModelName(info.pModel) : nullptr;
		if (mn && (strstr(mn, "/v_") || strstr(mn, "\\v_") || strstr(mn, "/vm_") ||
		           strstr(mn, "arms") || strstr(mn, "hand")))
		{
			static int s_vm = 0;
			if (s_vm < 24)
			{
				Game::logMsg("VIEWMODEL seen stereo=%d origin=(%.0f,%.0f,%.0f) setup=(%.0f,%.0f,%.0f) %s",
				             (int)g_inStereoPass,
				             info.origin.x, info.origin.y, info.origin.z,
				             m_VR->m_SetupOrigin.x, m_VR->m_SetupOrigin.y, m_VR->m_SetupOrigin.z, mn);
				++s_vm;
			}
		}
		if ((s_drawCalls % 20000) == 1)
			Game::logMsg("DRAWEXEC #%ld alive stereo=%d", s_drawCalls, (int)g_inStereoPass);
	}

	// Unfiltered name scan. The filtered version matched nothing at all, which
	// tells us the viewmodel is not named the way we assumed -- so log whatever
	// IS coming through and read the answer off the list.
	if (m_VR && m_VR->m_IsVREnabled && info.pModel && m_Game->m_ModelInfo)
	{
		static int s_names = 0;
		if (s_names < 40)
		{
			const char *mn = m_Game->m_ModelInfo->GetModelName(info.pModel);
			if (mn && mn[0])
			{
				Game::logMsg("MODELSCAN #%d stereo=%d %s", s_names, (int)g_inStereoPass, mn);
				++s_names;
			}
		}
	}

	// Tracked weapon: first-person models (the gun, and separate arms if the
	// game draws them) near the eye get drawn at the hand. Computed before the
	// block below rewrites info.origin, which is the viewmodel pose we need.
	void *bones = pCustomBoneToWorld;
	bool trackedDraw = false;
	if (g_inStereoPass && m_VR && m_VR->m_IsVREnabled && m_VR->m_TrackedWeapon && pCustomBoneToWorld &&
	    state && info.pModel && m_Game->m_ModelInfo)
	{
		const char *mn = m_Game->m_ModelInfo->GetModelName(info.pModel);
		const bool firstPerson = mn && (strstr(mn, "/v_") || strstr(mn, "\\v_") || strstr(mn, "/vm_") ||
		                                strstr(mn, "/arms/") || strstr(mn, "v_hands"));
		if (firstPerson && VectorLength(info.origin - m_VR->m_SetupOrigin) < 80.0f)
		{
			bones = TrackedWeaponBones(state, info, pCustomBoneToWorld, mn, m_VR);
			trackedDraw = (bones != pCustomBoneToWorld);
		}
	}

	// Face aim: the same models, left at the head: true shape, and out where
	// GE:S puts their muzzle flash (see FaceAimBones).
	if (!trackedDraw && g_inStereoPass && m_VR && m_VR->m_IsVREnabled &&
	    (m_VR->m_FixViewmodelAspect || fabsf(m_VR->m_FaceAimGunSpread) > 0.001f) &&
	    pCustomBoneToWorld && state && info.pModel && m_Game->m_ModelInfo)
	{
		const char *mn = m_Game->m_ModelInfo->GetModelName(info.pModel);
		const bool firstPerson = mn && (strstr(mn, "/v_") || strstr(mn, "\\v_") || strstr(mn, "/vm_") ||
		                                strstr(mn, "/arms/") || strstr(mn, "v_hands"));
		if (firstPerson && VectorLength(info.origin - m_VR->m_SetupOrigin) < 80.0f)
			bones = FaceAimBones(state, info, pCustomBoneToWorld, mn, m_VR);
	}

	// The death curtain, worn by each eye (see BloodCurtainBones).
	if (g_inStereoPass && g_eyeValid && m_VR && m_VR->m_IsVREnabled && pCustomBoneToWorld && state &&
	    info.pModel && m_Game->m_ModelInfo)
	{
		const char *mn = m_Game->m_ModelInfo->GetModelName(info.pModel);
		if (mn && strstr(mn, "bloodanimation"))
			bones = BloodCurtainBones(state, info, pCustomBoneToWorld, m_VR);
	}

	if (!trackedDraw && g_inStereoPass && m_VR && m_VR->m_IsVREnabled && info.pModel && m_Game->m_ModelInfo)
	{
		const char *modelName = m_Game->m_ModelInfo->GetModelName(info.pModel);
		if (modelName && (strstr(modelName, "/v_") || strstr(modelName, "\\v_") || strstr(modelName, "/vm_")))
		{
			Vector delta = info.origin - m_VR->m_SetupOrigin;
			if (VectorLength(delta) < 80.0f)
			{
				ModelRenderInfo_t &mut = const_cast<ModelRenderInfo_t &>(info);
				mut.origin = m_VR->GetRecommendedViewmodelAbsPos();
				mut.angles = m_VR->GetRecommendedViewmodelAbsAngle();
				g_execMoves = g_execMoves + 1;
				static int s_exec = 0;
				if (s_exec < 10)
				{
					// If this prints and the gun still does not translate, the
					// write is simply too late: bone matrices are already built by
					// DrawModelExecute, so only DrawModelSetup can move it.
					Game::logMsg("EXEC MOVE fired -> (%.0f,%.0f,%.0f) was (%.0f,%.0f,%.0f)",
					             mut.origin.x, mut.origin.y, mut.origin.z,
					             m_VR->m_SetupOrigin.x, m_VR->m_SetupOrigin.y, m_VR->m_SetupOrigin.z);
					++s_exec;
				}
			}
		}
	}

	bool hideArms = m_Game->m_IsMeleeWeaponActive || m_VR->m_HideArms;
	
	if (info.pModel)
	{
		std::string modelName = m_Game->m_ModelInfo->GetModelName(info.pModel);
		// Only the first-person model (v_*) is the gun in hand. Any /weapons/
		// path used to count, so ammo crates and guns lying on the floor
		// (models/weapons/ammocrate.mdl, w_*) kept overwriting it.
		const bool inWeapons = modelName.find("/weapons/") != std::string::npos
		                    || modelName.find("\\weapons\\") != std::string::npos;
		// Fallback only: once VR::RefreshActiveWeapon reads the weapon's own
		// viewmodel index this guess is ignored -- GE:S draws the slapper hands
		// after the gun, so "last v_ model drawn" named the shotgun "slappers".
		if (inWeapons && !m_VR->m_WeaponFromNetvar &&
		    (modelName.find("/v_") != std::string::npos || modelName.find("\\v_") != std::string::npos))
			m_Game->m_ActiveWeaponModel = modelName;

		if (hideArms && !m_Game->m_CachedArmsModel)
		{
			if (modelName.find("/arms/") != std::string::npos || modelName.find("v_hands") != std::string::npos)
			{
				m_Game->m_ArmsMaterial = m_Game->m_MaterialSystem->FindMaterial(modelName.c_str(), "Model textures");
				m_Game->m_ArmsModel = info.pModel;
				m_Game->m_CachedArmsModel = true;
			}
		}
	}

	if (info.pModel && info.pModel == m_Game->m_ArmsModel && hideArms)
	{
		m_Game->m_ArmsMaterial->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, true);
		m_Game->m_ModelRender->ForcedMaterialOverride(m_Game->m_ArmsMaterial);
		hkDrawModelExecute.fOriginal(ecx, state, info, bones);
		m_Game->m_ModelRender->ForcedMaterialOverride(NULL);
		return;
	}

	hkDrawModelExecute.fOriginal(ecx, state, info, bones);
}

void Hooks::dPushRenderTargetAndViewport(void *ecx, void *edx, ITexture *pTexture, ITexture *pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH)
{
	if (!m_VR->m_CreatedVRTextures)
		return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);

	if (m_PushHUDStep == 2)
		++m_PushHUDStep;
	else
		m_PushHUDStep = -999;

	// RenderView calls PushRenderTargetAndViewport multiple times with different textures. 
	// When the call order goes PopRenderTargetAndViewport -> IsSplitScreen -> PrePushRenderTarget -> PushRenderTargetAndViewport,
	// then it pushed the HUD/GUI render target to the RT stack.
	if (m_PushHUDStep == 3)
	{
		pTexture = m_VR->m_HUDTexture;

		IMatRenderContext *renderContext = m_Game->m_MaterialSystem->GetRenderContext();
		renderContext->ClearBuffers(false, true, true);

		hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);

		renderContext->OverrideAlphaWriteEnable(true, true);
		renderContext->ClearColor4ub(0, 0, 0, 0);
		renderContext->ClearBuffers(true, false);

		m_VR->m_RenderedHud = true;
		m_PushedHud = true;
	}
	else
	{
		hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
	}
}

void Hooks::dPopRenderTargetAndViewport(void *ecx, void *edx)
{
	if (!m_VR->m_CreatedVRTextures)
		return hkPopRenderTargetAndViewport.fOriginal(ecx);

	m_PushHUDStep = 0;

	if (m_PushedHud)
	{
		m_Game->m_MaterialSystem->GetRenderContext()->OverrideAlphaWriteEnable(false, true);
		m_Game->m_MaterialSystem->GetRenderContext()->ClearColor4ub(0, 0, 0, 255);
	}

	hkPopRenderTargetAndViewport.fOriginal(ecx);
}

void Hooks::dVGui_Paint(void *ecx, void *edx, int mode)
{
	// The stereo eye passes never paint VGUI (Grok's guard, restored as it was).
	//
	// It was narrowed to "only while a cursor menu is up" on 2026-09-21 so the
	// in-game HUD would paint again, and both runs of that build broke: the
	// headset flickered between menu and game mode, then locked in menu mode
	// for a whole map (just the floating 2D window). Painting in-game VGUI
	// inside our stereo RenderView evidently keeps the Win32 cursor visible,
	// and the visible cursor is exactly what IsMenuMode() reads as "an in-map
	// menu is open". The 19th's runs with this full guard were stable in map.
	// Cost: the in-game HUD is not in the captured frame, so the head-tap HUD
	// crops come up empty -- find another way to get the HUD before narrowing
	// this again.
	if (g_inStereoPass)
		return;

	if (!m_VR->m_CreatedVRTextures)
		return hkVgui_Paint.fOriginal(ecx, mode);

	// GE:S has no splitscreen HUD push. If the L4D2-style 3-step dance never
	// fired, force the HUD onto our overlay texture here.
	if (!m_PushedHud && hkPushRenderTargetAndViewport.fOriginal && m_VR->m_HUDTexture)
	{
		IMatRenderContext *renderContext = m_Game->m_MaterialSystem->GetRenderContext();
		int w = 1280, h = 720;
		if (m_Game->m_EngineClient)
			m_Game->m_EngineClient->GetScreenSize(w, h);
		hkPushRenderTargetAndViewport.fOriginal(renderContext, m_VR->m_HUDTexture, nullptr, 0, 0, w, h);
		renderContext->ClearColor4ub(0, 0, 0, 0);
		renderContext->ClearBuffers(true, false);
		m_PushedHud = true;
		m_VR->m_RenderedHud = true;
	}

	if (m_PushedHud)
		mode = PAINT_UIPANELS | PAINT_INGAMEPANELS;

	hkVgui_Paint.fOriginal(ecx, mode);

	if (m_PushedHud && hkPopRenderTargetAndViewport.fOriginal)
	{
		hkPopRenderTargetAndViewport.fOriginal(m_Game->m_MaterialSystem->GetRenderContext());
	}
}

int Hooks::dIsSplitScreen()
{
	if (m_PushHUDStep == 0)
		++m_PushHUDStep;
	else
		m_PushHUDStep = -999;

	return hkIsSplitScreen.fOriginal();
}

DWORD *Hooks::dPrePushRenderTarget(void *ecx, void *edx, int a2)
{
	if (m_PushHUDStep == 1)
		++m_PushHUDStep;
	else
		m_PushHUDStep = -999;

	return hkPrePushRenderTarget.fOriginal(ecx, a2);
}

// Reported by the motion trace: if these climb and the weapon still does not
// move, the renderer is not placing the model from these accessors and the
// approach is wrong. If they stay at zero, the patch is not being reached.
long GESVR_RenderOriginCalls() { return VmRenderable::g_originCalls; }
long GESVR_RenderAnglesCalls() { return VmRenderable::g_anglesCalls; }
