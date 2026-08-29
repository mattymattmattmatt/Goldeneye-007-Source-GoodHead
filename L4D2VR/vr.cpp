#include "vr.h"
#include <Windows.h>
#include "sdk.h"
#include "game.h"
#include "hooks.h"
#include "trace.h"
#include "weapons.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <string>
#include <filesystem>
#include <thread>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <atomic>
#include <mutex>
#include <d3d9_vr.h>

// Frame-stage timing. The mod has twice been diagnosed by guesswork; this
// makes the cost of each stage visible so a single run localizes a stall.
namespace {
    using vrclock = std::chrono::steady_clock;
    inline float MsSince(vrclock::time_point a, vrclock::time_point b)
    {
        return std::chrono::duration<float, std::milli>(b - a).count();
    }
}

static int  g_theaterThrottleMs = 2000;
static bool g_menuDriveCursor = true;

// --- Present watchdog -------------------------------------------------------
// A frozen main thread cannot report its own freeze. This samples the last
// Present timestamp from a separate thread so the log distinguishes "engine is
// busy loading a map" (comes back) from "deadlocked" (never does), and says
// how long it has been stuck. Touches no engine interfaces -- only atomics.
static std::atomic<long long> g_lastPresentMs{ 0 };
static std::atomic<int>  g_watchInMap{ 0 };
static std::atomic<int>  g_watchStereoPass{ 0 };
static std::atomic<bool> g_watchdogRun{ false };

static long long NowMs()
{
    return (long long)GetTickCount64();
}

// ---------------------------------------------------------------------------
// Hang diagnosis: sample the stalled thread instead of guessing.
// Three theories about this freeze have now been wrong. Rather than a fourth,
// suspend the Present thread and report where its instruction pointer actually
// is, plus any return addresses still on its stack that fall inside a loaded
// module. Module names alone are decisive: openvr_api = compositor/overlay,
// user32 = window/cursor, vgui2/vguimatsurface = VGUI, engine/client = the game
// itself, d3d9 = our own code.
// ---------------------------------------------------------------------------
static HANDLE g_presentThread = nullptr;

static bool GESVR_DescribeAddr(DWORD_PTR addr, char *out, size_t outSz)
{
    if (addr < 0x10000)
        return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT || mbi.AllocationBase == nullptr)
        return false;
    // Only executable pages -- filters out stack data that merely looks like a pointer.
    const DWORD prot = mbi.Protect & 0xFF;
    if (!(prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
          prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY))
        return false;
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH))
        return false;
    const char *base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    _snprintf_s(out, outSz, _TRUNCATE, "%s+0x%X", base,
                (unsigned)(addr - (DWORD_PTR)mbi.AllocationBase));
    return true;
}

static void GESVR_CaptureStalledStack()
{
    if (!g_presentThread)
    {
        Game::logMsg("STACK: no Present-thread handle captured");
        return;
    }

    if (SuspendThread(g_presentThread) == (DWORD)-1)
    {
        Game::logMsg("STACK: SuspendThread failed (%lu)", GetLastError());
        return;
    }

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    char desc[320];
    if (GetThreadContext(g_presentThread, &ctx))
    {
        if (GESVR_DescribeAddr((DWORD_PTR)ctx.Eip, desc, sizeof(desc)))
            Game::logMsg("STACK: EIP  %s", desc);
        else
            Game::logMsg("STACK: EIP  0x%08X (unresolved)", (unsigned)ctx.Eip);

        // Poor-man's unwind: these binaries are old MSVC with frame-pointer
        // omission, so scan the stack for executable return addresses instead
        // of trusting an EBP chain.
        int printed = 0;
        DWORD_PTR *sp = (DWORD_PTR *)ctx.Esp;
        for (int i = 0; i < 1024 && printed < 14; ++i)
        {
            DWORD_PTR val = 0;
            __try { val = sp[i]; }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            if (GESVR_DescribeAddr(val, desc, sizeof(desc)))
            {
                Game::logMsg("STACK:  [%02d] %s", printed, desc);
                ++printed;
            }
        }
        if (printed == 0)
            Game::logMsg("STACK: no resolvable return addresses");
    }
    else
    {
        Game::logMsg("STACK: GetThreadContext failed (%lu)", GetLastError());
    }

    ResumeThread(g_presentThread);
}

static void GESVR_WatchdogThread()
{
    int reported = 0;
    while (g_watchdogRun.load())
    {
        Sleep(1000);
        const long long last = g_lastPresentMs.load();
        if (last == 0)
            continue;
        const long long stalled = NowMs() - last;
        if (stalled > 3000)
        {
            if (reported < 20)
            {
                Game::logMsg("WATCHDOG no Present for %lld ms (inMap=%d stereoPasses=%d)",
                             stalled, g_watchInMap.load(), g_watchStereoPass.load());
                ++reported;
            }
            // Sample twice, seconds apart: identical EIP means a hard
            // block, a moving EIP means a spin.
            if (reported == 1 || reported == 5)
                GESVR_CaptureStalledStack();
        }
        else
        {
            reported = 0;
        }
    }
}

void GESVR_NoteStereoPass()
{
    g_watchStereoPass.fetch_add(1);
}

// ============================================================================
// Menu input worker
// ----------------------------------------------------------------------------
// EVERY USER32 call for menu input lives on this thread and nowhere else.
//
// Why: VR::Update runs inside D3D9DeviceEx::PresentEx. Driving the OS cursor
// from there (ClientToScreen / SetCursorPos / PostMessage) hung the main thread
// permanently -- the 13:12 log shows the freeze beginning on the exact frame
// menu input went live, with the watchdog then reporting "no Present" for 22+
// seconds while the process stayed alive. SetCursorPos transacts with the
// desktop input thread; issuing it from a thread that is inside Present and not
// pumping its own message queue is a classic cross-thread deadlock.
//
// The render thread only stores plain values here. It never blocks, never takes
// a lock, and never calls into USER32. This thread applies them at ~120Hz and
// also publishes window state back, so even the diagnostic logging on the
// Present path reads an atomic instead of calling FindWindow/GetForegroundWindow.
// ============================================================================
namespace MenuInput
{
    static std::atomic<bool> g_run{ false };
    static std::atomic<int>  g_aimX{ -1 };
    static std::atomic<int>  g_aimY{ -1 };
    static std::atomic<unsigned> g_aimSeq{ 0 };     // bumped when aim changes
    static std::atomic<unsigned> g_clickSeq{ 0 };   // bumped to request a click
    static std::atomic<unsigned> g_keySeq{ 0 };
    static std::atomic<int>  g_keyVk{ 0 };
    static std::atomic<bool> g_enabled{ true };
    // SetCursorPos is a system-wide call that can transact with the desktop
    // input thread. PostMessage cannot block. If the cursor turns out to be
    // what wedges the game, turn this off and rely on WM_MOUSEMOVE alone --
    // Source's inputsystem does consume it.
    static std::atomic<bool> g_useSetCursorPos{ true };
    // Click delivery. PostMessage synthesises a message with no hardware input
    // state behind it; VGUI calls SetMouseCapture on button-down, and a
    // synthetic press whose physical button was never down can leave it waiting
    // for a release that cannot come. SendInput goes through the real input
    // queue instead, exactly like a physical mouse. Safe from this thread --
    // this thread owns all USER32.
    static std::atomic<bool> g_clickViaSendInput{ false };

    // Published back for the render thread to read cheaply.
    static std::atomic<void*> g_hwnd{ nullptr };
    static std::atomic<int>  g_foreground{ 0 };
    static std::atomic<int>  g_iconic{ 0 };
    static std::atomic<int>  g_visible{ 0 };

    static void SetAim(int x, int y)
    {
        if (x < 0 || y < 0)
            return;
        const int px = g_aimX.exchange(x);
        const int py = g_aimY.exchange(y);
        if (px != x || py != y)
            g_aimSeq.fetch_add(1);
    }

    static void QueueClick()  { g_clickSeq.fetch_add(1); }
    static void QueueKey(int vk) { g_keyVk.store(vk); g_keySeq.fetch_add(1); }

    static void Thread()
    {
        HWND hwnd = nullptr;
        unsigned lastAim = 0, lastClick = 0, lastKey = 0;
        int lastX = -1, lastY = -1;

        while (g_run.load())
        {
            Sleep(8);

            if (!hwnd || !IsWindow(hwnd))
            {
                hwnd = FindWindowA("Valve001", nullptr);
                g_hwnd.store((void*)hwnd);
            }
            if (!hwnd)
                continue;

            g_foreground.store(GetForegroundWindow() == hwnd ? 1 : 0);
            g_iconic.store(IsIconic(hwnd) ? 1 : 0);
            g_visible.store(IsWindowVisible(hwnd) ? 1 : 0);

            if (!g_enabled.load())
                continue;

            const unsigned aimSeq = g_aimSeq.load();
            if (aimSeq != lastAim)
            {
                lastAim = aimSeq;
                const int x = g_aimX.load();
                const int y = g_aimY.load();
                if (x >= 0 && y >= 0 && (x != lastX || y != lastY))
                {
                    lastX = x; lastY = y;
                    POINT pt = { x, y };
                    ClientToScreen(hwnd, &pt);
                    SetCursorPos(pt.x, pt.y);
                    PostMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
                }
            }

            const unsigned clickSeq = g_clickSeq.load();
            if (clickSeq != lastClick)
            {
                lastClick = clickSeq;
                const int x = g_aimX.load(), y = g_aimY.load();
                if (x >= 0 && y >= 0)
                {
                    if (g_clickViaSendInput.load())
                    {
                        POINT pt = { x, y };
                        ClientToScreen(hwnd, &pt);
                        SetCursorPos(pt.x, pt.y);
                        INPUT dn{}; dn.type = INPUT_MOUSE; dn.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                        INPUT up{}; up.type = INPUT_MOUSE; up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                        SendInput(1, &dn, sizeof(INPUT));
                        Sleep(24);
                        SendInput(1, &up, sizeof(INPUT));
                        Game::logMsg("MenuInput click via SendInput at (%d,%d) fg=%d", x, y, g_foreground.load());
                    }
                    else
                    {
                        const LPARAM lp = MAKELPARAM(x, y);
                        PostMessageA(hwnd, WM_MOUSEMOVE, 0, lp);
                        Game::logMsg("MenuInput click: posting DOWN at (%d,%d) fg=%d", x, y, g_foreground.load());
                        PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
                        Sleep(24);
                        PostMessageA(hwnd, WM_LBUTTONUP, 0, lp);
                        Game::logMsg("MenuInput click: posted UP, click complete");
                    }
                }
            }

            const unsigned keySeq = g_keySeq.load();
            if (keySeq != lastKey)
            {
                lastKey = keySeq;
                const WPARAM vk = (WPARAM)g_keyVk.load();
                PostMessageA(hwnd, WM_KEYDOWN, vk, 0);
                PostMessageA(hwnd, WM_KEYUP, vk, 0);
            }
        }
    }

    static void Start()
    {
        bool expected = false;
        if (g_run.compare_exchange_strong(expected, true))
            std::thread(Thread).detach();
    }
}

// ============================================================================
// Compositor submit thread
// ----------------------------------------------------------------------------
// Every vr::VRCompositor() Submit / WaitGetPoses call lives here and nowhere
// else. Running them on the D3D Present callstack deadlocks: DXVK's present and
// OpenVR's Vulkan submit both drive the same graphics queue. The 13:26 log is
// the proof -- "MENU f=5" logged, "AfterPresent tick #5" never did, and the
// watchdog then counted 22 seconds with the process still alive.
//
// This thread performs NO D3D work. The render thread prepares textures and
// publishes them; this thread only submits. Blocking in WaitGetPoses here is
// correct and costs the game nothing.
// ============================================================================
namespace VRSubmit
{
    static std::atomic<bool> g_run{ false };
    static std::atomic<bool> g_blackReady{ false };
    static std::atomic<bool> g_eyesReady{ false };
    static std::atomic<int>  g_frames{ 0 };
    static std::atomic<int>  g_lastErr{ 0 };
    static std::mutex        g_poseLock;
    // OFF by default -- see the note in AfterPresent. Kept only for A/B.
    static std::atomic<bool> g_useThread{ false };
}

extern long GESVR_ExecMoveCount();
extern long GESVR_RenderOriginCalls();
extern long GESVR_RenderAnglesCalls();

static bool g_menuPlaced = false;
static float g_menuYaw = 0.0f;

// A SharedTextureHolder is only safe to hand to OpenVR when our own capture
// code filled it. Every filler sets handle = &m_VulkanData, so that self-
// pointer is the signature we check for; stale heap bytes will not reproduce
// it. Checking handle != nullptr alone is NOT enough -- that is what let a
// garbage pointer reach SetOverlayTexture and kill the process at menu time.
// OpenVR copies the Vulkan image when you hand it to an overlay, which means a
// vkQueueSubmit on OUR graphics queue -- the same queue DXVK submits from. That
// is the identical race that made compositor Submit deadlock Present, and every
// SetOverlayTexture call in this file was doing it unlocked, once per frame.
static vr::EVROverlayError SetOverlayTextureLocked(vr::IVROverlay *ov,
                                                   vr::VROverlayHandle_t handle,
                                                   const vr::Texture_t *tex)
{
    if (!ov)
        return vr::VROverlayError_InvalidHandle;
    if (g_D3DVR9) g_D3DVR9->LockSubmission();
    const vr::EVROverlayError e = ov->SetOverlayTexture(handle, tex);
    if (g_D3DVR9) g_D3DVR9->UnlockSubmission();
    return e;
}

bool VR::TextureReady(const SharedTextureHolder &tex) const
{
    if (tex.m_VRTexture.handle != &tex.m_VulkanData)
        return false;
    if (tex.m_VRTexture.eType != vr::TextureType_Vulkan)
        return false;
    if (tex.m_VulkanData.m_nImage == 0 || tex.m_VulkanData.m_pDevice == nullptr)
        return false;
    if (tex.m_VulkanData.m_nWidth == 0 || tex.m_VulkanData.m_nHeight == 0)
        return false;
    return true;
}

void VR::MakeVRPath(char *out, size_t outCount, const char *relative)
{
    const char *dir = Game::ModDir();
    if (dir && dir[0])
        snprintf(out, outCount, "%s\\VR\\%s", dir, relative);
    else
    {
        char cwd[MAX_STR_LEN];
        GetCurrentDirectoryA(MAX_STR_LEN, cwd);
        snprintf(out, outCount, "%s\\VR\\%s", cwd, relative);
    }
}

// Every call here is an IPC round-trip to vrserver: one SetBool, BringToFront,
// FadeGrid, and seven FindOverlay lookups. This used to run on every single
// Present -- roughly 900 IPC calls a second on the render thread. Theater only
// needs suppressing occasionally, so throttle it and let the caller force a
// pass at startup.
static void GESVR_HideTheaterOverlays(bool force = false)
{
    static DWORD s_lastRun = 0;
    const DWORD now = GetTickCount();
    if (!force && s_lastRun != 0 && (now - s_lastRun) < 2000)
        return;
    s_lastRun = now;

    if (auto *settings = vr::VRSettings())
        settings->SetBool("dashboard", "autoShowGameTheater", false);

    // Do NOT CompositorBringToFront here. It runs on the render/Present
    // thread and has been measured to stall Submit by 400ms+, which is what
    // the user feels as "clicked the mouse and it locked up". Startup already
    // brings the scene to front once; periodic Theater suppression only needs
    // to hide the overlay handles.
    // FadeGrid removed: it is a compositor call, and this function runs on
    // the render/Present thread.

    auto *ov = vr::VROverlay();
    if (!ov)
        return;

    static const char *kNames[] = {
        "system.TheaterScreen",
        "system.dashboard.theater",
        "system.desktopGameView",
        "valve.steam.windows_gaming",
        "system.TheaterScreenDesktop",
        "system.GameTheater",
        "system.theaterScreen",
    };
    static bool s_logged = false;
    for (const char *name : kNames)
    {
        vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
        if (ov->FindOverlay(name, &handle) == vr::VROverlayError_None && handle)
        {
            ov->HideOverlay(handle);
            if (!s_logged)
                Game::logMsg("Hid SteamVR overlay %s", name);
        }
    }
    s_logged = true;
}

void GESVR_ClaimSteamVRScene()
{
    if (auto *settings = vr::VRSettings())
    {
        settings->SetBool("dashboard", "autoShowGameTheater", false);
        settings->SetBool("dashboard", "enableGameTheater", false);
        Game::logMsg("SteamVR dashboard.autoShowGameTheater/enableGameTheater = false");
    }

    if (auto *apps = vr::VRApplications())
    {
        char manifest[MAX_STR_LEN];
        VR::MakeVRPath(manifest, MAX_STR_LEN, "manifest.vrmanifest");
        apps->AddApplicationManifest(manifest, true);
        apps->IdentifyApplication(GetCurrentProcessId(), "gesource.vr");
        Game::logMsg("Identified process as gesource.vr pid=%u (%s)", GetCurrentProcessId(), manifest);
    }

    GESVR_HideTheaterOverlays(true);
}

void GESVR_SubmitBackBufferFallback()
{
    // Intentionally empty. This used to Submit() straight from the render
    // thread; the VRSubmit thread now owns every compositor submit, and it
    // keeps the scene alive with the black texture whenever no eyes are ready.
    // Submitting from here would reintroduce the Present-thread deadlock.
}

VR::VR(Game *game) 
{
    m_Game = game;

    char errorString[MAX_STR_LEN];

    vr::HmdError error = vr::VRInitError_None;
    m_System = vr::VRSystem();
    if (!m_System)
        m_System = vr::VR_Init(&error, vr::VRApplication_Scene);
    else
        error = vr::VRInitError_None;

    if (error != vr::VRInitError_None) 
    {
        snprintf(errorString, MAX_STR_LEN, "VR_Init failed: %s", vr::VR_GetVRInitErrorAsEnglishDescription(error));
        Game::errorMsg(errorString);
        return;
    }

    vr::EVRInitError peError = vr::VRInitError_None;

    if (!vr::VRCompositor())
    {
        Game::errorMsg("Compositor initialization failed.");
        return;
    }

    GESVR_ClaimSteamVRScene();

    m_Input = vr::VRInput();
    m_System = vr::VRSystem();
    if (!m_System)
        m_System = vr::OpenVRInternal_ModuleContext().VRSystem();

    m_System->GetRecommendedRenderTargetSize(&m_RenderWidth, &m_RenderHeight);
    Game::logMsg("OpenVR scene ready. recommended RT %ux%u", m_RenderWidth, m_RenderHeight);

    // Report the actual controller type. The action manifest ships default
    // bindings for oculus_touch, knuckles and vive_cosmos_controller ONLY. A
    // Vive wand (vive_controller), WMR (holographic_controller) or Reverb G2
    // (hpmotioncontroller) has no binding, so every digital action silently
    // reads false forever and no motion control works -- with no error anywhere.
    // MenuHealth has shown sel=0 atk=0 on every sample so far, which this
    // distinguishes between "not pressed" and "not bound".
    for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i)
    {
        if (m_System->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller)
            continue;
        char ctype[128] = {};
        char model[128] = {};
        m_System->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, ctype, sizeof(ctype));
        m_System->GetStringTrackedDeviceProperty(i, vr::Prop_ModelNumber_String, model, sizeof(model));
        const bool known = strstr(ctype, "oculus_touch") || strstr(ctype, "knuckles")
                        || strstr(ctype, "vive_cosmos");
        Game::logMsg("Controller %u type='%s' model='%s' bindingShipped=%d%s",
                     i, ctype, model, (int)known,
                     known ? "" : "  <-- NO DEFAULT BINDING, actions will never fire");
    }

    float l_left = 0.0f, l_right = 0.0f, l_top = 0.0f, l_bottom = 0.0f;
    m_System->GetProjectionRaw(vr::EVREye::Eye_Left, &l_left, &l_right, &l_top, &l_bottom);

    float r_left = 0.0f, r_right = 0.0f, r_top = 0.0f, r_bottom = 0.0f;
    m_System->GetProjectionRaw(vr::EVREye::Eye_Right, &r_left, &r_right, &r_top, &r_bottom);

    float tanHalfFov[2];

    tanHalfFov[0] = std::max({ -l_left, l_right, -r_left, r_right });
    tanHalfFov[1] = std::max({ -l_top, l_bottom, -r_top, r_bottom });

    m_TextureBounds[0].uMin = 0.5f + 0.5f * l_left / tanHalfFov[0];
    m_TextureBounds[0].uMax = 0.5f + 0.5f * l_right / tanHalfFov[0];
    m_TextureBounds[0].vMin = 0.5f - 0.5f * l_bottom / tanHalfFov[1];
    m_TextureBounds[0].vMax = 0.5f - 0.5f * l_top / tanHalfFov[1];

    m_TextureBounds[1].uMin = 0.5f + 0.5f * r_left / tanHalfFov[0];
    m_TextureBounds[1].uMax = 0.5f + 0.5f * r_right / tanHalfFov[0];
    m_TextureBounds[1].vMin = 0.5f - 0.5f * r_bottom / tanHalfFov[1];
    m_TextureBounds[1].vMax = 0.5f - 0.5f * r_top / tanHalfFov[1];

    m_Aspect = tanHalfFov[0] / tanHalfFov[1];
    m_Fov = 2.0f * atan(tanHalfFov[0]) * 360 / (3.14159265358979323846 * 2);
    m_HaveTextureBounds = (tanHalfFov[0] > 0.01f && tanHalfFov[1] > 0.01f);
    Game::logMsg("Eye frusta: superset fov=%.2f aspect=%.3f | L u[%.4f..%.4f] v[%.4f..%.4f] | R u[%.4f..%.4f] v[%.4f..%.4f]",
                 m_Fov, m_Aspect,
                 m_TextureBounds[0].uMin, m_TextureBounds[0].uMax,
                 m_TextureBounds[0].vMin, m_TextureBounds[0].vMax,
                 m_TextureBounds[1].uMin, m_TextureBounds[1].uMax,
                 m_TextureBounds[1].vMin, m_TextureBounds[1].vMax);

    InstallApplicationManifest("manifest.vrmanifest");
    SetActionManifest("action_manifest.json");

    std::thread configParser(&VR::WaitForConfigUpdate, this);
    configParser.detach();

    g_watchdogRun.store(true);
    std::thread(GESVR_WatchdogThread).detach();
    MenuInput::Start();

    // Background submit thread is OFF by default -- it froze the game by
    // frame 8 (18:48 build). See the long note in AfterPresent.
    if (VRSubmit::g_useThread.load())
    {
        VRSubmit::g_run.store(true);
        std::thread(&VR::SubmitThreadBody, this).detach();
        Game::logMsg("VRSubmit background thread ENABLED (experimental)");
    }
    else
        Game::logMsg("Compositor submit: render thread, once per frame (default)");

    // Deliberately NOT calling ShowMirrorWindow(). It opens a SteamVR desktop
    // window that competes for Win32 foreground focus, and Source throttles its
    // frame rate hard (and mutes audio) whenever its own window is not the
    // active app -- measured at ~3fps with 0.3ms of that spent in this mod.
    if (m_ShowMirrorWindow && vr::VRCompositor())
        vr::VRCompositor()->ShowMirrorWindow();

    Game::logMsg("Waiting for D3D9 VR device...");
    int waitedMs = 0;
    while (!g_D3DVR9 && waitedMs < 20000)
    {
        Sleep(10);
        waitedMs += 10;
    }

    if (!g_D3DVR9)
    {
        Game::logMsg("g_D3DVR9 never appeared; headset will use Present fallback until it does.");
        m_IsInitialized = true;
        m_IsVREnabled = true;
        return;
    }

    Game::logMsg("D3D9 VR device ready after %d ms", waitedMs);
    g_D3DVR9->GetBackBufferData(&m_VKBackBuffer);
    m_Overlay = vr::VROverlay();
    m_Overlay->CreateOverlay("MenuOverlayKey", "MenuOverlay", &m_MainMenuHandle);
    m_Overlay->CreateOverlay("HUDOverlayKey", "HUDOverlay", &m_HUDHandle);
    m_Overlay->CreateOverlay("GESVRWorldKey", "GESVRWorld", &m_WorldHandle);
    m_Overlay->SetOverlayInputMethod(m_MainMenuHandle, vr::VROverlayInputMethod_Mouse);
    m_Overlay->SetOverlayInputMethod(m_HUDHandle, vr::VROverlayInputMethod_Mouse);
    m_Overlay->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    m_Overlay->SetOverlayFlag(m_HUDHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    CreateWristOverlays();

    int windowWidth = 1280, windowHeight = 720;
    if (m_Game->m_EngineClient)
        m_Game->m_EngineClient->GetScreenSize(windowWidth, windowHeight);

    const vr::HmdVector2_t mouseScaleHUD = {windowWidth, windowHeight};
    m_Overlay->SetOverlayMouseScale(m_HUDHandle, &mouseScaleHUD);

    const vr::HmdVector2_t mouseScaleMenu = {(float)windowWidth, (float)windowHeight};
    m_Overlay->SetOverlayMouseScale(m_MainMenuHandle, &mouseScaleMenu);

    // Do not WaitGetPoses/Submit from this worker thread. Compositor calls
    // belong on the device/Present thread, and even then not *inside* Present.
    m_PosesThisFrame = false;

    m_IsInitialized = true;
    m_IsVREnabled = true;
    Game::logMsg("VR initialized (overlay-only until in-map stereo).");
}

int VR::SetActionManifest(const char *fileName) 
{
    char path[MAX_STR_LEN];
    char relative[MAX_STR_LEN];
    sprintf_s(relative, MAX_STR_LEN, "SteamVRActionManifest\\%s", fileName);
    MakeVRPath(path, MAX_STR_LEN, relative);

    // Report this either way. Every return value in the action chain --
    // SetActionManifestPath, GetActionHandle, GetActionSetHandle,
    // UpdateActionState, GetDigitalActionData -- was being discarded, so a
    // broken action set is indistinguishable from "the user isn't pressing".
    {
        const vr::EVRInputError merr = m_Input->SetActionManifestPath(path);
        Game::logMsg("SetActionManifestPath('%s') -> %d%s", path, (int)merr,
                     merr == vr::VRInputError_None ? " OK" : "  <-- FAILED, no action will ever fire");
    }

    m_Input->GetActionHandle("/actions/main/in/ActivateVR", &m_ActionActivateVR);
    m_Input->GetActionHandle("/actions/main/in/Jump", &m_ActionJump);
    m_Input->GetActionHandle("/actions/main/in/PrimaryAttack", &m_ActionPrimaryAttack);
    m_Input->GetActionHandle("/actions/main/in/Reload", &m_ActionReload);
    m_Input->GetActionHandle("/actions/main/in/TwoHand", &m_ActionTwoHand);
    m_Input->GetActionHandle("/actions/main/in/Use", &m_ActionUse);
    m_Input->GetActionHandle("/actions/main/in/Walk", &m_ActionWalk);
    m_Input->GetActionHandle("/actions/main/in/Turn", &m_ActionTurn);
    m_Input->GetActionHandle("/actions/main/in/SecondaryAttack", &m_ActionSecondaryAttack);
    m_Input->GetActionHandle("/actions/main/in/NextItem", &m_ActionNextItem);
    m_Input->GetActionHandle("/actions/main/in/PrevItem", &m_ActionPrevItem);
    m_Input->GetActionHandle("/actions/main/in/ResetPosition", &m_ActionResetPosition);
    m_Input->GetActionHandle("/actions/main/in/Crouch", &m_ActionCrouch);
    m_Input->GetActionHandle("/actions/main/in/Flashlight", &m_ActionFlashlight);
    m_Input->GetActionHandle("/actions/main/in/MenuSelect", &m_MenuSelect);
    m_Input->GetActionHandle("/actions/main/in/MenuBack", &m_MenuBack);
    m_Input->GetActionHandle("/actions/main/in/MenuUp", &m_MenuUp);
    m_Input->GetActionHandle("/actions/main/in/MenuDown", &m_MenuDown);
    m_Input->GetActionHandle("/actions/main/in/MenuLeft", &m_MenuLeft);
    m_Input->GetActionHandle("/actions/main/in/MenuRight", &m_MenuRight);
    m_Input->GetActionHandle("/actions/main/in/Spray", &m_Spray);
    m_Input->GetActionHandle("/actions/main/in/Scoreboard", &m_Scoreboard);
    m_Input->GetActionHandle("/actions/main/in/ShowHUD", &m_ShowHUD);
    m_Input->GetActionHandle("/actions/main/in/Pause", &m_Pause);

    {
        const vr::EVRInputError serr = m_Input->GetActionSetHandle("/actions/main", &m_ActionSet);
        Game::logMsg("GetActionSetHandle('/actions/main') -> %d handle=%llu%s",
                     (int)serr, (unsigned long long)m_ActionSet,
                     (serr == vr::VRInputError_None && m_ActionSet != vr::k_ulInvalidActionSetHandle)
                       ? " OK" : "  <-- INVALID, UpdateActionState will fail");
    }
    m_ActiveActionSet = {};
    m_ActiveActionSet.ulActionSet = m_ActionSet;

    return 0;
}

void VR::InstallApplicationManifest(const char *fileName)
{
    char path[MAX_STR_LEN];
    MakeVRPath(path, MAX_STR_LEN, fileName);
    vr::VRApplications()->AddApplicationManifest(path, true);
}

void VR::Update()
{
    if (!m_IsInitialized)
        return;

    GESVR_HideTheaterOverlays();

    static int s_frames = 0;
    if ((++s_frames % 90) == 1)
    {
        Game::logMsg("VR::Update frame=%d stereoFrame=%d menu=%d gameui=%d inmap=%d left=%p right=%p",
                     s_frames, (int)m_RenderedNewFrame, (int)IsMenuMode(),
                     (int)(m_Game && m_Game->IsGameUIVisible()),
                     (int)(m_Game && m_Game->IsInMap()),
                     m_VKLeftEye.m_VRTexture.handle, m_VKRightEye.m_VRTexture.handle);
    }

    // Verbose for the first 40 frames, then periodically. If the game is
    // crawling, the timestamps alone reveal the frame rate and the per-stage
    // numbers say which call is eating it.
    const bool trace = (s_frames <= 40) || ((s_frames % 90) == 1);

    // "gap" is wall time from the end of the previous VR::Update to the start of
    // this one: everything the ENGINE does per frame, with this mod excluded.
    // If gap is large while TOTAL is small, the stall is not ours. Focus state
    // is logged alongside because Source throttles when it is not the active app.
    static vrclock::time_point s_lastEnd{};
    static bool s_haveLast = false;
    const auto tFrameStart = vrclock::now();
    g_lastPresentMs.store(NowMs());
    if (!g_presentThread)
    {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                        GetCurrentProcess(), &g_presentThread,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                        FALSE, 0);
    }
    if (trace)
    {
        Game::logMsg("FRAME f=%d gap=%.1fms fg=%d iconic=%d visible=%d",
                     s_frames,
                     s_haveLast ? MsSince(s_lastEnd, tFrameStart) : 0.0f,
                     MenuInput::g_foreground.load(),
                     MenuInput::g_iconic.load(),
                     MenuInput::g_visible.load());
    }
    struct EndStamp {
        vrclock::time_point *out; bool *have;
        ~EndStamp() { *out = vrclock::now(); *have = true; }
    } endStamp{ &s_lastEnd, &s_haveLast };

    HideWorldOverlay();

    if (IsMenuMode())
    {
        const auto tStart = vrclock::now();
        if (m_Input)
        {
            const vr::EVRInputError uerr = m_Input->UpdateActionState(
                &m_ActiveActionSet, sizeof(vr::VRActiveActionSet_t), 1);
            static vr::EVRInputError s_lastUerr = (vr::EVRInputError)-1;
            if (uerr != s_lastUerr)
            {
                s_lastUerr = uerr;
                Game::logMsg("UpdateActionState -> %d%s", (int)uerr,
                             uerr == vr::VRInputError_None ? " OK"
                               : "  <-- FAILING, all digital actions read false");
            }
        }
        GetPoses();

        int cx = -1, cy = -1;
        ComputeMenuPointer(cx, cy);
        if (g_D3DVR9)
            g_D3DVR9->CaptureForOverlay(&m_VKHUD, cx, cy);
        const auto tCapture = vrclock::now();
        ShowMenuPanel();
        const auto tPanel = vrclock::now();
        ProcessMenuInput();
        const auto tEnd = vrclock::now();
        if (trace)
            Game::logMsg("MENU f=%d cap=%.1f panel=%.1f input=%.1f TOTAL=%.1fms",
                         s_frames, MsSince(tStart, tCapture),
                         MsSince(tCapture, tPanel), MsSince(tPanel, tEnd),
                         MsSince(tStart, tEnd));
        // Compositor Submit is AfterPresent — never inside PresentEx.
        return;
    }

    const auto tStart = vrclock::now();
    HideMenuPanel();

    // Refresh the flat-HUD capture while IN GAME. The wrist overlays crop
    // their watch/ammo faces out of m_VKHUD, but m_VKHUD was only ever
    // filled by the MENU branch -- so in game it held a stale menu frame
    // forever. At Present the backbuffer holds the finished frame including
    // the 2D HUD, which is exactly what we want to crop from.
    if (g_D3DVR9 && m_ShowWristHUD)
        g_D3DVR9->CaptureForOverlay(&m_VKHUD, -1, -1);

    // UpdateWristHUD/UpdateHurtHUD had NO call sites: the whole wrist
    // watch and damage-flash HUD, and every Wrist*/HurtHUD* config key,
    // were inert. Only runs in map.
    if (m_Game && m_Game->IsInMap())
    {
        UpdateWristHUD();
        UpdateHurtHUD();
    }
    else
        HideWristOverlays();

    ProcessInput();
    const auto tEnd = vrclock::now();
    if (trace)
        Game::logMsg("GAME f=%d input=%.1f TOTAL=%.1fms",
                     s_frames, MsSince(tStart, tEnd), MsSince(tStart, tEnd));
}

bool VR::IsMenuMode()
{
    if (m_Game && m_Game->IsGameUIVisible())
        return true;
    return !m_RenderedNewFrame;
}

static bool g_pendMouseDown = false;
static bool g_pendMouseUp = false;
static HWND g_pendHwnd = nullptr;
static int g_lastCursorX = 0;
static int g_lastCursorY = 0;
static bool g_haveCursor = false;

void VR::AfterPresent()
{
    const bool inMap = m_Game && m_Game->IsInMap();
    const bool haveEyes = TextureReady(m_VKLeftEye) && TextureReady(m_VKRightEye);
    const bool clicking = g_pendMouseDown || g_pendMouseUp;
    const auto t0 = vrclock::now();
    vr::EVRCompositorError werr = vr::VRCompositorError_None;

    // Clicks must not share a callstack with WaitGetPoses/Submit. Create
    // Server posted from AfterPresent (still inside PresentEx) froze the
    // main thread — music glitched because the audio thread kept going.
    // Fire the click from a short-lived worker after PresentEx unwinds.
    // Queue on the MenuInput thread instead of spawning a std::thread per
    // click from inside PresentEx -- thread creation takes the loader lock,
    // which is not something to do on the Present callstack.
    // Queue on the DOWN edge ONLY. g_pendMouseUp used to queue a second, complete
    // down+up cycle of its own, so every trigger pull sent TWO clicks and the
    // menu toggled straight back off again.
    if (g_pendMouseDown && g_menuDriveCursor)
        MenuInput::QueueClick();
    g_pendMouseDown = false;
    g_pendMouseUp = false;

    if (!vr::VRCompositor())
        return;

    // Log the in-map transition once: the whole map-load window used to be a
    // blind spot in the log.
    static int s_wasInMap = -1;
    if ((int)inMap != s_wasInMap)
    {
        Game::logMsg("=== inMap %d -> %d (haveEyes=%d) ===", s_wasInMap, (int)inMap, (int)haveEyes);
        s_wasInMap = (int)inMap;
        g_watchInMap.store((int)inMap);
    }

    // NOTE the condition: it is "have eyes", NOT "in map". Previously an in-map
    // frame with no captured eyes submitted NOTHING and never called
    // WaitGetPoses -- which is exactly the state during a multi-second map load,
    // leaving the compositor with no frames at all for the whole load.
    PrepareBlackTexture();
    VRSubmit::g_eyesReady.store(haveEyes && inMap);

    // Submit ONCE PER FRAME, on this (the render) thread, bracketed by DXVK's
    // submission lock. This is what the l4d2vr reference does and it is the
    // shape DXVK's lockSubmission() is designed for.
    //
    // Do NOT move this to a background thread looping at display rate: that was
    // tried (18:48 build) and froze the game by frame 8. lockSubmission() calls
    // m_submissionQueue.synchronize() -- a full wait for pending submissions --
    // before taking the queue, so holding it from a continuous 90Hz loop starves
    // DXVK's own submission thread and Present blocks in waitForSubmission
    // forever. Once per frame the wait is one frame's work and it is fine.
    if (!VRSubmit::g_useThread.load() && vr::VRCompositor())
    {
        auto *comp = vr::VRCompositor();
        werr = comp->WaitGetPoses(m_Poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        GetPoses();

        const vr::VRTextureBounds_t full = { 0.0f, 0.0f, 1.0f, 1.0f };
        vr::EVRCompositorError el = vr::VRCompositorError_None;
        vr::EVRCompositorError er = vr::VRCompositorError_None;
        bool submitted = false;

        if (g_D3DVR9)
            g_D3DVR9->LockSubmission();

        if (haveEyes && inMap)
        {
            const bool useBounds = m_UseTextureBounds && m_HaveTextureBounds;
            vr::VRTextureBounds_t lb = useBounds ? m_TextureBounds[0] : full;
            vr::VRTextureBounds_t rb = useBounds ? m_TextureBounds[1] : full;
            if (!m_UseVerticalCrop)
            {
                lb.vMin = rb.vMin = 0.0f;
                lb.vMax = rb.vMax = 1.0f;
            }
            el = comp->Submit(vr::Eye_Left,  &m_VKLeftEye.m_VRTexture,  &lb, vr::Submit_Default);
            er = comp->Submit(vr::Eye_Right, &m_VKRightEye.m_VRTexture, &rb, vr::Submit_Default);
            submitted = true;
        }
        else if (VRSubmit::g_blackReady.load() && TextureReady(m_SubmitBlack))
        {
            el = comp->Submit(vr::Eye_Left,  &m_SubmitBlack.m_VRTexture, &full, vr::Submit_Default);
            er = comp->Submit(vr::Eye_Right, &m_SubmitBlack.m_VRTexture, &full, vr::Submit_Default);
            submitted = true;
        }

        if (g_D3DVR9)
            g_D3DVR9->UnlockSubmission();

        if (submitted)
        {
            const int n = VRSubmit::g_frames.fetch_add(1) + 1;
            if (n <= 5 || (n % 900) == 1 || el != vr::VRCompositorError_None || er != vr::VRCompositorError_None)
                Game::logMsg("Submit(render thread) #%d waitErr=%d Lerr=%d Rerr=%d eyes=%d",
                             n, (int)werr, (int)el, (int)er, (int)(haveEyes && inMap));
        }
    }

    m_PosesThisFrame = false;

    const float ms = MsSince(t0, vrclock::now());
    static int s_n = 0;
    if ((++s_n) <= 8 || (s_n % 90) == 1 || ms > 50.0f)
        Game::logMsg("AfterPresent tick #%d %.1fms inmap=%d waitErr=%d eyes=%d click=%d",
                     s_n, ms, (int)inMap, (int)werr, (int)haveEyes, (int)clicking);
}

void VR::ShowMenuPanel()
{
    if (!m_Overlay || !m_MainMenuHandle)
        return;

    if (!TextureReady(m_VKHUD))
        return;

    int windowWidth = 1280, windowHeight = 720;
    if (m_Game && m_Game->m_EngineClient)
        m_Game->m_EngineClient->GetScreenSize(windowWidth, windowHeight);

    // ONLY cosmetic properties belong in this once-block. Every setter is an
    // IPC round-trip, but anything affecting input routing is re-asserted per
    // frame in ProcessMenuInput -- hoisting those broke clicking outright.
    if (!m_MenuOverlayConfigured || m_MenuCfgW != windowWidth || m_MenuCfgH != windowHeight)
    {
        const vr::VRTextureBounds_t full = { 0.0f, 0.0f, 1.0f, 1.0f };
        m_Overlay->SetOverlayTextureBounds(m_MainMenuHandle, &full);
        // Full brightness — default overlay shading made the GE:S menu look washed out.
        m_Overlay->SetOverlayAlpha(m_MainMenuHandle, 1.0f);
        m_Overlay->SetOverlayColor(m_MainMenuHandle, 1.0f, 1.0f, 1.0f);
        m_Overlay->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_HideLaserIntersection, false);
        m_Overlay->SetOverlaySortOrder(m_MainMenuHandle, 20);
        m_MenuOverlayConfigured = true;
        m_MenuCfgW = windowWidth;
        m_MenuCfgH = windowHeight;
        Game::logMsg("Menu overlay cosmetics configured %dx%d", windowWidth, windowHeight);
    }

    vr::EVROverlayError err = SetOverlayTextureLocked(m_Overlay, m_MainMenuHandle, &m_VKHUD.m_VRTexture);

    // Keep the panel still. Re-aiming it every frame made the laser unusable.
    float yaw = 0.0f;
    const vr::TrackedDevicePose_t &hmd = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (hmd.bPoseIsValid)
        yaw = atan2f(-hmd.mDeviceToAbsoluteTracking.m[0][2], -hmd.mDeviceToAbsoluteTracking.m[2][2]);
    float dyaw = yaw - g_menuYaw;
    while (dyaw > 3.1416f) dyaw -= 6.2832f;
    while (dyaw < -3.1416f) dyaw += 6.2832f;
    if (!g_menuPlaced || fabsf(dyaw) > 1.2f)
    {
        PlaceMenuPanelInFront();
        g_menuPlaced = true;
        g_menuYaw = yaw;
    }
    // Per-frame, as it was before. If SteamVR ever drops the overlay, gating
    // this on a local "already shown" flag would never bring it back.
    m_Overlay->ShowOverlay(m_MainMenuHandle);
    if (m_HUDHandle)
        m_Overlay->HideOverlay(m_HUDHandle);

    static int s_logged = 0;
    if (s_logged < 3)
    {
        Game::logMsg("Floating menu overlay %dx%d SetOverlayTexture=%d visible=%d",
                     windowWidth, windowHeight, (int)err,
                     (int)m_Overlay->IsOverlayVisible(m_MainMenuHandle));
        ++s_logged;
    }
}

void VR::PlaceMenuPanelInFront()
{
    if (!m_Overlay || !m_MainMenuHandle || !vr::VRCompositor())
        return;

    // Place the panel on the HMD's forward vector so it is actually in view.
    vr::HmdMatrix34_t xf = {
        1.0f, 0.0f, 0.0f,  0.0f,
        0.0f, 1.0f, 0.0f,  1.35f,
        0.0f, 0.0f, 1.0f, -2.1f
    };

    const vr::TrackedDevicePose_t &hmd = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (hmd.bPoseIsValid)
    {
        const vr::HmdMatrix34_t &m = hmd.mDeviceToAbsoluteTracking;
        const float dist = m_MenuDistanceMeters;
        float fx = -m.m[0][2], fy = -m.m[1][2], fz = -m.m[2][2];
        xf.m[0][0] = m.m[0][0]; xf.m[0][1] = m.m[0][1]; xf.m[0][2] = m.m[0][2];
        xf.m[1][0] = m.m[1][0]; xf.m[1][1] = m.m[1][1]; xf.m[1][2] = m.m[1][2];
        xf.m[2][0] = m.m[2][0]; xf.m[2][1] = m.m[2][1]; xf.m[2][2] = m.m[2][2];
        xf.m[0][3] = m.m[0][3] + fx * dist;
        xf.m[1][3] = m.m[1][3] + fy * dist;
        xf.m[2][3] = m.m[2][3] + fz * dist;
    }

    m_Overlay->SetOverlayTransformAbsolute(m_MainMenuHandle, vr::VRCompositor()->GetTrackingSpace(), &xf);
    m_Overlay->SetOverlayWidthInMeters(m_MainMenuHandle, m_MenuWidthMeters);
}

bool VR::ComputeMenuPointer(int &x, int &y)
{
    x = -1;
    y = -1;
    if (!m_Overlay || !m_MainMenuHandle || !m_System || !vr::VRCompositor())
        return false;

    int windowWidth = 1280, windowHeight = 720;
    if (m_Game && m_Game->m_EngineClient)
        m_Game->m_EngineClient->GetScreenSize(windowWidth, windowHeight);
    if (windowWidth < 1) windowWidth = 1280;
    if (windowHeight < 1) windowHeight = 720;

    vr::TrackedDeviceIndex_t rightIdx = m_System->GetTrackedDeviceIndexForControllerRole(
        m_LeftHanded ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
    if (rightIdx >= vr::k_unMaxTrackedDeviceCount || !m_Poses[rightIdx].bPoseIsValid)
        return false;

    const vr::HmdMatrix34_t &ctrl = m_Poses[rightIdx].mDeviceToAbsoluteTracking;
    vr::VROverlayIntersectionParams_t ip{};
    ip.eOrigin = vr::VRCompositor()->GetTrackingSpace();
    ip.vSource.v[0] = ctrl.m[0][3];
    ip.vSource.v[1] = ctrl.m[1][3];
    ip.vSource.v[2] = ctrl.m[2][3];
    ip.vDirection.v[0] = -ctrl.m[0][2];
    ip.vDirection.v[1] = -ctrl.m[1][2];
    ip.vDirection.v[2] = -ctrl.m[2][2];
    vr::VROverlayIntersectionResults_t ir{};
    if (!m_Overlay->ComputeOverlayIntersection(m_MainMenuHandle, &ip, &ir))
        return false;

    float u = ir.vUVs.v[0];
    float v = 1.0f - ir.vUVs.v[1];
    if (u < 0.f) u = 0.f; if (u > 1.f) u = 1.f;
    if (v < 0.f) v = 0.f; if (v > 1.f) v = 1.f;
    x = (int)(u * (float)(windowWidth - 1));
    y = (int)(v * (float)(windowHeight - 1));
    return true;
}

void VR::ShowWorldStereoOverlay()
{
    if (!m_Overlay || !m_WorldHandle || !g_D3DVR9)
        return;
    if (FAILED(g_D3DVR9->BlitEyesSBS(&m_VKWorld)) || !TextureReady(m_VKWorld))
        return;

    m_Overlay->SetOverlayFlag(m_WorldHandle, vr::VROverlayFlags_SideBySide_Parallel, true);
    m_Overlay->SetOverlayFlag(m_WorldHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);
    m_Overlay->SetOverlayInputMethod(m_WorldHandle, vr::VROverlayInputMethod_None);
    m_Overlay->SetOverlaySortOrder(m_WorldHandle, 5);

    // HMD-locked SBS quad. This is the path the headset can actually see
    // (SteamVR Theater covers compositor Scene submits).
    vr::HmdMatrix34_t xf{};
    xf.m[0][0] = xf.m[1][1] = xf.m[2][2] = 1.0f;
    xf.m[2][3] = -m_SbsDistance;
    m_Overlay->SetOverlayTransformTrackedDeviceRelative(
        m_WorldHandle, vr::k_unTrackedDeviceIndex_Hmd, &xf);
    const float widthM = m_SbsWidthMeters;
    m_Overlay->SetOverlayWidthInMeters(m_WorldHandle, widthM);

    vr::EVROverlayError err = SetOverlayTextureLocked(m_Overlay, m_WorldHandle, &m_VKWorld.m_VRTexture);
    m_Overlay->ShowOverlay(m_WorldHandle);

    static int s_log = 0;
    if (s_log < 4)
    {
        Game::logMsg("World SBS overlay SetTexture=%d visible=%d w=%.2f",
                     (int)err, (int)m_Overlay->IsOverlayVisible(m_WorldHandle), widthM);
        ++s_log;
    }
}

// True per-eye stereo. Each eye was rendered at the symmetric superset FOV
// (m_Fov), which covers both eyes' frusta; m_TextureBounds pulls that eye's
// real asymmetric rectangle back out. Submitting the full 0..1 rect instead
// (what this mod did before) stretches a ~106 degree image across an eye that
// is not 106 degrees, which is precisely why the world read as an oversized
// screen rather than a room you are standing in.
void VR::SubmitStereoToCompositor()
{
    // Publish only. The VRSubmit thread applies m_TextureBounds and submits.
    VRSubmit::g_eyesReady.store(TextureReady(m_VKLeftEye) && TextureReady(m_VKRightEye));
}

// Single place that decides how a finished stereo frame reaches the headset.
void VR::PresentStereo()
{
    const bool haveEyes = TextureReady(m_VKLeftEye) && TextureReady(m_VKRightEye);
    if (!haveEyes)
    {
        HideWorldOverlay();
        GESVR_SubmitBackBufferFallback();
        return;
    }

    switch (m_DisplayMode)
    {
    case Display_SBS:
        // Legacy path: quad carries the image, so Scene must stay black or the
        // compositor draws a second copy behind it.
        ShowWorldStereoOverlay();
        SubmitBlackEyes();
        break;

    case Display_Both:
        SubmitStereoToCompositor();
        ShowWorldStereoOverlay();
        break;

    case Display_Compositor:
    default:
        HideWorldOverlay();
        SubmitStereoToCompositor();
        break;
    }
}

void VR::HideWorldOverlay()
{
    if (!m_Overlay)
        return;
    if (m_WorldHandle)
        m_Overlay->HideOverlay(m_WorldHandle);
}

void VR::HideMenuPanel()
{
    g_menuPlaced = false;
    m_MenuOverlayShown = false;
    if (!m_Overlay)
        return;
    if (m_MainMenuHandle)
    {
        m_Overlay->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);
        m_Overlay->HideOverlay(m_MainMenuHandle);
    }
    if (m_HUDHandle)
        m_Overlay->HideOverlay(m_HUDHandle);
}

// Render-thread only: creates/fills the black surface via D3D and caches the
// resulting Vulkan handle for the submit thread. The submit thread must never
// touch g_D3DVR9.
void VR::PrepareBlackTexture()
{
    if (m_BlackPrepared || !g_D3DVR9)
        return;
    if (FAILED(g_D3DVR9->GetBlackTexture(&m_SubmitBlack)) || !TextureReady(m_SubmitBlack))
        return;
    m_BlackPrepared = true;
    VRSubmit::g_blackReady.store(true);
    Game::logMsg("Black keepalive texture prepared on the render thread");
}

// Runs on the submit thread. WaitGetPoses blocks here at display cadence, which
// is exactly where blocking belongs -- off the game's render thread.
void VR::SubmitThreadBody()
{
    while (VRSubmit::g_run.load())
    {
        auto *comp = vr::VRCompositor();
        if (!comp)
        {
            Sleep(10);
            continue;
        }

        static vr::TrackedDevicePose_t s_poses[vr::k_unMaxTrackedDeviceCount];
        vr::EVRCompositorError werr = comp->WaitGetPoses(s_poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        {
            // Held only for the copy. Never across WaitGetPoses or Submit.
            std::lock_guard<std::mutex> lk(VRSubmit::g_poseLock);
            memcpy(m_Poses, s_poses, sizeof(m_Poses));
        }

        vr::EVRCompositorError el = vr::VRCompositorError_None;
        vr::EVRCompositorError er = vr::VRCompositorError_None;

        if (VRSubmit::g_eyesReady.load() && TextureReady(m_VKLeftEye) && TextureReady(m_VKRightEye))
        {
            const vr::VRTextureBounds_t full = { 0.0f, 0.0f, 1.0f, 1.0f };
            const bool useBounds = m_UseTextureBounds && m_HaveTextureBounds;
            vr::VRTextureBounds_t lb = useBounds ? m_TextureBounds[0] : full;
            vr::VRTextureBounds_t rb = useBounds ? m_TextureBounds[1] : full;
            if (!m_UseVerticalCrop)
            {
                lb.vMin = rb.vMin = 0.0f;
                lb.vMax = rb.vMax = 1.0f;
            }
            // Vulkan queues are single-thread-access. Bracket OpenVR's submit so it
            // cannot race DXVK's submission thread on the same VkQueue.
            if (g_D3DVR9) g_D3DVR9->LockSubmission();
            el = comp->Submit(vr::Eye_Left,  &m_VKLeftEye.m_VRTexture,  &lb, vr::Submit_Default);
            er = comp->Submit(vr::Eye_Right, &m_VKRightEye.m_VRTexture, &rb, vr::Submit_Default);
            if (g_D3DVR9) g_D3DVR9->UnlockSubmission();
        }
        else if (VRSubmit::g_blackReady.load() && TextureReady(m_SubmitBlack))
        {
            const vr::VRTextureBounds_t full = { 0.0f, 0.0f, 1.0f, 1.0f };
            if (g_D3DVR9) g_D3DVR9->LockSubmission();
            el = comp->Submit(vr::Eye_Left,  &m_SubmitBlack.m_VRTexture, &full, vr::Submit_Default);
            er = comp->Submit(vr::Eye_Right, &m_SubmitBlack.m_VRTexture, &full, vr::Submit_Default);
            if (g_D3DVR9) g_D3DVR9->UnlockSubmission();
        }
        else
        {
            Sleep(5);
            continue;
        }

        const int n = VRSubmit::g_frames.fetch_add(1) + 1;
        VRSubmit::g_lastErr.store((int)el);
        if (n <= 5 || (n % 900) == 1 || el != vr::VRCompositorError_None || er != vr::VRCompositorError_None)
            Game::logMsg("VRSubmit #%d waitErr=%d Lerr=%d Rerr=%d eyes=%d",
                         n, (int)werr, (int)el, (int)er, (int)VRSubmit::g_eyesReady.load());
    }
}

void VR::SubmitBlackEyes()
{
    // Publish only -- the VRSubmit thread performs the actual Submit. Calling
    // vr::VRCompositor()->Submit() from the render thread is what deadlocked
    // the game against DXVK Present (13:26 log).
    PrepareBlackTexture();
    VRSubmit::g_eyesReady.store(false);
}

void VR::CreateVRTextures()
{
    int windowWidth = 1280, windowHeight = 720;
    if (m_Game->m_EngineClient)
        m_Game->m_EngineClient->GetScreenSize(windowWidth, windowHeight);

    // SDK 2007 CMaterialSystem::m_bGameRunning is not at L4D2's 0x2AB8.
    // Try a normal runtime RT allocation; GE:S still accepts this on 2007.
    m_Game->m_MaterialSystem->BeginRenderTargetAllocation();

    m_CreatingTextureID = Texture_LeftEye;
    m_LeftEyeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("leftEye0", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);
    
    m_CreatingTextureID = Texture_RightEye;
    m_RightEyeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("rightEye0", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);
    
    m_CreatingTextureID = Texture_HUD;
    m_HUDTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("vrHUD", windowWidth, windowHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);
    
    m_CreatingTextureID = Texture_Blank;
    m_BlankTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("blankTexture", 512, 512, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);
    
    m_CreatingTextureID = Texture_None;

    m_Game->m_MaterialSystem->EndRenderTargetAllocation();

    m_CreatedVRTextures = (m_LeftEyeTexture && m_RightEyeTexture);
    Game::logMsg("CreateVRTextures left=%p right=%p hud=%p ok=%d size=%ux%u",
                 m_LeftEyeTexture, m_RightEyeTexture, m_HUDTexture,
                 (int)m_CreatedVRTextures, m_RenderWidth, m_RenderHeight);
}

void VR::SubmitVRTextures()
{
    // DEAD CODE, kept only so the name resolves. It has had no callers for the
    // life of this project, and it used to Submit() directly from whatever
    // thread called it. Do not revive it: all compositor submission belongs to
    // VR::SubmitThreadBody. Left here rather than deleted because it is exactly
    // the kind of function someone re-wires by accident.
}

void VR::GetPoseData(vr::TrackedDevicePose_t &poseRaw, TrackedDevicePoseData &poseOut)
{
    if (poseRaw.bPoseIsValid) 
    {
        vr::HmdMatrix34_t mat = poseRaw.mDeviceToAbsoluteTracking;
        Vector pos;
        Vector vel;
        QAngle ang;
        QAngle angvel;
        pos.x = -mat.m[2][3];
        pos.y = -mat.m[0][3];
        pos.z = mat.m[1][3];
        ang.x = asin(mat.m[1][2]) * (180.0 / 3.141592654);
        ang.y = atan2f(mat.m[0][2], mat.m[2][2]) * (180.0 / 3.141592654);
        ang.z = atan2f(-mat.m[1][0], mat.m[1][1]) * (180.0 / 3.141592654);
        vel.x = -poseRaw.vVelocity.v[2];
        vel.y = -poseRaw.vVelocity.v[0];
        vel.z = poseRaw.vVelocity.v[1];
        angvel.x = -poseRaw.vAngularVelocity.v[2] * (180.0 / 3.141592654);
        angvel.y = -poseRaw.vAngularVelocity.v[0] * (180.0 / 3.141592654);
        angvel.z = poseRaw.vAngularVelocity.v[1] * (180.0 / 3.141592654);

        poseOut.TrackedDevicePos = pos;
        poseOut.TrackedDeviceVel = vel;
        poseOut.TrackedDeviceAng = ang;
        poseOut.TrackedDeviceAngVel = angvel;
    }
}

void VR::RepositionOverlays()
{
    vr::TrackedDevicePose_t hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
    vr::HmdMatrix34_t hmdMat = hmdPose.mDeviceToAbsoluteTracking;
    Vector hmdPosition = { hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3] };
    Vector hmdForward = { -hmdMat.m[0][2], 0, -hmdMat.m[2][2] };

    int windowWidth = 1280, windowHeight = 720;
    if (m_Game->m_EngineClient)
        m_Game->m_EngineClient->GetScreenSize(windowWidth, windowHeight);

    vr::HmdMatrix34_t menuTransform = 
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f, 1.0f
    };

    vr::ETrackingUniverseOrigin trackingOrigin = vr::VRCompositor()->GetTrackingSpace();

    // Reposition main menu overlay
    float renderWidth = (float)m_VKBackBuffer.m_VulkanData.m_nWidth;
    float renderHeight = (float)m_VKBackBuffer.m_VulkanData.m_nHeight;
    if (renderWidth < 1.0f) renderWidth = (float)windowWidth;
    if (renderHeight < 1.0f) renderHeight = (float)windowHeight;

    float widthRatio = windowWidth / renderWidth;
    float heightRatio = windowHeight / renderHeight;
    menuTransform.m[0][0] *= widthRatio;
    menuTransform.m[1][1] *= heightRatio;

    hmdForward[1] = 0;
    VectorNormalize(hmdForward);

    Vector menuDistance = hmdForward * 2.2f;
    Vector menuNewPos = menuDistance + hmdPosition;

    menuTransform.m[0][3] = menuNewPos.x;
    menuTransform.m[1][3] = menuNewPos.y - 0.25;
    menuTransform.m[2][3] = menuNewPos.z;

    float xScale = menuTransform.m[0][0];
    float hmdRotationDegrees = atan2f(hmdMat.m[0][2], hmdMat.m[2][2]);

    menuTransform.m[0][0] *= cos(hmdRotationDegrees);
    menuTransform.m[0][2] = sin(hmdRotationDegrees);
    menuTransform.m[2][0] = -sin(hmdRotationDegrees) * xScale;
    menuTransform.m[2][2] *= cos(hmdRotationDegrees);

    vr::VROverlay()->SetOverlayTransformAbsolute(m_MainMenuHandle, trackingOrigin, &menuTransform);
    vr::VROverlay()->SetOverlayWidthInMeters(m_MainMenuHandle, 1.8f);

    // Reposition HUD overlay
    vr::HmdMatrix34_t hudTransform =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };

    Vector hudDistance = hmdForward * m_HudDistance;
    Vector hudNewPos = hudDistance + hmdPosition;

    hudTransform.m[0][3] = hudNewPos.x;
    hudTransform.m[1][3] = hudNewPos.y - 0.25;
    hudTransform.m[2][3] = hudNewPos.z;

    hudTransform.m[0][0] *= cos(hmdRotationDegrees);
    hudTransform.m[0][2] = sin(hmdRotationDegrees);
    hudTransform.m[2][0] = -sin(hmdRotationDegrees);
    hudTransform.m[2][2] *= cos(hmdRotationDegrees);

    vr::VROverlay()->SetOverlayTransformAbsolute(m_HUDHandle, trackingOrigin, &hudTransform);
    vr::VROverlay()->SetOverlayWidthInMeters(m_HUDHandle, m_HudSize);
}

void VR::GetPoses() 
{
    vr::TrackedDevicePose_t hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];

    vr::TrackedDeviceIndex_t leftControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
    vr::TrackedDeviceIndex_t rightControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);

    if (m_LeftHanded)
        std::swap(leftControllerIndex, rightControllerIndex);

    vr::TrackedDevicePose_t leftControllerPose = m_Poses[leftControllerIndex];
    vr::TrackedDevicePose_t rightControllerPose = m_Poses[rightControllerIndex];

    GetPoseData(hmdPose, m_HmdPose);
    GetPoseData(leftControllerPose, m_LeftControllerPose);
    GetPoseData(rightControllerPose, m_RightControllerPose);
}

void VR::UpdatePosesAndActions() 
{
    // Poses now arrive from the VRSubmit thread's WaitGetPoses. Calling it here
    // put a blocking compositor call on the engine's render thread, which is
    // the deadlock this whole rework exists to remove. Action state is a plain
    // IPC read and is safe.
    if (m_Input)
        m_Input->UpdateActionState(&m_ActiveActionSet, sizeof(vr::VRActiveActionSet_t), 1);
    m_PosesThisFrame = true;
}

void VR::GetViewParameters() 
{
    vr::HmdMatrix34_t eyeToHeadLeft = m_System->GetEyeToHeadTransform(vr::Eye_Left);
    vr::HmdMatrix34_t eyeToHeadRight = m_System->GetEyeToHeadTransform(vr::Eye_Right);
    m_EyeToHeadTransformPosLeft.x = eyeToHeadLeft.m[0][3];
    m_EyeToHeadTransformPosLeft.y = eyeToHeadLeft.m[1][3];
    m_EyeToHeadTransformPosLeft.z = eyeToHeadLeft.m[2][3];

    m_EyeToHeadTransformPosRight.x = eyeToHeadRight.m[0][3];
    m_EyeToHeadTransformPosRight.y = eyeToHeadRight.m[1][3];
    m_EyeToHeadTransformPosRight.z = eyeToHeadRight.m[2][3];
}

// Reads the trigger straight off the device with the legacy controller API,
// bypassing the action manifest entirely. The action system has produced
// sel=0/atk=0 on every sample while the laser tracked fine (moves climbing), so
// this exists both as a diagnostic and as a working fallback if the action set
// turns out to be the broken link.
bool VR::LegacyTriggerDown(float *outValue)
{
    if (outValue) *outValue = 0.0f;
    if (!m_System)
        return false;
    bool any = false;
    for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i)
    {
        if (m_System->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller)
            continue;
        vr::VRControllerState_t st{};
        if (!m_System->GetControllerState(i, &st, sizeof(st)))
            continue;
        const float axis = st.rAxis[1].x;   // trigger axis on every OpenVR controller
        const bool pressed = (st.ulButtonPressed &
            vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0;
        if (outValue && axis > *outValue) *outValue = axis;
        if (pressed || axis > 0.6f)
            any = true;
    }
    return any;
}

bool VR::PressedDigitalAction(vr::VRActionHandle_t &actionHandle, bool checkIfActionChanged)
{
    if (!m_Input)
        return false;
    vr::InputDigitalActionData_t digitalActionData;
    vr::EVRInputError result = m_Input->GetDigitalActionData(actionHandle, &digitalActionData, sizeof(digitalActionData), vr::k_ulInvalidInputValueHandle);
    
    if (result == vr::VRInputError_None)
    {
        if (checkIfActionChanged)
            return digitalActionData.bState && digitalActionData.bChanged;
        else
            return digitalActionData.bState;
    }

    return false;
}

bool VR::GetAnalogActionData(vr::VRActionHandle_t &actionHandle, vr::InputAnalogActionData_t &analogDataOut)
{
    vr::EVRInputError result = m_Input->GetAnalogActionData(actionHandle, &analogDataOut, sizeof(analogDataOut), vr::k_ulInvalidInputValueHandle);

    if (result == vr::VRInputError_None)
        return true;

    return false;
}

// Source reads the menu cursor two ways: vgui pulls the OS cursor through
// CMatSystemSurface::SurfaceGetCursorPos (so Win32 SetCursorPos matters), and
// inputsystem consumes window messages (so WM_MOUSEMOVE matters). Drive both.
//
// IMPORTANT: do NOT require the game to be foreground. With the HMD on,
// SteamVR almost always owns focus, so a foreground check disables the laser
// entirely ("controllers can't highlight or select").
// CORRECTION (13:12 log): SetCursorPos and PostMessage are NOT safe from
// Present either. Driving them from inside PresentEx hung the main thread
// permanently the moment menu input went live. Every USER32 call now lives
// on the MenuInput worker thread; this function only publishes an aim point.
static void DriveGameCursor(HWND /*hwnd*/, int x, int y, IInput *input, bool useVguiInternal)
{
    g_lastCursorX = x;
    g_lastCursorY = y;
    g_haveCursor = true;

    // Publish only. All USER32 work happens on the MenuInput thread -- doing it
    // here meant doing it inside PresentEx, which deadlocked the main thread.
    if (g_menuDriveCursor)
        MenuInput::SetAim(x, y);

    if (!useVguiInternal || !input)
        return;
    __try
    {
        input->SetCursorPos(x, y);
        input->InternalCursorMoved(x, y);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// Source's inputsystem window proc turns these into IE_ButtonPressed events.
// PostMessage works even when SteamVR owns focus; do not gate on foreground.
static void PostClickToGameWindow(HWND /*hwnd*/)
{
    if (!g_haveCursor || !g_menuDriveCursor)
        return;
    MenuInput::QueueClick();
}

static void PostKeyToGameWindow(HWND /*hwnd*/, WPARAM vk)
{
    MenuInput::QueueKey((int)vk);
}

static void SafeVguiMouse(IInput *input, bool down)
{
    if (!input)
        return;
    __try
    {
        if (down)
            input->InternalMousePressed(ButtonCode_t::MOUSE_LEFT);
        else
            input->InternalMouseReleased(ButtonCode_t::MOUSE_LEFT);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static void SafeVguiClick(IInput *input)
{
    SafeVguiMouse(input, true);
    SafeVguiMouse(input, false);
}

static void SafeVguiKey(IInput *input, ButtonCode_t key)
{
    if (!input)
        return;
    __try
    {
        input->InternalKeyCodeTyped(key);
        input->InternalKeyCodePressed(key);
        input->InternalKeyCodeReleased(key);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void VR::ProcessMenuInput()
{
    if (!m_Overlay || !m_MainMenuHandle || !m_Game)
        return;

    int windowWidth = 1280, windowHeight = 720;
    if (m_Game->m_EngineClient)
        m_Game->m_EngineClient->GetScreenSize(windowWidth, windowHeight);
    if (windowWidth < 1) windowWidth = 1280;
    if (windowHeight < 1) windowHeight = 720;

    IInput *input = m_Game->m_VguiInput;
    // Resolved and refreshed by the MenuInput thread; read the published
    // handle rather than calling FindWindow/IsWindow from Present.
    HWND hwnd = (HWND)MenuInput::g_hwnd.load();
    // These three are re-asserted EVERY frame on purpose. Hoisting them into a
    // configure-once block (12:03 build) coincided with SteamVR delivering no
    // VREvent_MouseButtonDown at all -- the menu became unclickable. Overlay
    // input routing is evidently not as sticky as the cosmetic properties, so
    // the IPC cost here is paid deliberately. Do not "optimize" these again
    // without a headset test proving clicks still land.
    const vr::HmdVector2_t mouseScale = { (float)windowWidth, (float)windowHeight };
    m_Overlay->SetOverlayMouseScale(m_MainMenuHandle, &mouseScale);
    m_Overlay->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
    m_Overlay->SetOverlayInputMethod(m_MainMenuHandle, vr::VROverlayInputMethod_Mouse);

    // Let GameUI finish building the main-menu panel list before we touch the
    // Win32 cursor/message queue. Flooding SetCursorPos/PostMessage from Present
    // (55+ overlay moves in one frame) stalled the engine ~3s and froze the
    // overlay on the title backdrop with no menu items.
    // Two separate arming points. Until now BOTH the first SetCursorPos (via
    // the MenuInput worker) and per-frame OpenVR action polling switched on at
    // frame 90, and the main thread died on frame 91 -- with no way to tell
    // which. Cursor arms at 90, action polling at 300 (~2s later). Whichever
    // frame the log stops on now identifies the culprit by itself.
    static int s_menuFrames = 0;
    ++s_menuFrames;
    const bool cursorLive  = (s_menuFrames >= 90);
    const bool actionsLive = (s_menuFrames >= 300);
    const bool inputLive = cursorLive;
    const bool armTrace = (s_menuFrames >= 88 && s_menuFrames <= 95)
                       || (s_menuFrames >= 298 && s_menuFrames <= 305);
    if (s_menuFrames == 90)
        Game::logMsg("ARM: cursor publishing ON (frame 90)");
    if (s_menuFrames == 300)
        Game::logMsg("ARM: OpenVR action polling ON (frame 300)");

    // Drain overlay events but coalesce to a single aim point per frame.
    int aimX = -1, aimY = -1;
    int overlayMoves = 0;
    bool overlayDown = false, overlayUp = false;
    vr::VREvent_t ev{};
    while (m_Overlay->PollNextOverlayEvent(m_MainMenuHandle, &ev, sizeof(ev)))
    {
        switch (ev.eventType)
        {
        case vr::VREvent_MouseMove:
        {
            int laserX = (int)ev.data.mouse.x;
            int laserY = windowHeight - (int)ev.data.mouse.y;
            if (laserX < 0) laserX = 0;
            if (laserY < 0) laserY = 0;
            if (laserX >= windowWidth) laserX = windowWidth - 1;
            if (laserY >= windowHeight) laserY = windowHeight - 1;
            aimX = laserX;
            aimY = laserY;
            ++overlayMoves;
            break;
        }
        case vr::VREvent_MouseButtonDown:
            overlayDown = true;
            break;
        case vr::VREvent_MouseButtonUp:
            overlayUp = true;
            break;
        default:
            break;
        }
    }

    int tipX = -1, tipY = -1;
    const bool tipHit = ComputeMenuPointer(tipX, tipY);
    if (tipHit)
    {
        aimX = tipX;
        aimY = tipY;
    }

    if (armTrace) Game::logMsg("  f=%d -> publishing aim (%d,%d)", s_menuFrames, aimX, aimY);
    if (cursorLive && aimX >= 0)
        DriveGameCursor(hwnd, aimX, aimY, input, m_MenuUseVguiInternal);
    if (armTrace) Game::logMsg("  f=%d <- aim published", s_menuFrames);

    g_pendHwnd = hwnd;
    if (inputLive && overlayDown)
    {
        g_pendMouseDown = true;
        if (m_MenuUseVguiInternal)
            SafeVguiMouse(input, true);
        Game::logMsg("Overlay MouseButtonDown at (%d,%d)", g_lastCursorX, g_lastCursorY);
    }
    if (inputLive && overlayUp)
    {
        g_pendMouseUp = true;
        if (m_MenuUseVguiInternal)
            SafeVguiMouse(input, false);
    }

    static int s_aimLog = 0;
    if (s_aimLog < 8 && (overlayMoves > 0 || tipHit))
    {
        Game::logMsg("Menu aim live=%d frames=%d overlayMoves=%d tip=%d -> (%d,%d) fg=%d",
                     (int)inputLive, s_menuFrames, overlayMoves, (int)tipHit,
                     aimX, aimY, MenuInput::g_foreground.load());
        ++s_aimLog;
    }
    if (s_menuFrames == 90)
        Game::logMsg("Menu input armed after warmup (%d frames)", s_menuFrames);

    // The in-game character/level menu is reported as unclickable even though
    // it should take this exact path (GameUI visible => menu mode). Log the
    // in-map case specifically: if this never prints, ProcessMenuInput is not
    // running there and the cause is upstream in IsMenuMode(); if it prints
    // with moves=0, SteamVR is not routing the laser to our overlay in map.
    {
        static bool s_loggedInMap = false;
        const bool inMapNow = m_Game && m_Game->IsInMap();
        if (inMapNow && !s_loggedInMap)
        {
            s_loggedInMap = true;
            Game::logMsg("IN-MAP MENU active: overlayMoves=%d tip=%d live=%d vis=%d",
                         overlayMoves, (int)tipHit, (int)cursorLive,
                         (int)(m_Overlay && m_Overlay->IsOverlayVisible(m_MainMenuHandle)));
        }
        if (!inMapNow)
            s_loggedInMap = false;
    }

    // Menu input health, once a second. The 12:03 run produced NEITHER an
    // overlay MouseButtonDown NOR a digital-action press when the trigger was
    // pulled, which are two independent paths -- so this reports both, plus
    // whether the action handles resolved at all. If actionsOk=0 the action
    // manifest or the controller bindings never loaded, and no amount of
    // overlay work will make the trigger click.
    {
        static DWORD s_lastHealth = 0;
        static int s_moveTotal = 0, s_downTotal = 0, s_upTotal = 0;
        s_moveTotal += overlayMoves;
        s_downTotal += overlayDown ? 1 : 0;
        s_upTotal   += overlayUp ? 1 : 0;
        const DWORD hnow = GetTickCount();
        if (s_lastHealth == 0 || (hnow - s_lastHealth) >= 1000)
        {
            s_lastHealth = hnow;
            const bool actionsOk = (m_MenuSelect != vr::k_ulInvalidActionHandle)
                                && (m_ActionPrimaryAttack != vr::k_ulInvalidActionHandle);
            // bActive is the field that matters: false means the action is not
            // bound to anything on the current controller, which is
            // indistinguishable from "not pressed" everywhere else in this code.
            bool selDown = false, atkDown = false;
            int selErr = -1, atkErr = -1, selActive = -1, atkActive = -1;
            if (m_Input)
            {
                vr::InputDigitalActionData_t d{};
                selErr = (int)m_Input->GetDigitalActionData(m_MenuSelect, &d, sizeof(d), vr::k_ulInvalidInputValueHandle);
                selDown = d.bState; selActive = (int)d.bActive;
                vr::InputDigitalActionData_t d2{};
                atkErr = (int)m_Input->GetDigitalActionData(m_ActionPrimaryAttack, &d2, sizeof(d2), vr::k_ulInvalidInputValueHandle);
                atkDown = d2.bState; atkActive = (int)d2.bActive;
            }
            float legacyAxis = 0.0f;
            const bool legacyDown = LegacyTriggerDown(&legacyAxis);
            // Is the controller even tracking? ComputeMenuPointer (tip) has
            // never once hit, and it is the aim path that does NOT depend on
            // SteamVR routing laser events to us.
            bool ctrlPoseValid = false;
            if (m_System)
            {
                vr::TrackedDeviceIndex_t ci = m_System->GetTrackedDeviceIndexForControllerRole(
                    m_LeftHanded ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
                ctrlPoseValid = (ci < vr::k_unMaxTrackedDeviceCount) && m_Poses[ci].bPoseIsValid;
            }
            bool interactive = false;
            if (m_Overlay)
                m_Overlay->GetOverlayFlag(m_MainMenuHandle,
                    vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, &interactive);
            Game::logMsg("MenuHealth live=%d moves=%d down=%d up=%d | sel=%d(act=%d err=%d) atk=%d(act=%d err=%d) LEGACY=%d(%.2f) | interactive=%d vis=%d tip=%d ctrlPose=%d aim=(%d,%d)",
                         (int)inputLive, s_moveTotal, s_downTotal, s_upTotal,
                         (int)selDown, selActive, selErr,
                         (int)atkDown, atkActive, atkErr,
                         (int)legacyDown, legacyAxis,
                         (int)interactive,
                         (int)(m_Overlay && m_Overlay->IsOverlayVisible(m_MainMenuHandle)),
                         (int)tipHit, (int)ctrlPoseValid,
                         g_lastCursorX, g_lastCursorY);
        }
    }

    static bool s_loggedWindow = false;
    if (!s_loggedWindow)
    {
        Game::logMsg("Menu input: hwnd=%p win32=%d vguiInternal=%d vguiInput=%p size=%dx%d",
                     hwnd, (int)m_MenuUseWin32, (int)m_MenuUseVguiInternal,
                     input, windowWidth, windowHeight);
        s_loggedWindow = true;
    }

    if (!m_Input || !actionsLive)
        return;

    static DWORD s_lastClick = 0;
    if (armTrace) Game::logMsg("  f=%d -> polling OpenVR actions", s_menuFrames);
    // Legacy trigger is included as a click source. The action-system path has
    // never once produced a press in any log, while the overlay laser tracks
    // fine -- so until the action set is proven working, the direct device read
    // is what actually lets you click a menu item.
    static bool s_legacyPrev = false;
    const bool legacyNow = LegacyTriggerDown();
    const bool legacyEdge = legacyNow && !s_legacyPrev;
    s_legacyPrev = legacyNow;

    const bool pressed = PressedDigitalAction(m_MenuSelect, true)
        || PressedDigitalAction(m_ActionPrimaryAttack, true)
        || legacyEdge;
    if (armTrace) Game::logMsg("  f=%d <- actions polled", s_menuFrames);
    const DWORD now = GetTickCount();
    if (pressed && (now - s_lastClick) > 350)
    {
        g_pendHwnd = hwnd;
        g_pendMouseDown = true;
        g_pendMouseUp = true;
        if (m_MenuUseVguiInternal)
            SafeVguiClick(input);
        s_lastClick = now;
        static int s_clicks = 0;
        if (s_clicks < 10)
        {
            Game::logMsg("Menu click #%d at (%d,%d) hwnd=%p fg=%d",
                         s_clicks, g_lastCursorX, g_lastCursorY, hwnd,
                         MenuInput::g_foreground.load());
            ++s_clicks;
        }
    }

    static DWORD s_lastBack = 0;
    if (PressedDigitalAction(m_MenuBack, true) || PressedDigitalAction(m_Pause, true))
    {
        if ((now - s_lastBack) > 350)
        {
            if (m_MenuUseWin32)
                PostKeyToGameWindow(hwnd, VK_ESCAPE);
            if (m_MenuUseVguiInternal)
                SafeVguiKey(input, ButtonCode_t::KEY_ESCAPE);
            s_lastBack = now;
        }
    }
}

void VR::MoveCmd(const char *cmd)
{
    if (!m_Game || !cmd || !cmd[0])
        return;
    if (cmd[0] != '+' && cmd[0] != '-')
    {
        m_Game->ClientCmd_Unrestricted(cmd);
        return;
    }
    // "+forward" / "-forward" share the key "forward"; only a change is sent.
    static std::unordered_map<std::string, bool> s_state;
    const bool on = (cmd[0] == '+');
    const std::string key(cmd + 1);
    auto it = s_state.find(key);
    if (it != s_state.end() && it->second == on)
        return;
    s_state[key] = on;
    m_Game->ClientCmd_Unrestricted(cmd);
}

void VR::ProcessInput()
{
    if (!m_IsVREnabled)
        return;

    vr::VROverlay()->SetOverlayFlag(m_HUDHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);

    typedef std::chrono::duration<float, std::milli> duration;
    auto currentTime = std::chrono::steady_clock::now();
    duration elapsed = currentTime - m_PrevFrameTime;
    float deltaTime = elapsed.count();
    m_PrevFrameTime = currentTime;

    vr::InputAnalogActionData_t analogActionData;

    if (GetAnalogActionData(m_ActionTurn, analogActionData))
    {
        if (m_SnapTurning)
        {
            if (!m_PressedTurn && analogActionData.x > 0.5)
            {
                m_RotationOffset -= m_SnapTurnAngle;
                m_PressedTurn = true;
            }
            else if (!m_PressedTurn && analogActionData.x < -0.5)
            {
                m_RotationOffset += m_SnapTurnAngle;
                m_PressedTurn = true;
            }
            else if (analogActionData.x < 0.3 && analogActionData.x > -0.3)
                m_PressedTurn = false;
        }
        // Smooth turning
        else
        {
            float deadzone = 0.2;
            // smoother turning
            float xNormalized = (abs(analogActionData.x) - deadzone) / (1 - deadzone);
            if (analogActionData.x > deadzone)
            {
                m_RotationOffset -= m_TurnSpeed * deltaTime * xNormalized;
            }
            if (analogActionData.x < -deadzone)
            {
                m_RotationOffset += m_TurnSpeed * deltaTime * xNormalized;
            }
        }

        // Wrap from 0 to 360
        m_RotationOffset -= 360 * std::floor(m_RotationOffset / 360);
    }

    // TODO: Instead of ClientCmding, override Usercmd in CreateMove
    if (GetAnalogActionData(m_ActionWalk, analogActionData))		
    {
        bool pushingStickX = true;
        bool pushingStickY = true;
        if (analogActionData.y > 0.5)	
        {
            MoveCmd("-back");
            MoveCmd("+forward");
        }
        else if (analogActionData.y < -0.5)		
        {
            MoveCmd("-forward");
            MoveCmd("+back");
        }
        else
        {
            MoveCmd("-back");
            MoveCmd("-forward");
            pushingStickY = false;
        }

        if (analogActionData.x > 0.5)		
        {
            MoveCmd("-moveleft");
            MoveCmd("+moveright");
        }
        else if (analogActionData.x < -0.5)		
        {
            MoveCmd("-moveright");
            MoveCmd("+moveleft");
        }
        else
        {
            MoveCmd("-moveright");
            MoveCmd("-moveleft");
            pushingStickX = false;
        }

        m_PushingThumbstick = pushingStickX || pushingStickY;
    }
    else
    {
        MoveCmd("-forward");
        MoveCmd("-back");
        MoveCmd("-moveleft");
        MoveCmd("-moveright");
    }

    if (PressedDigitalAction(m_ActionPrimaryAttack))
    {
        MoveCmd("+attack");
    }
    else
    {
        MoveCmd("-attack");
    }

    if (PressedDigitalAction(m_ActionJump))
    {
        MoveCmd("+jump");
    }
    else
    {
        MoveCmd("-jump");
    }

    if (PressedDigitalAction(m_ActionUse))
    {
        MoveCmd("+use");
    }
    else
    {
        MoveCmd("-use");
    }

    // Manual reload: bring the magazines together (hands close) or press the bound button.
    const float reloadDist = VectorLength(m_RightControllerPosAbs - m_LeftControllerPosAbs);
    const bool reloadGesture = reloadDist < 9.0f;
    if (reloadGesture && !m_ReloadGestureLatched)
    {
        m_ReloadGestureLatched = true;
        MoveCmd("+reload");
    }
    else if (!reloadGesture)
    {
        m_ReloadGestureLatched = false;
    }

    if (PressedDigitalAction(m_ActionReload) || m_ReloadGestureLatched)
    {
        MoveCmd("+reload");
    }
    else
    {
        MoveCmd("-reload");
    }

    if (PressedDigitalAction(m_ActionSecondaryAttack))
    {
        MoveCmd("+attack2");
    }
    else
    {
        MoveCmd("-attack2");
    }

    if (PressedDigitalAction(m_ActionPrevItem, true))
    {
        m_Game->ClientCmd_Unrestricted("invprev");
    }
    else if (PressedDigitalAction(m_ActionNextItem, true))
    {
        m_Game->ClientCmd_Unrestricted("invnext");
    }

    if (PressedDigitalAction(m_ActionResetPosition, true))
    {
        ResetPosition();
    }

    if (PressedDigitalAction(m_ActionCrouch))
    {
        MoveCmd("+duck");
    }
    else
    {
        MoveCmd("-duck");
    }

    if (PressedDigitalAction(m_ActionFlashlight, true))
    {
        m_Game->ClientCmd_Unrestricted("impulse 100");
    }

    if (PressedDigitalAction(m_Spray, true))
    {
        m_Game->ClientCmd_Unrestricted("impulse 201");
    }
    
    // Full floating HUD is opt-in (ShowHUD / scoreboard / always-on).
    // Everyday HUD lives on the off-hand watch; health also flashes in front
    // of the HMD when you take a hit (see UpdateHurtHUD).
    const bool wantFullHud = PressedDigitalAction(m_ShowHUD) || PressedDigitalAction(m_Scoreboard) || m_HudAlwaysVisible;
    if (wantFullHud && m_RenderedHud)
    {
        RepositionOverlays();

        if (PressedDigitalAction(m_Scoreboard))
            MoveCmd("+showscores");
        else
            MoveCmd("-showscores");

        vr::VROverlay()->ShowOverlay(m_HUDHandle);
    }
    else
    {
        vr::VROverlay()->HideOverlay(m_HUDHandle);
        MoveCmd("-showscores");
    }
    m_RenderedHud = false;

    if (PressedDigitalAction(m_Pause, true))
    {
        m_Game->ClientCmd_Unrestricted("gameui_activate");
        RepositionOverlays();
    }
}

VMatrix VR::VMatrixFromHmdMatrix(const vr::HmdMatrix34_t &hmdMat)
{
    // VMatrix has a different implicit coordinate system than HmdMatrix34_t, but this function does not convert between them
    VMatrix vMat(
        hmdMat.m[0][0], hmdMat.m[1][0], hmdMat.m[2][0], 0.0f,
        hmdMat.m[0][1], hmdMat.m[1][1], hmdMat.m[2][1], 0.0f,
        hmdMat.m[0][2], hmdMat.m[1][2], hmdMat.m[2][2], 0.0f,
        hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3], 1.0f
    );

    return vMat;
}

vr::HmdMatrix34_t VR::VMatrixToHmdMatrix(const VMatrix &vMat)
{
    vr::HmdMatrix34_t hmdMat = {0};

    hmdMat.m[0][0] = vMat.m[0][0];
    hmdMat.m[1][0] = vMat.m[0][1];
    hmdMat.m[2][0] = vMat.m[0][2];

    hmdMat.m[0][1] = vMat.m[1][0];
    hmdMat.m[1][1] = vMat.m[1][1];
    hmdMat.m[2][1] = vMat.m[1][2];

    hmdMat.m[0][2] = vMat.m[2][0];
    hmdMat.m[1][2] = vMat.m[2][1];
    hmdMat.m[2][2] = vMat.m[2][2];

    hmdMat.m[0][3] = vMat.m[3][0];
    hmdMat.m[1][3] = vMat.m[3][1];
    hmdMat.m[2][3] = vMat.m[3][2];

    return hmdMat;
}

vr::HmdMatrix34_t VR::GetControllerTipMatrix(vr::ETrackedControllerRole controllerRole)
{
    vr::VRInputValueHandle_t inputValue = vr::k_ulInvalidInputValueHandle;

    if (controllerRole == vr::TrackedControllerRole_RightHand)
    {
        m_Input->GetInputSourceHandle("/user/hand/right", &inputValue);
    }
    else if (controllerRole == vr::TrackedControllerRole_LeftHand)
    {
        m_Input->GetInputSourceHandle("/user/hand/left", &inputValue);
    }

    if (inputValue != vr::k_ulInvalidInputValueHandle)
    {
        char buffer[vr::k_unMaxPropertyStringSize];

        m_System->GetStringTrackedDeviceProperty(vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(controllerRole), vr::Prop_RenderModelName_String, 
                                                 buffer, vr::k_unMaxPropertyStringSize);

        vr::RenderModel_ControllerMode_State_t controllerState = {0};
        vr::RenderModel_ComponentState_t componentState = {0};

        if (vr::VRRenderModels() && vr::VRRenderModels()->GetComponentStateForDevicePath(buffer, vr::k_pch_Controller_Component_Tip, inputValue, &controllerState, &componentState))
        {
            return componentState.mTrackingToComponentLocal;
        }
    }

    // Not a hand controller role or tip lookup failed, return identity
    const vr::HmdMatrix34_t identity = 
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };

    return identity;
}

bool VR::CheckOverlayIntersectionForController(vr::VROverlayHandle_t overlayHandle, vr::ETrackedControllerRole controllerRole)
{
    if (!m_System || !m_Overlay)
        return false;

    vr::TrackedDeviceIndex_t deviceIndex = m_System->GetTrackedDeviceIndexForControllerRole(controllerRole);

    if (deviceIndex == vr::k_unTrackedDeviceIndexInvalid || deviceIndex >= vr::k_unMaxTrackedDeviceCount)
        return false;

    vr::TrackedDevicePose_t &controllerPose = m_Poses[deviceIndex];

    if (!controllerPose.bPoseIsValid)
        return false;

    VMatrix controllerVMatrix = VMatrixFromHmdMatrix(controllerPose.mDeviceToAbsoluteTracking);
    VMatrix tipVMatrix        = VMatrixFromHmdMatrix(GetControllerTipMatrix(controllerRole));
    tipVMatrix.MatrixMul(controllerVMatrix, controllerVMatrix);

    vr::VROverlayIntersectionParams_t  params  = {0};
    vr::VROverlayIntersectionResults_t results = {0};

    params.eOrigin    = vr::VRCompositor()->GetTrackingSpace();
    params.vSource    = { controllerVMatrix.m[3][0],  controllerVMatrix.m[3][1],  controllerVMatrix.m[3][2]};
    params.vDirection = {-controllerVMatrix.m[2][0], -controllerVMatrix.m[2][1], -controllerVMatrix.m[2][2]};

    return m_Overlay->ComputeOverlayIntersection(overlayHandle, &params, &results);
}

QAngle VR::GetRightControllerAbsAngle()
{
    return m_RightControllerAngAbs;
}

Vector VR::GetRightControllerAbsPos()
{
    return m_RightControllerPosAbs;
}

QAngle VR::GetLeftControllerAbsAngle()
{
    return m_LeftControllerAngAbs;
}

Vector VR::GetLeftControllerAbsPos()
{
    return m_LeftControllerPosAbs;
}

Vector VR::GetRecommendedViewmodelAbsPos()
{
    Vector viewmodelPos = GetRightControllerAbsPos();
    viewmodelPos -= m_ViewmodelForward * m_ViewmodelPosOffset.x;
    viewmodelPos -= m_ViewmodelRight * m_ViewmodelPosOffset.y;
    viewmodelPos -= m_ViewmodelUp * m_ViewmodelPosOffset.z;

    return viewmodelPos;
}

QAngle VR::GetRecommendedViewmodelAbsAngle()
{
    QAngle result{};

    QAngle::VectorAngles(m_ViewmodelForward, m_ViewmodelUp, result);

    // Model-only tweak. Aim is driven by m_RightControllerAngAbs elsewhere, so
    // dialling the weapon into your hand here cannot move your point of impact.
    result.x += m_ViewmodelAngleOffset.x;
    result.y += m_ViewmodelAngleOffset.y;
    result.z += m_ViewmodelAngleOffset.z;

    return result;
}

void VR::UpdateTracking()
{
    GetPoses();

    int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    if (playerIndex < 1)
        return;

    C_BasePlayer *localPlayer = (C_BasePlayer *)m_Game->GetClientEntity(playerIndex);

    m_Game->m_IsMeleeWeaponActive = Weapons::IsMelee(m_Game->m_ActiveWeaponModel);

    // HMD tracking
    QAngle hmdAngLocal = m_HmdPose.TrackedDeviceAng;	
    Vector hmdPosLocal = m_HmdPose.TrackedDevicePos;	

    Vector deltaPosition = hmdPosLocal - m_HmdPosLocalPrev;
    Vector hmdPosCorrected = m_HmdPosCorrectedPrev + deltaPosition;

    VectorPivotXY(hmdPosCorrected, m_HmdPosCorrectedPrev, m_RotationOffset);

    m_HmdPosCorrectedPrev = hmdPosCorrected;
    m_HmdPosLocalPrev = hmdPosLocal;

    hmdAngLocal.y += m_RotationOffset;
    // Wrap angle from -180 to 180
    hmdAngLocal.y -= 360 * std::floor((hmdAngLocal.y + 180) / 360);

    QAngle::AngleVectors(hmdAngLocal, &m_HmdForward, &m_HmdRight, &m_HmdUp);				

    m_HmdPosLocalInWorld = hmdPosCorrected * m_VRScale;

    // Roomscale setup
    Vector cameraMovingDirection = m_SetupOrigin - m_SetupOriginPrev;
    Vector cameraToPlayer = m_HmdPosAbsPrev - m_SetupOriginPrev;
    cameraMovingDirection.z = 0;
    cameraToPlayer.z = 0;
    float cameraFollowing = DotProduct(cameraMovingDirection, cameraToPlayer);
    float cameraDistance = VectorLength(cameraToPlayer);

    if (cameraDistance < 24.0f)
        m_RoomscaleActive = true;

    if ((cameraFollowing < 0 && cameraDistance > 1) || (m_PushingThumbstick))
        m_RoomscaleActive = false;

    if (!m_RoomscaleActive)
        m_CameraAnchor += m_SetupOrigin - m_SetupOriginPrev;
    
    m_CameraAnchor.z = m_SetupOrigin.z + m_HeightOffset;

    m_HmdPosAbs = m_CameraAnchor - Vector(0, 0, 64) + m_HmdPosLocalInWorld;

    // Check if camera is clipping inside wall
    CGameTrace trace;
    Ray_t ray;
    CTraceFilterSkipNPCsAndPlayers tracefilter((IHandleEntity*)localPlayer, 0);

    Vector extendedHmdPos = m_HmdPosAbs - m_SetupOrigin;
    VectorNormalize(extendedHmdPos);
    extendedHmdPos = m_HmdPosAbs + (extendedHmdPos * 10);
    ray.Init(m_SetupOrigin, extendedHmdPos);

    m_Game->m_EngineTrace->TraceRay(ray, STANDARD_TRACE_MASK, &tracefilter, &trace);
    if (trace.fraction < 1 && trace.fraction > 0)
    {
        Vector distanceInsideWall = trace.endpos - extendedHmdPos;
        m_CameraAnchor += distanceInsideWall;
        m_HmdPosAbs = m_CameraAnchor - Vector(0, 0, 64) + m_HmdPosLocalInWorld;
    }

    // Reset camera if it somehow gets too far
    m_SetupOriginToHMD = m_HmdPosAbs - m_SetupOrigin;
    if (VectorLength(m_SetupOriginToHMD) > 150)
        ResetPosition();

    m_HmdAngAbs = hmdAngLocal;

    m_HmdPosAbsPrev = m_HmdPosAbs;
    m_SetupOriginPrev = m_SetupOrigin;

    GetViewParameters();
    m_Ipd = m_EyeToHeadTransformPosRight.x * 2;
    m_EyeZ = m_EyeToHeadTransformPosRight.z;

    // Hand tracking
    Vector leftControllerPosLocal = m_LeftControllerPose.TrackedDevicePos;											
    QAngle leftControllerAngLocal = m_LeftControllerPose.TrackedDeviceAng;

    Vector rightControllerPosLocal = m_RightControllerPose.TrackedDevicePos;
    QAngle rightControllerAngLocal = m_RightControllerPose.TrackedDeviceAng;

    Vector hmdToController = rightControllerPosLocal - hmdPosLocal;
    Vector rightControllerPosCorrected = hmdPosCorrected + hmdToController;

    Vector hmdToLeftController = leftControllerPosLocal - hmdPosLocal;
    Vector leftControllerPosCorrected = hmdPosCorrected + hmdToLeftController;

    // When using stick turning, pivot the controllers around the HMD
    VectorPivotXY(rightControllerPosCorrected, hmdPosCorrected, m_RotationOffset);
    VectorPivotXY(leftControllerPosCorrected, hmdPosCorrected, m_RotationOffset);

    Vector rightControllerPosLocalInWorld = rightControllerPosCorrected * m_VRScale;
    Vector leftControllerPosLocalInWorld = leftControllerPosCorrected * m_VRScale;

    m_RightControllerPosAbs = m_CameraAnchor - Vector(0, 0, 64) + rightControllerPosLocalInWorld;
    m_LeftControllerPosAbs = m_CameraAnchor - Vector(0, 0, 64) + leftControllerPosLocalInWorld;

    rightControllerAngLocal.y += m_RotationOffset;
    // Wrap angle from -180 to 180
    rightControllerAngLocal.y -= 360 * std::floor((rightControllerAngLocal.y + 180) / 360);

    QAngle::AngleVectors(leftControllerAngLocal, &m_LeftControllerForward, &m_LeftControllerRight, &m_LeftControllerUp);			
    QAngle::AngleVectors(rightControllerAngLocal, &m_RightControllerForward, &m_RightControllerRight, &m_RightControllerUp);	

    // Adjust controller angle 45 degrees downward
    m_LeftControllerForward = VectorRotate(m_LeftControllerForward, m_LeftControllerRight, -45.0);
    m_LeftControllerUp = VectorRotate(m_LeftControllerUp, m_LeftControllerRight, -45.0);

    m_RightControllerForward = VectorRotate(m_RightControllerForward, m_RightControllerRight, -45.0);
    m_RightControllerUp = VectorRotate(m_RightControllerUp, m_RightControllerRight, -45.0);

    // controller angles
    QAngle::VectorAngles(m_LeftControllerForward, m_LeftControllerUp, m_LeftControllerAngAbs);
    QAngle::VectorAngles(m_RightControllerForward, m_RightControllerUp, m_RightControllerAngAbs);
    
    PositionAngle viewmodelOffset = Weapons::GetOffset(m_Game->m_ActiveWeaponModel);

    m_ViewmodelPosOffset = viewmodelOffset.position;
    m_ViewmodelAngOffset = viewmodelOffset.angle;

    m_ViewmodelForward = m_RightControllerForward;
    m_ViewmodelUp = m_RightControllerUp;
    m_ViewmodelRight = m_RightControllerRight;

    // Two-handed rifle/shotgun grip: off-hand near the barrel aims along both hands.
    m_TwoHanded = false;
    Vector handDelta = m_LeftControllerPosAbs - m_RightControllerPosAbs;
    const float handDist = VectorLength(handDelta);
    if (handDist > 10.0f && handDist < 32.0f &&
        !Weapons::IsMelee(m_Game->m_ActiveWeaponModel) &&
        !Weapons::IsDualWieldable(m_Game->m_ActiveWeaponModel) &&
        !Weapons::IsThrowable(m_Game->m_ActiveWeaponModel))
    {
        VectorNormalize(handDelta);
        m_ViewmodelForward = handDelta;
        m_TwoHanded = true;
        QAngle::VectorAngles(m_ViewmodelForward, m_ViewmodelUp, m_RightControllerAngAbs);
    }

    // Viewmodel yaw offset
    m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelUp, m_ViewmodelAngOffset.y);
    m_ViewmodelRight = VectorRotate(m_ViewmodelRight, m_ViewmodelUp, m_ViewmodelAngOffset.y);

    // Viewmodel pitch offset
    m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelRight, m_ViewmodelAngOffset.x);
    m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelRight, m_ViewmodelAngOffset.x);

    // Viewmodel roll offset
    m_ViewmodelRight = VectorRotate(m_ViewmodelRight, m_ViewmodelForward, m_ViewmodelAngOffset.z);
    m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelForward, m_ViewmodelAngOffset.z);
}

Vector VR::GetViewAngle()
{
    return Vector( m_HmdAngAbs.x, m_HmdAngAbs.y, m_HmdAngAbs.z );
}

Vector VR::GetViewOriginLeft()
{
    Vector viewOriginLeft;

    viewOriginLeft = m_HmdPosAbs + (m_HmdForward * (-(m_EyeZ * m_VRScale)));
    viewOriginLeft = viewOriginLeft + (m_HmdRight * (-((m_Ipd * m_IpdScale * m_VRScale) / 2)));

    return viewOriginLeft;
}

Vector VR::GetViewOriginRight()
{
    Vector viewOriginRight;

    viewOriginRight = m_HmdPosAbs + (m_HmdForward * (-(m_EyeZ * m_VRScale)));
    viewOriginRight = viewOriginRight + (m_HmdRight * (m_Ipd * m_IpdScale * m_VRScale) / 2);

    return viewOriginRight;
}

void VR::ApplyHeadAndIpd(CViewSetup &left, CViewSetup &right, const CViewSetup &setup)
{
    GetPoses();

    const bool poseValid = m_Poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid;
    QAngle hmdAng = poseValid ? m_HmdPose.TrackedDeviceAng : setup.angles;
    hmdAng.y += m_RotationOffset;
    hmdAng.y -= 360.0f * std::floor((hmdAng.y + 180.0f) / 360.0f);

    QAngle::AngleVectors(hmdAng, &m_HmdForward, &m_HmdRight, &m_HmdUp);
    m_HmdAngAbs = hmdAng;

    if (poseValid)
    {
        if (!m_HaveSeatPose)
        {
            m_SeatHmdPos = m_HmdPose.TrackedDevicePos;
            m_HaveSeatPose = true;
        }
        m_HmdPosLocalInWorld = (m_HmdPose.TrackedDevicePos - m_SeatHmdPos) * m_VRScale;
        // Same room-frame problem as the hands: leaning/stepping must be
        // rotated into the game's turned frame or roomscale drifts off-axis.
        if (fabsf(m_RotationOffset) > 0.01f)
            m_HmdPosLocalInWorld = VectorRotate(m_HmdPosLocalInWorld, Vector(0.0f, 0.0f, 1.0f), m_RotationOffset);
    }
    else
    {
        m_HmdPosLocalInWorld = { 0, 0, 0 };
    }

    GetViewParameters();
    m_Ipd = m_EyeToHeadTransformPosRight.x * 2.0f;
    m_EyeZ = m_EyeToHeadTransformPosRight.z;

    // Real IPD (~6.3cm). Scale 43.2 -> ~1.4u. Keep ~2u so depth is obvious
    // without the 8u (~37cm) warp from the earlier build.
    float halfIpd = (m_Ipd > 0.001f) ? (m_Ipd * m_IpdScale * m_VRScale) * 0.5f : 2.0f;
    if (halfIpd < 1.8f) halfIpd = 1.8f;
    if (halfIpd > 2.6f) halfIpd = 2.6f;

    Vector eyeOrigin = setup.origin + m_HmdPosLocalInWorld;

    left.origin = eyeOrigin + (m_HmdRight * (-halfIpd));
    right.origin = eyeOrigin + (m_HmdRight * (halfIpd));
    left.angles = hmdAng;
    right.angles = hmdAng;

    left.fov = m_Fov;
    right.fov = m_Fov;
    left.fovViewmodel = m_Fov;
    right.fovViewmodel = m_Fov;
    left.m_flAspectRatio = m_Aspect;
    right.m_flAspectRatio = m_Aspect;
    left.zNear = 6.0f;
    right.zNear = 6.0f;
    left.zNearViewmodel = 6.0f;
    right.zNearViewmodel = 6.0f;

    if (m_Game && m_Game->m_EngineClient)
    {
        if (m_MotionControls && PressedDigitalAction(m_ActionPrimaryAttack))
        {
            QAngle gun = m_RightControllerAngAbs;
            m_Game->m_EngineClient->SetViewAngles(gun);
        }
        else
            m_Game->m_EngineClient->SetViewAngles(hmdAng);
    }

    // Hands: controller pose relative to HMD, in the same space as the camera.
    auto placeController = [&](const TrackedDevicePoseData &pose, Vector &outPos, QAngle &outAng,
                               Vector &fwd, Vector &right, Vector &up)
    {
        Vector delta = (pose.TrackedDevicePos - m_HmdPose.TrackedDevicePos) * m_VRScale;
        // The hand offset comes out of OpenVR in the PHYSICAL room frame. The
        // game world is rotated by m_RotationOffset every time you stick- or
        // snap-turn, and this delta was being added without that rotation -- so
        // the instant you turned, the gun stopped corresponding to your hand.
        // This is a core 1:1 correctness fix, not a tuning tweak.
        if (fabsf(m_RotationOffset) > 0.01f)
            delta = VectorRotate(delta, Vector(0.0f, 0.0f, 1.0f), m_RotationOffset);
        outPos = eyeOrigin + delta;
        outAng = pose.TrackedDeviceAng;
        outAng.y += m_RotationOffset;
        outAng.y -= 360.0f * std::floor((outAng.y + 180.0f) / 360.0f);
        QAngle::AngleVectors(outAng, &fwd, &right, &up);
    };
    placeController(m_RightControllerPose, m_RightControllerPosAbs, m_RightControllerAngAbs,
                    m_ViewmodelForward, m_ViewmodelRight, m_ViewmodelUp);
    placeController(m_LeftControllerPose, m_LeftControllerPosAbs, m_LeftControllerAngAbs,
                    m_LeftControllerForward, m_LeftControllerRight, m_LeftControllerUp);

    // Grip correction. Rotating the basis down around the controller's right
    // axis turns "pointing the device forward" into "pointing a barrel", and
    // re-deriving m_RightControllerAngAbs from it keeps the shot direction,
    // the view angles while firing and the rendered gun all in agreement.
    // Grip correction applies to the VIEWMODEL BASIS ONLY.
    //
    // It used to also re-derive m_RightControllerAngAbs via
    // VectorAngles(forward, up, ...). That angle drives SetViewAngles and
    // therefore the shot direction, and round-tripping it through the rotated
    // basis inverted PITCH -- reported in play as "aim up with the controller
    // and the bullets go into the ground", with left/right unaffected because
    // yaw survives the round trip. The pose angle from GetPoseData is already
    // correct in Source's convention (positive pitch = down); keep using it, and
    // express the grip as a plain pitch offset so it cannot flip anything.
    if (fabsf(m_GunGripAngle) > 0.01f)
    {
        m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelRight, -m_GunGripAngle);
        m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelRight, -m_GunGripAngle);
        m_LeftControllerForward = VectorRotate(m_LeftControllerForward, m_LeftControllerRight, -m_GunGripAngle);
        m_LeftControllerUp = VectorRotate(m_LeftControllerUp, m_LeftControllerRight, -m_GunGripAngle);

        // Source pitch is positive-down, so a downward grip tilt is +angle.
        m_RightControllerAngAbs.x += m_GunGripAngle;
        m_LeftControllerAngAbs.x += m_GunGripAngle;
    }

    // --- Per-weapon pose -----------------------------------------------------
    // weapons.cpp's offset table, the melee/dual-wield/throwable predicates and
    // the two-handed grip all lived in UpdateTracking(), which has never had a
    // call site. Every gun therefore used one generic pose. This is that logic,
    // moved to where the viewmodel basis is actually built.
    if (m_Game)
    {
        const std::string &wpn = m_Game->m_ActiveWeaponModel;
        m_Game->m_IsMeleeWeaponActive = Weapons::IsMelee(wpn);

        PositionAngle pose = m_PerWeaponOffsets
            ? Weapons::GetOffset(wpn)
            : PositionAngle{ { 0, 0, 0 }, { 0, 0, 0 } };
        m_ViewmodelPosOffset = pose.position + m_ViewmodelUserOffset;
        m_ViewmodelAngOffset = pose.angle;

        // Two-handed grip: with the off hand up near the barrel, aim along the
        // line between the hands instead of the gun hand's own axis.
        m_TwoHanded = false;
        if (m_TwoHandedGrip)
        {
            // Off-hand grip must be HELD, like HaloCEVR. Distance alone meant the
            // grip engaged any time your hands drifted near each other.
            const bool gripHeld = !m_TwoHandedNeedsGrip
                || PressedDigitalAction(m_ActionTwoHand);
            Vector handDelta = m_LeftControllerPosAbs - m_RightControllerPosAbs;
            const float handDist = VectorLength(handDelta);
            if (gripHeld && handDist > 6.0f && handDist < 40.0f &&
                !Weapons::IsMelee(wpn) && !Weapons::IsDualWieldable(wpn) &&
                !Weapons::IsThrowable(wpn))
            {
                VectorNormalize(handDelta);
                m_ViewmodelForward = handDelta;
                m_TwoHanded = true;
            }
        }

        // Per-weapon yaw / pitch / roll, same order as the original.
        m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelUp, m_ViewmodelAngOffset.y);
        m_ViewmodelRight   = VectorRotate(m_ViewmodelRight,   m_ViewmodelUp, m_ViewmodelAngOffset.y);
        m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelRight, m_ViewmodelAngOffset.x);
        m_ViewmodelUp      = VectorRotate(m_ViewmodelUp,      m_ViewmodelRight, m_ViewmodelAngOffset.x);
        m_ViewmodelRight   = VectorRotate(m_ViewmodelRight,   m_ViewmodelForward, m_ViewmodelAngOffset.z);
        m_ViewmodelUp      = VectorRotate(m_ViewmodelUp,      m_ViewmodelForward, m_ViewmodelAngOffset.z);

        // NOTE: deliberately NOT re-deriving m_RightControllerAngAbs from the
        // basis here. That is exactly what inverted pitch on the shot direction.
        // The per-weapon angles shape the MODEL; aim stays on the pose angle.

        static int s_wlog = 0;
        if (s_wlog < 6)
        {
            Game::logMsg("weapon pose '%s' off=(%.1f,%.1f,%.1f) ang=(%.1f,%.1f,%.1f) twoHanded=%d melee=%d",
                         wpn.c_str(), m_ViewmodelPosOffset.x, m_ViewmodelPosOffset.y, m_ViewmodelPosOffset.z,
                         m_ViewmodelAngOffset.x, m_ViewmodelAngOffset.y, m_ViewmodelAngOffset.z,
                         (int)m_TwoHanded, (int)m_Game->m_IsMeleeWeaponActive);
            ++s_wlog;
        }
    }

    // Fire marker. The trigger is used as a delimiter between calibration
    // steps, so log its rising edge with the current hand state -- that turns a
    // wall of samples into clearly separated phases.
    if (m_MotionDebug)
    {
        static bool s_firePrev = false;
        const bool fireNow = PressedDigitalAction(m_ActionPrimaryAttack);
        if (fireNow && !s_firePrev)
        {
            static int s_shot = 0;
            const Vector armRaw = m_RightControllerPose.TrackedDevicePos - m_HmdPose.TrackedDevicePos;
            Game::logMsg("======== MARKER %d (fire) armM=%.2f hand=(%.1f,%.1f,%.1f) player=(%.1f,%.1f,%.1f) ========",
                         ++s_shot, VectorLength(armRaw),
                         m_RightControllerPosAbs.x, m_RightControllerPosAbs.y, m_RightControllerPosAbs.z,
                         setup.origin.x, setup.origin.y, setup.origin.z);
        }
        s_firePrev = fireNow;
    }

    // --- Motion trace ------------------------------------------------------
    // Every stage of hand -> weapon, sampled 4x/second. Read it against a known
    // movement (arm straight out, then to the side, then up) and each stage can
    // be checked on its own:
    //   rawHand/rawHmd : OpenVR metres, room space. Should track your arm 1:1.
    //   armM           : hand-to-head distance in METRES. ~0.6 with the arm out.
    //   armU           : the same distance in Source units. Should be armM*VRScale.
    //   handWorld      : where the game thinks your hand is.
    //   fromPlayer     : handWorld - player origin. Should equal armU in size.
    //   vmWrite        : what we ask the weapon's origin to become.
    // If armM tracks but armU does not, VRScale is wrong. If handWorld does not
    // move with your arm, the room->world conversion is wrong. If everything
    // here is right and the gun still does not move, the write is being ignored.
    if (m_MotionDebug)
    {
        static DWORD s_lastMotion = 0;
        const DWORD nowMs = GetTickCount();
        if (s_lastMotion == 0 || (nowMs - s_lastMotion) >= 250)
        {
            s_lastMotion = nowMs;
            const Vector rawHand = m_RightControllerPose.TrackedDevicePos;
            const Vector rawHmd = m_HmdPose.TrackedDevicePos;
            const Vector armRaw = rawHand - rawHmd;
            const float armM = VectorLength(armRaw);
            const Vector fromPlayer = m_RightControllerPosAbs - setup.origin;
            const Vector vmWrite = GetRecommendedViewmodelAbsPos();
            Game::logMsg("MOTION rawHand=(%.2f,%.2f,%.2f)m rawHmd=(%.2f,%.2f,%.2f)m armM=%.2f armU=%.1f "
                         "handWorld=(%.1f,%.1f,%.1f) player=(%.1f,%.1f,%.1f) fromPlayer=(%.1f,%.1f,%.1f)|%.1f| "
                         "vmWrite=(%.1f,%.1f,%.1f) ang=(%.0f,%.0f,%.0f) hmdAng=(%.0f,%.0f,%.0f) scale=%.1f execMoves=%ld getOrigin=%ld getAngles=%ld",
                         rawHand.x, rawHand.y, rawHand.z,
                         rawHmd.x, rawHmd.y, rawHmd.z,
                         armM, armM * m_VRScale,
                         m_RightControllerPosAbs.x, m_RightControllerPosAbs.y, m_RightControllerPosAbs.z,
                         setup.origin.x, setup.origin.y, setup.origin.z,
                         fromPlayer.x, fromPlayer.y, fromPlayer.z, VectorLength(fromPlayer),
                         vmWrite.x, vmWrite.y, vmWrite.z,
                         m_RightControllerAngAbs.x, m_RightControllerAngAbs.y, m_RightControllerAngAbs.z,
                         m_HmdAngAbs.x, m_HmdAngAbs.y, m_HmdAngAbs.z,
                         m_VRScale, GESVR_ExecMoveCount(),
                         GESVR_RenderOriginCalls(), GESVR_RenderAnglesCalls());
        }
    }

    static int s_log = 0;
    if ((s_log++ % 90) == 0)
    {
        Game::logMsg("HMD poseValid=%d ang=(%.1f,%.1f,%.1f) halfIpd=%.2f gun=(%.1f,%.1f,%.1f)",
                     (int)poseValid, hmdAng.x, hmdAng.y, hmdAng.z, halfIpd,
                     m_RightControllerPosAbs.x, m_RightControllerPosAbs.y, m_RightControllerPosAbs.z);
    }
}


void VR::ResetPosition()
{
    m_CameraAnchor += m_SetupOrigin - m_HmdPosAbs;
    m_HeightOffset += m_SetupOrigin.z - m_HmdPosAbs.z;
    m_HaveSeatPose = false;
}

void VR::CreateWristOverlays()
{
    if (!m_Overlay)
        return;

    struct { vr::VROverlayHandle_t *handle; const char *key; } overlays[] = {
        { &m_WristWatchHandle, "GESVRWristWatch" },
        { &m_WristAmmoHandle,  "GESVRWristAmmo" },
        { &m_HurtHUDHandle,    "GESVRHurtHUD" },
    };

    for (auto &entry : overlays)
    {
        vr::EVROverlayError err = m_Overlay->CreateOverlay(entry.key, entry.key, entry.handle);
        if (err != vr::VROverlayError_None && err != vr::VROverlayError_KeyInUse)
        {
            Game::logMsg("Could not create overlay %s: %d", entry.key, (int)err);
            continue;
        }
        m_Overlay->SetOverlayInputMethod(*entry.handle, vr::VROverlayInputMethod_None);
        m_Overlay->SetOverlayFlag(*entry.handle, vr::VROverlayFlags_IsPremultiplied, true);
        m_Overlay->HideOverlay(*entry.handle);
    }
}

void VR::HideWristOverlays()
{
    if (!m_Overlay)
        return;
    if (m_WristWatchHandle) m_Overlay->HideOverlay(m_WristWatchHandle);
    if (m_WristAmmoHandle)  m_Overlay->HideOverlay(m_WristAmmoHandle);
}

static bool ReadablePtr(const void *p, size_t bytes)
{
    if (!p)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(p, &info, sizeof(info)))
        return false;
    if (info.State != MEM_COMMIT)
        return false;
    const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
    if (info.Protect & bad)
        return false;
    return true;
}

static bool ReadableCString(const char *s)
{
    if (!ReadablePtr(s, 1))
        return false;
    for (int i = 0; i < 64; ++i)
    {
        if (!ReadablePtr(s + i, 1))
            return false;
        if (s[i] == 0)
            return i > 0;
        if (s[i] < 32 || s[i] > 126)
            return false;
    }
    return false;
}

struct RecvPropStub
{
    const char *m_pVarName;
    int m_RecvType;
    int m_Flags;
    int m_StringBufferSize;
    int m_bInsideArray;
    const void *m_pExtraData;
    void *m_pArrayProp;
    void *m_ArrayLengthProxy;
    void *m_ProxyFn;
    void *m_DataTableProxyFn;
    void *m_pDataTable;
    int m_Offset;
};

struct RecvTableStub
{
    RecvPropStub *m_pProps;
    int m_nProps;
    void *m_pDecoder;
    const char *m_pNetTableName;
};

struct ClientClassStub
{
    void *m_pCreateFn;
    void *m_pCreateEventFn;
    const char *m_pNetworkName;
    RecvTableStub *m_pRecvTable;
    ClientClassStub *m_pNext;
    int m_ClassID;
};

static ClientClassStub *CallGetAllClasses(void *fn, void *client)
{
    __try
    {
        return reinterpret_cast<ClientClassStub *(__thiscall *)(void *)>(fn)(client);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

static int FindNetvarInTable(RecvTableStub *table, const char *wanted, int extra = 0, int depth = 0)
{
    if (!table || depth > 8 || !ReadablePtr(table, sizeof(RecvTableStub)))
        return -1;
    if (table->m_nProps <= 0 || table->m_nProps > 512)
        return -1;
    if (!ReadablePtr(table->m_pProps, sizeof(RecvPropStub)))
        return -1;

    for (int i = 0; i < table->m_nProps; ++i)
    {
        RecvPropStub *prop = &table->m_pProps[i];
        if (!ReadablePtr(prop, sizeof(RecvPropStub)))
            continue;
        if (ReadableCString(prop->m_pVarName) && strcmp(prop->m_pVarName, wanted) == 0)
            return extra + prop->m_Offset;

        if (prop->m_pDataTable)
        {
            int nested = FindNetvarInTable(reinterpret_cast<RecvTableStub *>(prop->m_pDataTable), wanted, extra + prop->m_Offset, depth + 1);
            if (nested >= 0)
                return nested;
        }
    }
    return -1;
}

void VR::ResolvePlayerNetvars()
{
    if (m_HealthNetvar >= 0)
        return;

    // This only cached on SUCCESS. On failure it re-walked every client class
    // and every recv table on the NEXT frame, and the next, forever -- 598 full
    // scans in one 80-second session once UpdateHurtHUD started calling it.
    // GE:S may simply not expose m_iHealth where this looks, so failure has to
    // be cached too. Retry occasionally in case the class list is not populated
    // yet at map load, then stop.
    static DWORD s_lastTry = 0;
    static int s_attempts = 0;
    const DWORD now = GetTickCount();
    if (s_attempts > 0)
    {
        if (s_attempts >= 12)
            return;                                  // gave up
        if ((now - s_lastTry) < 3000)
            return;                                  // not yet
    }
    s_lastTry = now;
    ++s_attempts;

    if (!m_Game || !m_Game->m_BaseClientDll)
        return;

    void *client = m_Game->m_BaseClientDll;
    if (!ReadablePtr(client, sizeof(void *)))
        return;
    void **vtable = *reinterpret_cast<void ***>(client);
    if (!ReadablePtr(vtable, sizeof(void *) * 16))
        return;

    ClientClassStub *head = nullptr;
    for (int i = 6; i <= 12; ++i)
    {
        if (!vtable[i])
            continue;
        ClientClassStub *maybe = CallGetAllClasses(vtable[i], client);
        if (!ReadablePtr(maybe, sizeof(ClientClassStub)))
            continue;
        if (!ReadableCString(maybe->m_pNetworkName))
            continue;
        head = maybe;
        if (s_attempts <= 2)
            Game::logMsg("GetAllClasses via vtable[%d], first class %s", i, maybe->m_pNetworkName);
        break;
    }

    if (!head)
        return;

    for (ClientClassStub *cc = head; cc && ReadablePtr(cc, sizeof(ClientClassStub)); cc = cc->m_pNext)
    {
        if (!ReadableCString(cc->m_pNetworkName))
            break;
        const char *name = cc->m_pNetworkName;
        const bool isPlayer = strstr(name, "Player") != nullptr && strstr(name, "Resource") == nullptr;
        if (!isPlayer)
            continue;

        int health = FindNetvarInTable(cc->m_pRecvTable, "m_iHealth");
        int armor = FindNetvarInTable(cc->m_pRecvTable, "m_ArmorValue");
        if (armor < 0)
            armor = FindNetvarInTable(cc->m_pRecvTable, "m_iArmor");
        if (armor < 0)
            armor = FindNetvarInTable(cc->m_pRecvTable, "m_Armor");

        if (health >= 0)
        {
            m_HealthNetvar = health;
            m_ArmorNetvar = armor;
            Game::logMsg("Player netvars on %s: m_iHealth=%d armor=%d", name, health, armor);
            return;
        }
    }
}

int VR::ReadLocalHealth()
{
    ResolvePlayerNetvars();
    if (m_HealthNetvar < 0 || !m_Game || !m_Game->m_EngineClient)
        return -1;

    int index = m_Game->m_EngineClient->GetLocalPlayer();
    CBaseEntity *ent = m_Game->GetClientEntity(index);
    if (!ent || !ReadablePtr(ent, m_HealthNetvar + sizeof(int)))
        return -1;

    int health = *reinterpret_cast<int *>(reinterpret_cast<char *>(ent) + m_HealthNetvar);
    if (health < 0 || health > 1000)
        return -1;
    return health;
}

bool VR::IsLookingAtOffhandWatch()
{
    if (!m_System)
        return false;

    const vr::ETrackedControllerRole offHand = m_LeftHanded
        ? vr::TrackedControllerRole_RightHand
        : vr::TrackedControllerRole_LeftHand;

    vr::TrackedDeviceIndex_t handIndex = m_System->GetTrackedDeviceIndexForControllerRole(offHand);
    if (handIndex == vr::k_unTrackedDeviceIndexInvalid)
        return false;
    if (!m_Poses[handIndex].bPoseIsValid || !m_Poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
        return false;

    const vr::HmdMatrix34_t &hmdMat = m_Poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
    const vr::HmdMatrix34_t &handMat = m_Poses[handIndex].mDeviceToAbsoluteTracking;

    Vector hmdPos(hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3]);
    Vector hmdForward(-hmdMat.m[0][2], -hmdMat.m[1][2], -hmdMat.m[2][2]);
    VectorNormalize(hmdForward);

    Vector handPos(handMat.m[0][3], handMat.m[1][3], handMat.m[2][3]);
    Vector toHand = handPos - hmdPos;
    float dist = VectorLength(toHand);
    if (dist < 0.001f)
        return false;
    Vector toHandDir = toHand * (1.0f / dist);

    m_LookingAtWrist = (dist < m_WristLookMaxDistance) && (DotProduct(hmdForward, toHandDir) > m_WristLookMinDot);
    return m_LookingAtWrist;
}

void VR::SubmitWristOverlay(vr::VROverlayHandle_t handle, vr::TrackedDeviceIndex_t handIndex,
    const Vector &right, const Vector &up, const Vector &backward,
    const Vector &offset, float width, const vr::VRTextureBounds_t &bounds)
{
    if (!handle || !m_Overlay)
        return;

    m_Overlay->SetOverlayWidthInMeters(handle, width);

    // Config offset is (forward, left, up). OpenVR device space is (right, up, back).
    vr::HmdMatrix34_t xf = {
        right.x, up.x, backward.x, -offset.y,
        right.y, up.y, backward.y,  offset.z,
        right.z, up.z, backward.z, -offset.x
    };
    m_Overlay->SetOverlayTransformTrackedDeviceRelative(handle, handIndex, &xf);
    m_Overlay->SetOverlayTextureBounds(handle, &bounds);
    SetOverlayTextureLocked(m_Overlay, handle, &m_VKHUD.m_VRTexture);
    m_Overlay->ShowOverlay(handle);
}

void VR::UpdateWristHUD()
{
    if (!m_ShowWristHUD || !m_Overlay || !m_RenderedHud)
    {
        HideWristOverlays();
        return;
    }

    const vr::ETrackedControllerRole offHand = m_LeftHanded
        ? vr::TrackedControllerRole_RightHand
        : vr::TrackedControllerRole_LeftHand;
    vr::TrackedDeviceIndex_t handIndex = m_System->GetTrackedDeviceIndexForControllerRole(offHand);

    if (handIndex == vr::k_unTrackedDeviceIndexInvalid || !m_Poses[handIndex].bPoseIsValid
        || !IsLookingAtOffhandWatch())
    {
        HideWristOverlays();
        return;
    }

    Vector right(1, 0, 0);
    Vector up(0, 1, 0);
    Vector backward(0, 0, 1);

    // roll, pitch, yaw in degrees — same order as HaloCEVR's watch face
    right = VectorRotate(right, Vector(0, 0, 1), m_WristRotationDeg.x);
    up = VectorRotate(up, Vector(0, 0, 1), m_WristRotationDeg.x);
    right = VectorRotate(right, Vector(1, 0, 0), m_WristRotationDeg.y);
    up = VectorRotate(up, Vector(1, 0, 0), m_WristRotationDeg.y);
    backward = VectorRotate(backward, Vector(1, 0, 0), m_WristRotationDeg.y);
    right = VectorRotate(right, Vector(0, 1, 0), m_WristRotationDeg.z);
    backward = VectorRotate(backward, Vector(0, 1, 0), m_WristRotationDeg.z);

    SubmitWristOverlay(m_WristWatchHandle, handIndex, right, up, backward,
        m_WristOffset + m_WristWatchFineOffset, m_WristWatchWidth, m_WristWatchBounds);
    SubmitWristOverlay(m_WristAmmoHandle, handIndex, right, up, backward,
        m_WristOffset + m_WristAmmoFineOffset, m_WristAmmoWidth, m_WristAmmoBounds);
}

void VR::UpdateHurtHUD()
{
    if (!m_Overlay || !m_HurtHUDHandle)
        return;

    const int health = ReadLocalHealth();
    const auto now = std::chrono::steady_clock::now();

    if (health >= 0)
    {
        if (m_LastHealth >= 0 && health < m_LastHealth)
            m_HurtUntil = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(m_HurtHUDSeconds));
        m_LastHealth = health;
    }

    const bool recentlyHurt = now < m_HurtUntil;
    const bool lowHealth = health >= 0 && health <= m_HurtHealthThreshold;
    // If we can't read health, still flash the visor bars whenever the player
    // is looking at the watch so they can confirm the crop; otherwise only on hit.
    const bool show = (recentlyHurt || lowHealth) && !m_LookingAtWrist;

    if (!show || !m_RenderedHud)
    {
        m_Overlay->HideOverlay(m_HurtHUDHandle);
        return;
    }

    if (!m_Poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
    {
        m_Overlay->HideOverlay(m_HurtHUDHandle);
        return;
    }

    const vr::HmdMatrix34_t &hmdMat = m_Poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
    Vector hmdPos(hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3]);
    Vector hmdForward(-hmdMat.m[0][2], -hmdMat.m[1][2], -hmdMat.m[2][2]);
    VectorNormalize(hmdForward);

    vr::HmdMatrix34_t xf = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0
    };
    Vector pos = hmdPos + hmdForward * m_HurtHUDDistance;
    pos.z -= 0.22f; // drop it into the lower visor, Bond-style health strip

    const float yaw = atan2f(hmdMat.m[0][2], hmdMat.m[2][2]);
    xf.m[0][0] = cosf(yaw);
    xf.m[0][2] = sinf(yaw);
    xf.m[2][0] = -sinf(yaw);
    xf.m[2][2] = cosf(yaw);
    xf.m[0][3] = pos.x;
    xf.m[1][3] = pos.y;
    xf.m[2][3] = pos.z;

    m_Overlay->SetOverlayWidthInMeters(m_HurtHUDHandle, m_HurtHUDWidth);
    m_Overlay->SetOverlayTransformAbsolute(m_HurtHUDHandle, vr::VRCompositor()->GetTrackingSpace(), &xf);
    m_Overlay->SetOverlayTextureBounds(m_HurtHUDHandle, &m_HurtHUDBounds);
    SetOverlayTextureLocked(m_Overlay, m_HurtHUDHandle, &m_VKHUD.m_VRTexture);
    m_Overlay->ShowOverlay(m_HurtHUDHandle);
}

static bool CfgHas(const std::unordered_map<std::string, std::string> &m, const char *k)
{
    return m.find(k) != m.end() && !m.at(k).empty();
}

static bool CfgBool(const std::unordered_map<std::string, std::string> &m, const char *k, bool def)
{
    if (!CfgHas(m, k))
        return def;
    return m.at(k) == "true" || m.at(k) == "1";
}

static float CfgFloat(const std::unordered_map<std::string, std::string> &m, const char *k, float def)
{
    if (!CfgHas(m, k))
        return def;
    try { return std::stof(m.at(k)); }
    catch (...) { return def; }
}

static int CfgInt(const std::unordered_map<std::string, std::string> &m, const char *k, int def)
{
    if (!CfgHas(m, k))
        return def;
    try { return std::stoi(m.at(k)); }
    catch (...) { return def; }
}

static Vector CfgVec(const std::unordered_map<std::string, std::string> &m, const char *k, Vector def)
{
    if (!CfgHas(m, k))
        return def;
    float x = def.x, y = def.y, z = def.z;
    if (sscanf_s(m.at(k).c_str(), "%f,%f,%f", &x, &y, &z) == 3)
        return Vector(x, y, z);
    return def;
}

static vr::VRTextureBounds_t CfgBounds(const std::unordered_map<std::string, std::string> &m,
    const char *umin, const char *vmin, const char *umax, const char *vmax, vr::VRTextureBounds_t def)
{
    def.uMin = CfgFloat(m, umin, def.uMin);
    def.vMin = CfgFloat(m, vmin, def.vMin);
    def.uMax = CfgFloat(m, umax, def.uMax);
    def.vMax = CfgFloat(m, vmax, def.vMax);
    return def;
}

void VR::ParseConfigFile()
{
    char configPath[MAX_STR_LEN];
    MakeVRPath(configPath, MAX_STR_LEN, "config.txt");
    std::ifstream configStream(configPath);
    std::unordered_map<std::string, std::string> userConfig;

    std::string line;
    while (std::getline(configStream, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream sLine(line);
        std::string key;
        if (std::getline(sLine, key, '='))
        {
            std::string value;
            if (std::getline(sLine, value))
                userConfig[key] = value;
        }
    }

    if (userConfig.empty())
        return;

    m_SnapTurning = CfgBool(userConfig, "SnapTurning", m_SnapTurning);
    m_SnapTurnAngle = CfgFloat(userConfig, "SnapTurnAngle", m_SnapTurnAngle);
    m_TurnSpeed = CfgFloat(userConfig, "TurnSpeed", m_TurnSpeed);
    m_LeftHanded = CfgBool(userConfig, "LeftHanded", m_LeftHanded);
    m_VRScale = CfgFloat(userConfig, "VRScale", m_VRScale);
    m_IpdScale = CfgFloat(userConfig, "IPDScale", m_IpdScale);
    m_HideArms = CfgBool(userConfig, "HideArms", m_HideArms);
    m_MotionControls = CfgBool(userConfig, "MotionControls", m_MotionControls);
    {
        auto it = userConfig.find("AimMode");
        if (it != userConfig.end())
        {
            const std::string &v = it->second;
            if (v == "gun" || v == "controller" || v == "motion")
                m_MotionControls = true;
            else if (v == "head")
                m_MotionControls = false;
        }
    }
    Game::logMsg("AimMode=%s", m_MotionControls ? "gun" : "head");

    {
        auto it = userConfig.find("DisplayMode");
        if (it != userConfig.end())
        {
            std::string v = it->second;
            while (!v.empty() && isspace((unsigned char)v.back()))
                v.pop_back();
            if (v == "compositor" || v == "scene")   m_DisplayMode = Display_Compositor;
            else if (v == "sbs" || v == "overlay")   m_DisplayMode = Display_SBS;
            else if (v == "both")                    m_DisplayMode = Display_Both;
        }
    }
    m_UseTextureBounds = CfgBool(userConfig, "UseEyeFrustumCrop", m_UseTextureBounds);
    m_UseVerticalCrop = CfgBool(userConfig, "UseVerticalCrop", m_UseVerticalCrop);
    m_EyeRenderScale = CfgFloat(userConfig, "EyeRenderScale", m_EyeRenderScale);
    m_MenuWidthMeters = CfgFloat(userConfig, "MenuWidthMeters", m_MenuWidthMeters);
    m_MenuDistanceMeters = CfgFloat(userConfig, "MenuDistanceMeters", m_MenuDistanceMeters);
    m_UseEyeRenderTargets = CfgBool(userConfig, "EyeRenderTargets", m_UseEyeRenderTargets);
    m_ModelDrawExecuteSlot = (int)CfgFloat(userConfig, "ModelDrawExecuteSlot", (float)m_ModelDrawExecuteSlot);
    m_ModelDrawSetupSlot = (int)CfgFloat(userConfig, "ModelDrawSetupSlot", (float)m_ModelDrawSetupSlot);
    m_WeaponSetupHook = CfgBool(userConfig, "WeaponSetupHook", m_WeaponSetupHook);
    m_ViewmodelRenderablePatch = CfgBool(userConfig, "ViewmodelRenderablePatch", m_ViewmodelRenderablePatch);
    m_ViewmodelScopedPose = CfgBool(userConfig, "ViewmodelScopedPose", m_ViewmodelScopedPose);
    m_SetupProbe = CfgBool(userConfig, "SetupProbe", m_SetupProbe);
    m_MotionDebug = CfgBool(userConfig, "MotionDebug", m_MotionDebug);
    m_VtableProbe = CfgBool(userConfig, "VtableProbe", m_VtableProbe);
    m_SbsWidthMeters = CfgFloat(userConfig, "SbsWidthMeters", m_SbsWidthMeters);
    m_SbsDistance = CfgFloat(userConfig, "SbsDistance", m_SbsDistance);
    m_GunGripAngle = CfgFloat(userConfig, "GunGripAngle", m_GunGripAngle);
    m_ViewmodelUserOffset = CfgVec(userConfig, "ViewmodelOffset", m_ViewmodelUserOffset);
    m_ViewmodelAngleOffset = CfgVec(userConfig, "ViewmodelAngleOffset", m_ViewmodelAngleOffset);
    m_PerWeaponOffsets = CfgBool(userConfig, "PerWeaponOffsets", m_PerWeaponOffsets);
    m_TwoHandedGrip = CfgBool(userConfig, "TwoHandedGrip", m_TwoHandedGrip);
    m_TwoHandedNeedsGrip = CfgBool(userConfig, "TwoHandedNeedsGrip", m_TwoHandedNeedsGrip);
    m_MenuUseWin32 = CfgBool(userConfig, "MenuInputWin32", m_MenuUseWin32);
    m_MenuDriveCursor = CfgBool(userConfig, "MenuDriveCursor", m_MenuDriveCursor);
    m_MenuKeepaliveMs = (int)CfgFloat(userConfig, "MenuKeepaliveMs", (float)m_MenuKeepaliveMs);
    m_ShowMirrorWindow = CfgBool(userConfig, "ShowMirrorWindow", m_ShowMirrorWindow);
    m_TheaterHideThrottleMs = (int)CfgFloat(userConfig, "TheaterHideThrottleMs", (float)m_TheaterHideThrottleMs);
    g_menuDriveCursor = m_MenuDriveCursor;
    MenuInput::g_enabled.store(m_MenuDriveCursor);
    MenuInput::g_useSetCursorPos.store(CfgBool(userConfig, "MenuUseSetCursorPos", true));
    MenuInput::g_clickViaSendInput.store(CfgBool(userConfig, "MenuClickViaSendInput", false));
    g_theaterThrottleMs = m_TheaterHideThrottleMs;
    m_MenuUseVguiInternal = CfgBool(userConfig, "MenuInputVguiInternal", m_MenuUseVguiInternal);
    Game::logMsg("DisplayMode=%s frustumCrop=%d menuWin32=%d menuVgui=%d",
                 m_DisplayMode == Display_Compositor ? "compositor"
                   : (m_DisplayMode == Display_SBS ? "sbs" : "both"),
                 (int)m_UseTextureBounds, (int)m_MenuUseWin32, (int)m_MenuUseVguiInternal);
    m_HudDistance = CfgFloat(userConfig, "HudDistance", m_HudDistance);
    m_HudSize = CfgFloat(userConfig, "HudSize", m_HudSize);
    m_HudAlwaysVisible = CfgBool(userConfig, "HudAlwaysVisible", m_HudAlwaysVisible);

    m_ShowWristHUD = CfgBool(userConfig, "ShowWristHUD", m_ShowWristHUD);
    m_WristLookMaxDistance = CfgFloat(userConfig, "WristLookMaxDistance", m_WristLookMaxDistance);
    m_WristLookMinDot = CfgFloat(userConfig, "WristLookMinDot", m_WristLookMinDot);
    m_WristWatchWidth = CfgFloat(userConfig, "WristWatchWidth", m_WristWatchWidth);
    m_WristAmmoWidth = CfgFloat(userConfig, "WristAmmoWidth", m_WristAmmoWidth);
    m_WristOffset = CfgVec(userConfig, "WristOffset", m_WristOffset);
    m_WristRotationDeg = CfgVec(userConfig, "WristRotation", m_WristRotationDeg);
    m_WristWatchFineOffset = CfgVec(userConfig, "WristWatchOffset", m_WristWatchFineOffset);
    m_WristAmmoFineOffset = CfgVec(userConfig, "WristAmmoOffset", m_WristAmmoFineOffset);
    m_WristWatchBounds = CfgBounds(userConfig, "WristWatchUMin", "WristWatchVMin", "WristWatchUMax", "WristWatchVMax", m_WristWatchBounds);
    m_WristAmmoBounds = CfgBounds(userConfig, "WristAmmoUMin", "WristAmmoVMin", "WristAmmoUMax", "WristAmmoVMax", m_WristAmmoBounds);
    m_HurtHUDBounds = CfgBounds(userConfig, "HurtHUDUMin", "HurtHUDVMin", "HurtHUDUMax", "HurtHUDVMax", m_HurtHUDBounds);
    m_HurtHUDWidth = CfgFloat(userConfig, "HurtHUDWidth", m_HurtHUDWidth);
    m_HurtHUDDistance = CfgFloat(userConfig, "HurtHUDDistance", m_HurtHUDDistance);
    m_HurtHUDSeconds = CfgFloat(userConfig, "HurtHUDSeconds", m_HurtHUDSeconds);
    m_HurtHealthThreshold = CfgInt(userConfig, "HurtHealthThreshold", m_HurtHealthThreshold);
    m_EnableNetVR = CfgBool(userConfig, "EnableNetVR", m_EnableNetVR);
    if (m_Game)
        m_Game->m_EnableNetVR = m_EnableNetVR;
}

void VR::WaitForConfigUpdate()
{
    char configDir[MAX_STR_LEN];
    MakeVRPath(configDir, MAX_STR_LEN, "");
    HANDLE fileChangeHandle = FindFirstChangeNotificationA(configDir, false, FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (fileChangeHandle == INVALID_HANDLE_VALUE)
    {
        Game::logMsg("Config watch not started (no %s)", configDir);
        ParseConfigFile();
        return;
    }

    char configPath[MAX_STR_LEN];
    MakeVRPath(configPath, MAX_STR_LEN, "config.txt");

    std::filesystem::file_time_type configLastModified;
    while (1)
    {
        try 
        {
            auto configModifiedTime = std::filesystem::last_write_time(configPath);
            if (configModifiedTime != configLastModified)
            {
                configLastModified = configModifiedTime;
                ParseConfigFile();
            }
        }
        catch (const std::invalid_argument &)
        {
            Game::logMsg("Failed to parse config.txt");
        }
        catch (const std::filesystem::filesystem_error &)
        {
            Game::logMsg("config.txt not found at %s", configPath);
            Sleep(1000);
            continue;
        }
        
        FindNextChangeNotification(fileChangeHandle);
        WaitForSingleObject(fileChangeHandle, INFINITE);
        Sleep(100);
    }
}