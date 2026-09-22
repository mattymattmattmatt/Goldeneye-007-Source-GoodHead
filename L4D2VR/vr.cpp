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
#include <cstddef>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <atomic>
#include <mutex>
#include <d3d9_vr.h>
#include <tlhelp32.h>
#include "vr_settings.h"
#include "vr_watch.h"
#include "vr_toast.h"

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
static std::atomic<bool> g_vrQuitting{ false };
// Time of the last click queued on a game menu (main or pause).
static std::atomic<long long> g_lastMenuClickMs{ 0 };

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

// Is the stalled Present thread sitting inside steamclient.dll? The pause-menu
// Quit hang (14:16 log) is an ntdll wait with steamclient on every frame above
// it. A map load stalls Present too, but in engine/materialsystem, so this is
// what separates "Quit hung" from "still loading".
static bool GESVR_StalledInSteamClient()
{
    if (!g_presentThread || SuspendThread(g_presentThread) == (DWORD)-1)
        return false;

    bool found = false;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(g_presentThread, &ctx))
    {
        char desc[320];
        int resolved = 0;
        DWORD_PTR *sp = (DWORD_PTR *)ctx.Esp;
        for (int i = 0; i < 1024 && resolved < 6 && !found; ++i)
        {
            DWORD_PTR val = 0;
            __try { val = sp[i]; }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            if (GESVR_DescribeAddr(val, desc, sizeof(desc)))
            {
                ++resolved;
                found = (_strnicmp(desc, "steamclient.dll+", 16) == 0);
            }
        }
    }

    ResumeThread(g_presentThread);
    return found;
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

// The 17:17 freeze sat in DXVK's Present waiting on its own submission thread
// (D3D9SwapChainEx::PresentImage -> DxvkDevice::waitForSubmission), so the
// Present thread alone cannot say what is stuck: the answer is on DXVK's
// worker threads. Log every other thread that has our d3d9.dll on its stack.
//
// Nothing that can take a lock runs while a thread is suspended -- it may be
// holding the heap or loader lock. Only registers and a raw copy of its stack
// are taken; module lookups and logging happen after it is resumed.
// Suspend, take EIP and a raw copy of the stack, resume. Separate function:
// __try cannot share a frame with objects that need unwinding.
static int GESVR_CopyThreadStack(HANDLE th, DWORD_PTR *out, int maxWords, DWORD_PTR &eip)
{
    int words = 0;
    eip = 0;
    if (SuspendThread(th) == (DWORD)-1)
        return 0;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(th, &ctx))
    {
        eip = ctx.Eip;
        const DWORD_PTR *sp = (const DWORD_PTR *)ctx.Esp;
        for (; words < maxWords; ++words)
        {
            __try { out[words] = sp[words]; }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        }
    }
    ResumeThread(th);
    return words;
}

static void GESVR_CaptureWorkerStacks()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    const DWORD presentId = g_presentThread ? GetThreadId(g_presentThread) : 0;
    static DWORD_PTR stackCopy[768];
    int logged = 0;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok && logged < 8; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || te.th32ThreadID == presentId)
            continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                               FALSE, te.th32ThreadID);
        if (!th)
            continue;

        DWORD_PTR eip = 0;
        const int words = GESVR_CopyThreadStack(th, stackCopy, 768, eip);
        CloseHandle(th);

        char frames[12][96];
        int n = 0;
        bool ours = false;
        char desc[320];
        if (GESVR_DescribeAddr(eip, desc, sizeof(desc)))
        {
            strncpy_s(frames[n++], desc, _TRUNCATE);
            ours = ours || (_strnicmp(desc, "d3d9.dll+", 9) == 0);
        }
        for (int i = 0; i < words && n < 12; ++i)
            if (GESVR_DescribeAddr(stackCopy[i], desc, sizeof(desc)))
            {
                strncpy_s(frames[n++], desc, _TRUNCATE);
                ours = ours || (_strnicmp(desc, "d3d9.dll+", 9) == 0);
            }
        if (!ours)
            continue;
        std::string line;
        for (int i = 0; i < n; ++i)
        {
            line += frames[i];
            line += (i + 1 < n) ? " < " : "";
        }
        Game::logMsg("THREAD %lu: %s", te.th32ThreadID, line.c_str());
        ++logged;
    }
    CloseHandle(snap);
    if (logged == 0)
        Game::logMsg("THREAD: no other thread has d3d9.dll on its stack");
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
            if (reported == 1)
                GESVR_CaptureWorkerStacks();
            // Pause-menu Quit deadlocks inside steamclient.dll (14:16 log:
            // ntdll wait, entire stack steamclient, inMap still 1). Present
            // never returns, so the engine cannot finish quitting. The user
            // already asked to exit, so leave.
            // Quitting from the MAIN menu hangs the same way (21:27 run:
            // inMap=0, every frame steamclient), so any menu click arms it.
            // Two conditions keep a slow map load safe: the last good frame
            // came right after a menu click, and the stuck thread is inside
            // steamclient -- a map load stalls in engine/materialsystem. The
            // click time is not cleared on healthy frames: a Quit click is
            // followed by 1-2 good Presents before steamclient hangs.
            const long long clicked = g_lastMenuClickMs.load();
            if (stalled > 5000 && clicked != 0 && (last - clicked) < 3000 &&
                GESVR_StalledInSteamClient())
            {
                Game::logMsg("WATCHDOG: menu click then Present stuck in steamclient %lld ms; forcing exit", stalled);
                g_vrQuitting.store(true);
                g_watchdogRun.store(false);
                TerminateProcess(GetCurrentProcess(), 0);
            }
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
    static std::atomic<bool> g_fitWindow{ true };
    // Click delivery. PostMessage synthesises a message with no hardware input
    // state behind it; VGUI calls SetMouseCapture on button-down, and a
    // synthetic press whose physical button was never down can leave it waiting
    // for a release that cannot come. SendInput goes through the real input
    // queue instead, exactly like a physical mouse. Safe from this thread --
    // this thread owns all USER32.
    static std::atomic<bool> g_clickViaSendInput{ false };

    // Is the game showing the OS cursor? Source hides and re-centres it during
    // gameplay and shows it when a VGUI panel takes the mouse. That makes it a
    // reliable, signature-free "a menu wants clicks" signal -- and unlike
    // IsGameUIVisible() it is true for the in-map character/team select panel,
    // which is the one that still needed the desktop mouse.
    // GetCursorInfo is USER32, so it is polled on THIS thread and nowhere else.
    static std::atomic<bool> g_gameCursorShowing{ false };
    // Last second of raw cursor polls, for the CURSOR log line: how many saw
    // it showing, how many of those had a NULL cursor image, how many sat
    // within 4 px of the window centre (where Source parks it in play).
    static std::atomic<int> g_curPolls{ 0 }, g_curVisible{ 0 }, g_curNull{ 0 }, g_curCentred{ 0 };
    // Numpad presses for the weapon tuning (VR::ProcessTuneKeys): counted
    // here, on the thread that owns USER32, applied on the render thread.
    // 0-9 are the digits, 10 is +, 11 is -, 12 is the decimal point.
    static std::atomic<unsigned> g_tunePress[13];

    // HUD toggle key. Polled here because this thread owns USER32; the render
    // thread just watches the sequence number.

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

            // Numpad (Num Lock on). Direction keys repeat while held.
            {
                static const int kKeys[13] = { VK_NUMPAD0, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4,
                                               VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9,
                                               VK_ADD, VK_SUBTRACT, VK_DECIMAL };
                static bool s_down[13] = {};
                static ULONGLONG s_since[13] = {}, s_repeat[13] = {};
                const ULONGLONG t = GetTickCount64();
                for (int k = 0; k < 13; ++k)
                {
                    const bool down = (GetAsyncKeyState(kKeys[k]) & 0x8000) != 0;
                    const bool repeats = (k == 2 || k == 3 || k == 4 || k == 6 || k == 8 || k == 9);
                    if (down && !s_down[k])
                    {
                        g_tunePress[k].fetch_add(1);
                        s_since[k] = s_repeat[k] = t;
                    }
                    else if (down && repeats && t - s_since[k] > 350 && t - s_repeat[k] > 90)
                    {
                        g_tunePress[k].fetch_add(1);
                        s_repeat[k] = t;
                    }
                    s_down[k] = down;
                }
            }

            // CURSOR_SHOWING is global, so the pointer merely wandering off the
            // window during play would read as 'showing' and wrongly flip us into
            // menu mode. Require it to be over the game window as well. This is
            // now only the fallback signal -- see VguiCursorVisible.
            CURSORINFO ci{}; ci.cbSize = sizeof(ci);
            RECT wr{};
            bool curVis = false;
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && GetWindowRect(hwnd, &wr))
                curVis = (ci.ptScreenPos.x >= wr.left && ci.ptScreenPos.x < wr.right &&
                          ci.ptScreenPos.y >= wr.top  && ci.ptScreenPos.y < wr.bottom);

            {
                static int s_polls = 0, s_vis = 0, s_null = 0, s_centred = 0;
                static ULONGLONG s_statStart = 0;
                const ULONGLONG t = GetTickCount64();
                ++s_polls;
                if (curVis)
                {
                    ++s_vis;
                    if (!ci.hCursor) ++s_null;
                    const int cx = (wr.left + wr.right) / 2, cy = (wr.top + wr.bottom) / 2;
                    if (abs(ci.ptScreenPos.x - cx) <= 4 && abs(ci.ptScreenPos.y - cy) <= 4) ++s_centred;
                }
                if (t - s_statStart >= 1000)
                {
                    g_curPolls.store(s_polls); g_curVisible.store(s_vis);
                    g_curNull.store(s_null); g_curCentred.store(s_centred);
                    s_polls = s_vis = s_null = s_centred = 0;
                    s_statStart = t;
                }
            }

            // Hysteresis, light: the cursor read as shown on some polls and
            // hidden on others while a panel was up. On needs it in 3 of the
            // last 8 polls (~64 ms), so a stray blip does not pop a menu; off
            // needs 250 ms unseen. (400 ms plus the narrowed VGUI guard locked
            // menu mode on for a whole map -- see dVGui_Paint.)
            {
                static unsigned s_hist = 0;
                static bool s_on = false;
                static ULONGLONG s_lastSeen = 0;
                const ULONGLONG t = GetTickCount64();
                s_hist = ((s_hist << 1) | (curVis ? 1u : 0u)) & 0xFFu;
                if (curVis)
                    s_lastSeen = t;
                int seen = 0;
                for (unsigned b = s_hist; b; b >>= 1)
                    seen += (int)(b & 1u);
                if (!s_on && seen >= 3)
                    s_on = true;
                else if (s_on && t - s_lastSeen > 250)
                    s_on = false;
                g_gameCursorShowing.store(s_on);
            }

            g_foreground.store(GetForegroundWindow() == hwnd ? 1 : 0);
            g_iconic.store(IsIconic(hwnd) ? 1 : 0);
            g_visible.store(IsWindowVisible(hwnd) ? 1 : 0);

            // Keep the window on ONE monitor. At higher render resolutions the
            // game window was reported spanning both displays. Only the captured
            // backbuffer reaches the headset, so a stretched desktop window costs
            // nothing visually -- but it is disruptive to work around. Done here
            // because this thread owns all USER32; doing it from the render
            // thread is what deadlocked the game earlier in this project.
            // Retry for the first ~30s rather than once: the worker starts while
            // the window is still being created, so a single early check saw a
            // half-built window, decided nothing was wrong and never looked
            // again. Also RESIZES, not just moves -- moving a window wider than
            // the monitor cannot make it fit.
            // Enumerate EVERY visible top-level window this process owns, once.
            // The game window measures correctly placed on the primary monitor,
            // yet the display is still reported as spanning both screens - so
            // something else is on the second one. This says what.
            // The visible-window enumeration that used to live here is GONE.
            // It read window titles inside an enumeration callback; that call sends
            // WM_GETTEXT and BLOCKS until the owning thread pumps messages. It fired
            // on an 8s timer, which landed while the main thread was loading a map
            // and not pumping -- so it hung there, holding this thread, and the
            // render thread stalled behind it. That is the USER32 -> callback
            // dispatcher -> us -> wait stack seen in the freeze logs.
            // It had already answered its question: ONE window, correctly placed,
            // 1926x1109 for a 1920x1080 client (borders). Do not reintroduce it.

            static DWORD s_firstSeen = 0;
            static DWORD s_lastFit = 0;
            static int   s_fitLogs = 0;
            if (g_fitWindow.load())
            {
                const DWORD tnow = GetTickCount();
                if (s_firstSeen == 0) s_firstSeen = tnow;
                if ((tnow - s_firstSeen) < 30000 && (s_lastFit == 0 || (tnow - s_lastFit) >= 2000))
                {
                    s_lastFit = tnow;
                    RECT wr{};
                    if (GetWindowRect(hwnd, &wr))
                    {
                        const int w  = wr.right - wr.left;
                        const int h  = wr.bottom - wr.top;
                        const int sw = GetSystemMetrics(SM_CXSCREEN);
                        const int sh = GetSystemMetrics(SM_CYSCREEN);
                        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                        const bool tooWide = (w > sw + 8);
                        const bool offPrimary = (wr.left < -8) || (wr.right > sw + 8);
                        if (s_fitLogs < 8)
                        {
                            Game::logMsg("WINDOWFIT rect=(%ld,%ld)-(%ld,%ld) %dx%d primary=%dx%d virtual=%d tooWide=%d offPrimary=%d",
                                         wr.left, wr.top, wr.right, wr.bottom, w, h, sw, sh, vw,
                                         (int)tooWide, (int)offPrimary);
                            ++s_fitLogs;
                        }
                        if (tooWide || offPrimary)
                        {
                            const int nw = tooWide ? sw : w;
                            const int nh = (h > sh) ? sh : h;
                            const int nx = (sw > nw) ? (sw - nw) / 2 : 0;
                            const int ny = (sh > nh) ? (sh - nh) / 2 : 0;
                            SetWindowPos(hwnd, nullptr, nx, ny, nw, nh,
                                         SWP_NOZORDER | SWP_NOACTIVATE);
                            Game::logMsg("WINDOWFIT moved/resized to (%d,%d) %dx%d", nx, ny, nw, nh);
                        }
                    }
                }
            }

            if (!g_enabled.load())
                continue;

            const unsigned aimSeq = g_aimSeq.load();
            if (aimSeq != lastAim)
            {
                lastAim = aimSeq;
                const int x = g_aimX.load();
                const int y = g_aimY.load();
                // Rate-limit and dead-zone.
                //
                // The angular head fallback always returns a position, and a head
                // is never perfectly still, so this went from "a few updates" to
                // SetCursorPos at up to 120Hz - fighting Source's own mouse
                // handling and hanging the game 27 stereo passes into a map.
                // A few pixels of dead zone and a 30Hz cap keep it usable while
                // removing the flood.
                static DWORD s_lastMove = 0;
                const DWORD mnow = GetTickCount();
                const int dx = (x > lastX) ? (x - lastX) : (lastX - x);
                const int dy = (y > lastY) ? (y - lastY) : (lastY - y);
                const bool moved = (lastX < 0) || (dx > 3) || (dy > 3);
                if (x >= 0 && y >= 0 && moved && (mnow - s_lastMove) >= 33)
                {
                    s_lastMove = mnow;
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

namespace dxvk { extern bool g_GESVR_ReticleUseAim; extern bool g_GESVR_ReticleAimValid[2];
                extern float g_GESVR_ReticleAimU[2]; extern float g_GESVR_ReticleAimV[2]; }
extern bool GESVR_MuzzleWorld(Vector &out, Vector &dir);
namespace dxvk { extern bool g_GESVR_DrawReticle; extern float g_GESVR_ReticleScale;
                extern int g_GESVR_ReticleStyle; extern int g_GESVR_ReticleColor;
                extern bool g_GESVR_ForceMenuOpaque;
                extern float g_GESVR_ReticleAspect;
                extern bool g_GESVR_SwapEyeSurfaces;
                extern bool g_GESVR_ReticleForce; }
extern long GESVR_ExecMoveCount();
extern long GESVR_RenderOriginCalls();
extern long GESVR_RenderAnglesCalls();

// How many SteamVR laser events arrived last frame. Non-zero means the laser
// is driving the cursor and our own marker would be redundant.

static bool g_menuPlaced = false;
static float g_menuYaw = 0.0f;

// Re-place the game menu panel on its next frame (menu size/distance changed).
void GESVR_RequestMenuReplace()
{
    g_menuPlaced = false;
}

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
    // Vertical crop: same sign convention as the horizontal above.
    //
    // This used to read (0.5 - 0.5*bottom) / (0.5 - 0.5*top), which flips the
    // convention between the two axes. With the measured frustum that produced
    // v[0.162..1.000] -- the BOTTOM 84% of the image -- where the eye actually
    // needs the TOP 84%, v[0.000..0.838]. Sampling too low pushes everything in
    // view upward, which is why the gun sat so high. EyeCropLegacyV restores the
    // old behaviour if this is ever wrong for a different headset.
    m_TextureBounds[0].vMin = m_EyeCropLegacyV ? (0.5f - 0.5f * l_bottom / tanHalfFov[1])
                                               : (0.5f + 0.5f * l_top    / tanHalfFov[1]);
    m_TextureBounds[0].vMax = m_EyeCropLegacyV ? (0.5f - 0.5f * l_top    / tanHalfFov[1])
                                               : (0.5f + 0.5f * l_bottom / tanHalfFov[1]);

    m_TextureBounds[1].uMin = 0.5f + 0.5f * r_left / tanHalfFov[0];
    m_TextureBounds[1].uMax = 0.5f + 0.5f * r_right / tanHalfFov[0];
    m_TextureBounds[1].vMin = m_EyeCropLegacyV ? (0.5f - 0.5f * r_bottom / tanHalfFov[1])
                                               : (0.5f + 0.5f * r_top    / tanHalfFov[1]);
    m_TextureBounds[1].vMax = m_EyeCropLegacyV ? (0.5f - 0.5f * r_top    / tanHalfFov[1])
                                               : (0.5f + 0.5f * r_bottom / tanHalfFov[1]);

    m_Aspect = tanHalfFov[0] / tanHalfFov[1];
    m_Fov = 2.0f * atan(tanHalfFov[0]) * 360 / (3.14159265358979323846 * 2);
    m_HaveTextureBounds = (tanHalfFov[0] > 0.01f && tanHalfFov[1] > 0.01f);

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
    VRSettings::Init(this);
    VRWatch::Init(this);
    VRToast::Init(this);
    {
        char weapons[MAX_STR_LEN];
        MakeVRPath(weapons, sizeof(weapons), "weapons.txt");
        Game::logMsg("Weapon positions: %s %s", weapons,
                     Weapons::LoadOverrides(weapons) ? "loaded" : "not found (built-in table)");
    }

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
    m_Input->GetActionHandle("/actions/main/in/Scope", &m_ActionScope);
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
    if (!m_IsInitialized || g_vrQuitting.load())
        return;

    // Once per frame; see IsMenuMode. Transitions are logged so a flicker
    // shows up as a burst of these instead of hiding between 1-in-90 samples.
    {
        const bool menu = ComputeMenuMode();
        if (menu != m_MenuMode)
        {
            static ULONGLONG s_windowStart = 0;
            static int s_flips = 0;
            const ULONGLONG t = GetTickCount64();
            if (t - s_windowStart > 1000) { s_windowStart = t; s_flips = 0; }
            if (++s_flips <= 4)
                Game::logMsg("Menu mode %d -> %d (gameui=%d inmap=%d vgui=%d cursor=%d stereo=%d)",
                             (int)m_MenuMode, (int)menu,
                             (int)(m_Game && m_Game->IsGameUIVisible()),
                             (int)(m_Game && m_Game->IsInMap()), m_VguiCursor,
                             (int)MenuInput::g_gameCursorShowing.load(), (int)m_RenderedNewFrame);
            m_MenuMode = menu;
        }
    }

    GESVR_HideTheaterOverlays();
    VRToast::Update();

    static int s_frames = 0;
    // The engine may install its own spew function after ours; re-chain.
    if ((s_frames % 120) == 0)
        VRSettings::InstallMenuHook();
    if ((s_frames % 90) == 0 && m_Game && m_Game->IsInMap())
        Game::logMsg("CURSOR vgui=%d | win32 polls=%d visible=%d nullImage=%d centred=%d -> menu=%d",
                     m_VguiCursor, MenuInput::g_curPolls.load(), MenuInput::g_curVisible.load(),
                     MenuInput::g_curNull.load(), MenuInput::g_curCentred.load(), (int)m_MenuMode);
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
        VRWatch::Hide();
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
        // Non-blocking. WaitGetPoses on the Present thread froze disconnect
        // and the next launch's menu/load.
        if (vr::VRCompositor())
            vr::VRCompositor()->GetLastPoses(m_Poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        GetPoses();

        // Exactly one cursor: SteamVR's dot while its laser is on the panel,
        // otherwise our marker at the controller aim from the last
        // ProcessMenuInput (one frame old, which is invisible). Never while
        // the VR settings panel is in front, which has its own pointer.
        const bool wantMarker = m_DrawMenuCursor && !m_MenuLaserOnPanel && m_MenuAimX >= 0
                             && !VRSettings::IsOpen();
        if (g_D3DVR9)
            g_D3DVR9->CaptureForOverlay(&m_VKHUD,
                                        wantMarker ? m_MenuAimX : -1,
                                        wantMarker ? m_MenuAimY : -1);
        const auto tCapture = vrclock::now();
        ShowMenuPanel();
        const auto tPanel = vrclock::now();
        // VR settings: opened by our "VR Settings" GameMenu entry, or left X
        // in any menu. While it is open it owns the pointer and the trigger,
        // so the game menu behind it gets no input at all.
        static int s_settingsFrames = 0;
        ++s_settingsFrames;
        if (VRSettings::ConsumeOpenRequest())
            VRSettings::Open();
        else if (s_settingsFrames > 300 && PressedDigitalAction(m_Scoreboard, true))
            VRSettings::IsOpen() ? VRSettings::Close() : VRSettings::Open();
        if (VRSettings::IsOpen())
            VRSettings::Frame();
        else
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
    VRSettings::Close();

    // Refresh the flat-HUD capture while IN GAME. The HUD elements crop
    // their faces out of m_VKHUD, but m_VKHUD was only ever filled by the
    // MENU branch -- so in game it held a stale menu frame forever. At
    // Present the backbuffer holds the finished frame including the 2D HUD,
    // which is exactly what we want to crop from. (The wrist watch draws its
    // own face and does not need this.)
    //
    // This capture is also what runs ForceOpaqueAlpha. While the radar existed
    // the gate was true every frame; turning the HUD off silently stopped it,
    // and the menu went see-through/blurry again at the same time. Decoupled
    // so HUD settings cannot switch the alpha fix off as a side effect.
    if (g_D3DVR9 && m_AlwaysCaptureOverlay)
        g_D3DVR9->CaptureForOverlay(&m_VKHUD, -1, -1);

    // Only runs in map.
    if (m_Game && m_Game->IsInMap())
    {
        ApplyExtraCvars();
        RefreshActiveWeapon();
        UpdateGameCrosshair();
        if (m_GraphicsDirty && m_ExtraCvarsDone)
            ApplyGraphicsCvars();
        ProcessTuneKeys();
        VRWatch::Update();
        UpdateHurtHUD();
    }
    else
    {
        m_ExtraCvarsDone = false;
        m_InMapSinceMs = 0;
        VRWatch::Hide();
    }

    ProcessInput();
    const auto tEnd = vrclock::now();
    if (trace)
        Game::logMsg("GAME f=%d input=%.1f TOTAL=%.1fms",
                     s_frames, MsSince(tStart, tEnd), MsSince(tStart, tEnd));
}

static bool ReadablePtr(const void *p, size_t bytes);

// VGUI's own "a panel needs the mouse" -- ISurface::IsCursorVisible, which is
// `return _currentCursor != dc_none`. VGUI sets dc_none whenever no visible
// popup takes mouse input, so this is exactly what the game uses to decide.
// The Win32 cursor it replaces stayed visible through a whole map on
// 2026-09-21 (118 of 118 polls a second, never centred) and locked the
// headset in menu mode.
//
// Vtable slot 51 in GE:S's vguimatsurface.dll, found by disassembling
// CMatSystemSurface's vtable (via RTTI): slot 51 is that one-liner and slot
// 50, SetCursor, writes the same field. The SDK header's slot 52 would have
// called the wrong function, so the bytes are checked before it is ever
// called: -1 means "not trusted", and the caller falls back to Win32.
static int VguiCursorVisible(void *surface)
{
    static int s_state = 0;           // 0 unchecked, 1 verified, -1 unusable
    static void *s_fn = nullptr;
    if (!surface)
        return -1;
    if (s_state == 0)
    {
        s_state = -1;
        void **vt = ReadablePtr(surface, sizeof(void *)) ? *reinterpret_cast<void ***>(surface) : nullptr;
        if (vt && ReadablePtr(vt + 51, sizeof(void *)))
        {
            // xor eax,eax / cmp dword ptr [ecx+disp32],1 / setne al / ret
            const unsigned char *p = static_cast<const unsigned char *>(vt[51]);
            if (ReadablePtr(p, 13) && p[0] == 0x33 && p[1] == 0xC0 && p[2] == 0x83 && p[3] == 0xB9 &&
                p[8] == 0x01 && p[9] == 0x0F && p[10] == 0x95 && p[11] == 0xC0 && p[12] == 0xC3)
            {
                s_fn = vt[51];
                s_state = 1;
            }
        }
        Game::logMsg("VGUI IsCursorVisible %s", s_state == 1 ? "verified at vtable slot 51"
                                                             : "NOT recognised -- falling back to the Win32 cursor");
    }
    if (s_state != 1)
        return -1;
    return reinterpret_cast<bool(__thiscall *)(void *)>(s_fn)(surface) ? 1 : 0;
}

bool VR::ComputeMenuMode()
{
    if (m_Game && m_Game->IsGameUIVisible())
        return true;

    // In-map panels (character/team/level select) are NOT "GameUI visible", so
    // this returned false for them and the entire menu branch -- capture, flat
    // panel, pointer, click -- never ran. The panel stayed inside the 3D view,
    // where the per-eye frustum crop magnified it ("too big and stretched") and
    // nothing but the desktop mouse could reach it.
    //
    // Routing it through the same flat panel puts it at MenuDistanceMeters and
    // makes the VR pointer work on it. Gated by InGameMenuPanel in case a map
    // ever shows the cursor with no panel behind it.
    m_VguiCursor = VguiCursorVisible(m_Game ? m_Game->m_VguiSurface : nullptr);
    if (m_InGameMenuPanel && m_Game && m_Game->IsInMap())
    {
        const bool panelWantsMouse = (m_VguiCursor >= 0) ? (m_VguiCursor == 1)
                                                         : MenuInput::g_gameCursorShowing.load();
        if (panelWantsMouse)
            return true;
    }

    return !m_RenderedNewFrame;
}

static bool g_pendMouseDown = false;
static bool g_pendMouseUp = false;
static HWND g_pendHwnd = nullptr;
static int g_lastCursorX = 0;
static int g_lastCursorY = 0;
static bool g_haveCursor = false;

void GESVR_OnProcessDetach()
{
    g_vrQuitting.store(true);
    g_watchdogRun.store(false);
    MenuInput::g_run.store(false);
    VRSubmit::g_run.store(false);
}

void VR::AfterPresent()
{
    if (g_vrQuitting.load())
        return;

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
    {
        MenuInput::QueueClick();
        // Arms the watchdog's Quit force-exit (main or pause menu).
        g_lastMenuClickMs.store(NowMs());
    }
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
        // Disconnect/quit returns to a 2D menu. Re-place the panel level
        // (no leftover in-game tilt) and stop compositor submits -- that is
        // what deadlocked Present after the 12:09:10 disconnect click.
        if (!inMap)
            g_menuPlaced = false;
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
    // Only the GameUI pause/quit overlay skips compositor. Character select
    // is in-map + cursor, NOT GameUI -- skipping WaitGetPoses there broke
    // the 3D character screen. Pause is IsGameUIVisible().
    const bool pauseUi = m_Game && m_Game->IsGameUIVisible();
    if (!VRSubmit::g_useThread.load() && vr::VRCompositor() && inMap && !pauseUi)
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

        // Character/load-game: the same VGUI is in the stereo eyes and gets
        // frustum-cropped into a giant stretched copy behind the overlay.
        // Submit black there. The floating overlay still captures the
        // backbuffer, so the good panel is unchanged.
        const bool hideWorldUi = IsMenuMode();
        if (hideWorldUi && TextureReady(m_SubmitBlack))
        {
            el = comp->Submit(vr::Eye_Left,  &m_SubmitBlack.m_VRTexture, &full, vr::Submit_Default);
            er = comp->Submit(vr::Eye_Right, &m_SubmitBlack.m_VRTexture, &full, vr::Submit_Default);
            submitted = true;
        }
        else if (haveEyes && inMap)
        {
            const bool useBounds = m_UseTextureBounds && m_HaveTextureBounds;
            vr::VRTextureBounds_t lb = useBounds ? m_TextureBounds[0] : full;
            vr::VRTextureBounds_t rb = useBounds ? m_TextureBounds[1] : full;
            if (!m_UseVerticalCrop)
            {
                lb.vMin = rb.vMin = 0.0f;
                lb.vMax = rb.vMax = 1.0f;
            }
            // Mono: send the LEFT image to both eyes.
            //
            // The left eye renders correctly at full resolution; the right eye
            // comes back black and the cause is still open. Duplicating the left
            // gives a correct, sharp, comfortable image with no stereo depth,
            // which beats playing with one eye blacked out. MonoEye=false
            // restores true stereo once the right eye is fixed.
            // One-time: are the two eye textures actually distinct and valid?
            // Mono works by submitting the LEFT texture for both eyes, so the
            // only thing separating working mono from working stereo is whether
            // m_VKRightEye is a good handle. Reading these fields costs nothing
            // and touches no GPU state.
            {
                static bool s_once = false;
                if (!s_once)
                {
                    s_once = true;
                    Game::logMsg("EYETEX left img=%llu %ux%u | right img=%llu %ux%u | same=%d",
                                 (unsigned long long)m_VKLeftEye.m_VulkanData.m_nImage,
                                 m_VKLeftEye.m_VulkanData.m_nWidth, m_VKLeftEye.m_VulkanData.m_nHeight,
                                 (unsigned long long)m_VKRightEye.m_VulkanData.m_nImage,
                                 m_VKRightEye.m_VulkanData.m_nWidth, m_VKRightEye.m_VulkanData.m_nHeight,
                                 (int)(m_VKLeftEye.m_VulkanData.m_nImage == m_VKRightEye.m_VulkanData.m_nImage));
                }
            }

            if (m_MonoEye)
            {
                // Same texture, but each eye keeps its OWN crop.
                //
                // Sending the left eye's crop to both eyes hands the right eye a
                // picture built for the left eye's frustum, which reads as warped
                // and mismatched. The render is the SUPERSET frustum and covers
                // both eyes, so cropping it with each eye's own bounds gives each
                // a geometrically correct view. What is lost is only the parallax
                // between them, which is exactly what mono means.
                // MonoEyeSource picks WHICH texture feeds both eyes. This is the
                // test that isolates the right eye's black frame: if the right
                // texture shown to both eyes looks correct, the texture is fine
                // and the fault is in submitting two textures in one frame. If
                // it is black in both eyes, the texture itself is empty.
                SharedTextureHolder &src = (m_MonoEyeSource == 1) ? m_VKRightEye : m_VKLeftEye;
                el = comp->Submit(vr::Eye_Left,  &src.m_VRTexture, &lb, vr::Submit_Default);
                er = comp->Submit(vr::Eye_Right, &src.m_VRTexture, &rb, vr::Submit_Default);
            }
            else
            {
                el = comp->Submit(vr::Eye_Left,  &m_VKLeftEye.m_VRTexture,  &lb, vr::Submit_Default);
                er = comp->Submit(vr::Eye_Right, &m_VKRightEye.m_VRTexture, &rb, vr::Submit_Default);
            }
            submitted = true;
        }
        // Do NOT Submit black on the menu. WaitGetPoses+Submit on the Present
        // thread after disconnect is the 12:09 hang (inMap 1->0, then Present
        // blocked in ntdll, called from vgui2). Menu is overlay-only.

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

    // Overlay texture AFTER the swap, with the queue lock. Doing this in
    // Update() (before Present) is a vkQueueSubmit vs DXVK Present race.
    if (IsMenuMode() && m_Overlay && m_MainMenuHandle && TextureReady(m_VKHUD))
        SetOverlayTextureLocked(m_Overlay, m_MainMenuHandle, &m_VKHUD.m_VRTexture);

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

    // Texture upload happens in AfterPresent (after DXVK Present) so OpenVR
    // does not vkQueueSubmit on the same callstack as the swap.

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
        Game::logMsg("Floating menu overlay %dx%d visible=%d",
                     windowWidth, windowHeight,
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
        float panelW = m_MenuWidthMeters, dist = m_MenuDistanceMeters;
        EffectiveMenuGeometry(panelW, dist);
        // Yaw only. Copying the full HMD matrix locked the panel to whatever
        // pitch/roll you had when the menu first appeared.
        float fx = -m.m[0][2], fz = -m.m[2][2];
        const float flen = sqrtf(fx * fx + fz * fz);
        if (flen > 0.001f) { fx /= flen; fz /= flen; }
        else { fx = 0.0f; fz = -1.0f; }
        // Columns: right, up, back (OpenVR). Level: up = (0,1,0).
        xf.m[0][0] = -fz; xf.m[0][1] = 0.0f; xf.m[0][2] = -fx;
        xf.m[1][0] = 0.0f; xf.m[1][1] = 1.0f; xf.m[1][2] = 0.0f;
        xf.m[2][0] =  fx; xf.m[2][1] = 0.0f; xf.m[2][2] = -fz;
        xf.m[0][3] = m.m[0][3] + fx * dist;
        xf.m[1][3] = m.m[1][3];
        xf.m[2][3] = m.m[2][3] + fz * dist;
    }

    m_Overlay->SetOverlayTransformAbsolute(m_MainMenuHandle, vr::VRCompositor()->GetTrackingSpace(), &xf);
    {
        float panelW = m_MenuWidthMeters, panelD = m_MenuDistanceMeters;
        EffectiveMenuGeometry(panelW, panelD);
        m_Overlay->SetOverlayWidthInMeters(m_MainMenuHandle, panelW);
    }
}

// Effective menu panel geometry.
//
// Two separate corrections live here, and BOTH the panel placement and the
// pointer's angular fallback must use the same answer or aiming is miscalibrated.
//
// 1. Resolution. Source lays the GameUI out in FIXED PIXELS, so as the capture
//    gets bigger the menu covers a smaller fraction of it -- which is why the
//    menu shrank and looked further away every time the resolution went up.
//    The overlay maps the whole texture to its width, so scaling the width by
//    the same factor keeps the menu's apparent size constant.
// 2. Pregame vs in-game want different distances: the create-server menu wants
//    to be close enough to read, the in-map character panel wants to be far
//    enough not to feel pressed against your face.
void VR::EffectiveMenuGeometry(float &widthM, float &distM) const
{
    const bool inMap  = m_Game && m_Game->IsInMap();
    const bool gameUi = m_Game && m_Game->IsGameUIVisible();

    // Only the cursor-driven in-map panel (character/level select) wants the far
    // distance. The PAUSE menu is in a map AND is GameUI, so keying purely off
    // inMap pushed it out to the far distance -- which is exactly why it read as
    // the worst of the lot. GameUI menus are text you read, so they stay near.
    distM  = (inMap && !gameUi) ? m_InGameMenuDistance : m_MenuDistanceMeters;
    widthM = m_MenuWidthMeters;
    // Pause (GameUI while in a map) was filling too much of the view once
    // compositor stereo was skipped behind it.
    if (inMap && gameUi)
    {
        widthM = m_MenuWidthMeters * 0.72f;
        if (distM < 1.8f) distM = 1.8f;
    }

    if (m_MenuScaleWithRes)
    {
        int sw = 1920, sh = 1080;
        if (m_Game && m_Game->m_EngineClient)
            m_Game->m_EngineClient->GetScreenSize(sw, sh);
        if (sh > 1)
        {
            float s = (float)sh / 1080.0f;
            if (s < 0.5f) s = 0.5f;
            if (s > 3.0f) s = 3.0f;
            widthM *= s;
        }
    }
    if (distM < 0.3f) distM = 0.3f;
    if (widthM < 0.2f) widthM = 0.2f;
}

// The pointing hand's laser as SteamVR draws it: the controller's "tip"
// component, not its raw pose. Raw -Z on Touch points well below the SteamVR
// laser, so a ray from it lands somewhere else on the panel.
bool VR::GetPointerPose(vr::HmdMatrix34_t &out)
{
    if (!m_System)
        return false;
    const vr::TrackedDeviceIndex_t hand = m_System->GetTrackedDeviceIndexForControllerRole(
        m_LeftHanded ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
    if (hand >= vr::k_unMaxTrackedDeviceCount || !m_Poses[hand].bPoseIsValid)
        return false;

    // The tip is fixed relative to the controller: look it up once per device.
    static vr::TrackedDeviceIndex_t s_tipFor = vr::k_unTrackedDeviceIndexInvalid;
    static vr::HmdMatrix34_t s_tip{};
    static bool s_haveTip = false;
    if (s_tipFor != hand)
    {
        s_tipFor = hand;
        s_haveTip = false;
        char model[256] = {};
        m_System->GetStringTrackedDeviceProperty(hand, vr::Prop_RenderModelName_String, model, sizeof(model));
        vr::VRControllerState_t state{};
        vr::RenderModel_ControllerMode_State_t mode{};
        vr::RenderModel_ComponentState_t comp{};
        if (model[0] && vr::VRRenderModels() &&
            vr::VRRenderModels()->GetComponentState(model, vr::k_pch_Controller_Component_Tip, &state, &mode, &comp))
        {
            s_tip = comp.mTrackingToComponentLocal;
            s_haveTip = true;
        }
        Game::logMsg("Pointer: device %u model '%s' tip %s", hand, model,
                     s_haveTip ? "found" : "not found, using the raw pose");
    }

    const vr::HmdMatrix34_t &d = m_Poses[hand].mDeviceToAbsoluteTracking;
    if (!s_haveTip)
    {
        out = d;
        return true;
    }
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
            out.m[i][j] = d.m[i][0] * s_tip.m[0][j] + d.m[i][1] * s_tip.m[1][j] + d.m[i][2] * s_tip.m[2][j];
        out.m[i][3] = d.m[i][0] * s_tip.m[0][3] + d.m[i][1] * s_tip.m[1][3] + d.m[i][2] * s_tip.m[2][3] + d.m[i][3];
    }
    return true;
}

// Where the pointing hand's ray meets the game menu panel, in window pixels.
// Controller only; false when it points off the panel.
bool VR::ComputeMenuPointer(int &x, int &y)
{
    x = -1;
    y = -1;
    if (!m_Overlay || !m_MainMenuHandle || !vr::VRCompositor())
        return false;

    int windowWidth = 1280, windowHeight = 720;
    if (m_Game && m_Game->m_EngineClient)
        m_Game->m_EngineClient->GetScreenSize(windowWidth, windowHeight);
    if (windowWidth < 1) windowWidth = 1280;
    if (windowHeight < 1) windowHeight = 720;

    vr::HmdMatrix34_t ray{};
    if (!GetPointerPose(ray))
        return false;

    vr::VROverlayIntersectionParams_t ip{};
    ip.eOrigin = vr::VRCompositor()->GetTrackingSpace();
    ip.vSource.v[0] = ray.m[0][3];
    ip.vSource.v[1] = ray.m[1][3];
    ip.vSource.v[2] = ray.m[2][3];
    ip.vDirection.v[0] = -ray.m[0][2];
    ip.vDirection.v[1] = -ray.m[1][2];
    ip.vDirection.v[2] = -ray.m[2][2];
    vr::VROverlayIntersectionResults_t ir{};
    if (!m_Overlay->ComputeOverlayIntersection(m_MainMenuHandle, &ip, &ir))
        return false;
    const float u = ir.vUVs.v[0];
    const float v = 1.0f - ir.vUVs.v[1];
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
        return false;
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
            // Mono: send the LEFT image to both eyes.
            //
            // The left eye renders correctly at full resolution; the right eye
            // comes back black and the cause is still open. Duplicating the left
            // gives a correct, sharp, comfortable image with no stereo depth,
            // which beats playing with one eye blacked out. MonoEye=false
            // restores true stereo once the right eye is fixed.
            if (m_MonoEye)
            {
                // Same texture, but each eye keeps its OWN crop.
                //
                // Sending the left eye's crop to both eyes hands the right eye a
                // picture built for the left eye's frustum, which reads as warped
                // and mismatched. The render is the SUPERSET frustum and covers
                // both eyes, so cropping it with each eye's own bounds gives each
                // a geometrically correct view. What is lost is only the parallax
                // between them, which is exactly what mono means.
                // MonoEyeSource picks WHICH texture feeds both eyes. This is the
                // test that isolates the right eye's black frame: if the right
                // texture shown to both eyes looks correct, the texture is fine
                // and the fault is in submitting two textures in one frame. If
                // it is black in both eyes, the texture itself is empty.
                SharedTextureHolder &src = (m_MonoEyeSource == 1) ? m_VKRightEye : m_VKLeftEye;
                el = comp->Submit(vr::Eye_Left,  &src.m_VRTexture, &lb, vr::Submit_Default);
                er = comp->Submit(vr::Eye_Right, &src.m_VRTexture, &rb, vr::Submit_Default);
            }
            else
            {
                el = comp->Submit(vr::Eye_Left,  &m_VKLeftEye.m_VRTexture,  &lb, vr::Submit_Default);
                er = comp->Submit(vr::Eye_Right, &m_VKRightEye.m_VRTexture, &rb, vr::Submit_Default);
            }
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

// Eye render target size.
//
// Both eyes are rendered with the SUPERSET frustum and cropped per eye at submit
// time by m_TextureBounds, so an eye only uses a fraction of the image. To end
// up with native per-eye resolution AFTER that crop the superset has to be
// correspondingly larger.
//
// Height follows from width via m_Aspect rather than being computed separately:
// the crop bounds assume the texture covers the superset frustum exactly, so any
// other aspect would stretch the result.
//
// Called from CreateVRTextures, NOT from Init: init runs before config.txt is
// parsed, so computing it there silently used the built-in EyeRenderScale and
// ignored the configured one.
void VR::ComputeEyeRTSize()
{
    const float uSpanL = fabsf(m_TextureBounds[0].uMax - m_TextureBounds[0].uMin);
    const float uSpanR = fabsf(m_TextureBounds[1].uMax - m_TextureBounds[1].uMin);
    float uSpan = (uSpanL < uSpanR) ? uSpanL : uSpanR;   // tighter crop needs more pixels
    if (uSpan < 0.20f) uSpan = 0.20f;                    // guard against nonsense bounds

    float sc = m_EyeRenderScale;
    if (sc < 0.5f) sc = 0.5f;
    if (sc > 2.0f) sc = 2.0f;

    float w = ((float)m_RenderWidth / uSpan) * sc;
    // A 32-bit process with DXVK on top does not have room to be greedy.
    if (w < 640.0f)  w = 640.0f;
    if (w > 3072.0f) w = 3072.0f;

    float aspect = (m_Aspect > 0.2f && m_Aspect < 5.0f) ? m_Aspect : 1.0f;
    float h = w / aspect;
    if (h < 640.0f)  h = 640.0f;
    if (h > 3072.0f) { h = 3072.0f; w = h * aspect; }

    m_EyeRTWidth  = (uint32_t)(w + 0.5f);
    m_EyeRTHeight = (uint32_t)(h + 0.5f);
    Game::logMsg("Eye RT size %ux%u (hmd recommends %ux%u per eye, crop keeps %.2f of width, scale %.2f)",
                 m_EyeRTWidth, m_EyeRTHeight, m_RenderWidth, m_RenderHeight, uSpan, sc);
}

void VR::CreateVRTextures()
{
    int windowWidth = 1280, windowHeight = 720;
    if (m_Game->m_EngineClient)
        m_Game->m_EngineClient->GetScreenSize(windowWidth, windowHeight);

    // The eye textures must be exactly the size the viewport will be set to.
    // Creating them at the HMD size while rendering at a window-derived size is
    // what produced the 'only a sliver renders' failure.
    ComputeEyeRTSize();

    // SEPARATE depth, not SHARED: the shared depth buffer is sized for the
    // backbuffer, and these targets are deliberately larger than it. Rendering
    // into a colour target bigger than its depth buffer is exactly the kind of
    // mismatch that produced a partially-drawn frame.
    const int rtW = (m_EyeRTWidth  > 0) ? (int)m_EyeRTWidth  : (int)m_RenderWidth;
    const int rtH = (m_EyeRTHeight > 0) ? (int)m_EyeRTHeight : (int)m_RenderHeight;

    // SDK 2007 CMaterialSystem::m_bGameRunning is not at L4D2's 0x2AB8.
    // Try a normal runtime RT allocation; GE:S still accepts this on 2007.
    m_Game->m_MaterialSystem->BeginRenderTargetAllocation();

    m_CreatingTextureID = Texture_LeftEye;
    m_LeftEyeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("leftEye0", rtW, rtH, RT_SIZE_LITERAL, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SEPARATE, TEXTUREFLAGS_NOMIP);
    
    // One target for both eyes, by default.
    //
    // Each eye is captured immediately after it is rendered, so the second pass
    // can reuse the first one's target -- the clear at the top of each pass wipes
    // it. That saves a full colour AND depth buffer: at this size roughly 52MB of
    // the ~156MB these targets were costing, in a 32-bit process where that
    // margin decides whether a map loads. It costs no resolution at all.
    //
    // It also removes the second SetRenderTarget entirely, which is one of the
    // remaining suspects for the right eye's black frame.
    if (m_SharedEyeTarget)
    {
        m_RightEyeTexture = m_LeftEyeTexture;
    }
    else
    {
        m_CreatingTextureID = Texture_RightEye;
        m_RightEyeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("rightEye0", rtW, rtH, RT_SIZE_LITERAL, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SEPARATE, TEXTUREFLAGS_NOMIP);
    }
    
    m_CreatingTextureID = Texture_HUD;
    m_HUDTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("vrHUD", windowWidth, windowHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);
    
    m_CreatingTextureID = Texture_Blank;
    m_BlankTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("blankTexture", 512, 512, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);
    
    m_CreatingTextureID = Texture_None;

    m_Game->m_MaterialSystem->EndRenderTargetAllocation();

    m_CreatedVRTextures = (m_LeftEyeTexture && m_RightEyeTexture);
    Game::logMsg("CreateVRTextures left=%p right=%p hud=%p ok=%d size=%dx%d shared=%d",
                 m_LeftEyeTexture, m_RightEyeTexture, m_HUDTexture,
                 (int)m_CreatedVRTextures, rtW, rtH, (int)m_SharedEyeTarget);
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

    // Aim: SteamVR's laser point while its laser is on the panel, else our
    // own ray from the same controller tip. SteamVR only sends MouseMove when
    // the laser MOVES, so the last laser point is kept while it holds still;
    // FocusLeave (or a second of silence with our ray off the panel) ends it.
    // Letting a computed ray take over on every still frame is what put a
    // second cursor on the menu and sent clicks to it.
    static int s_laserX = -1, s_laserY = -1;
    static bool s_laserFocus = false;
    static ULONGLONG s_lastLaserMove = 0;
    const ULONGLONG nowMs = GetTickCount64();
    // Any gap (settings panel in front, or back in game) may have eaten the
    // FocusLeave: start clean rather than trust a stale laser point.
    static ULONGLONG s_lastRun = 0;
    if (nowMs - s_lastRun > 250)
    {
        s_laserFocus = false;
        s_laserX = s_laserY = -1;
    }
    s_lastRun = nowMs;
    int overlayMoves = 0;
    bool overlayDown = false;
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
            s_laserX = laserX;
            s_laserY = laserY;
            s_laserFocus = true;
            s_lastLaserMove = nowMs;
            ++overlayMoves;
            break;
        }
        case vr::VREvent_FocusEnter:
            s_laserFocus = true;
            break;
        case vr::VREvent_FocusLeave:
            s_laserFocus = false;
            break;
        case vr::VREvent_MouseButtonDown:
            overlayDown = true;
            break;
        default:
            break;
        }
    }

    int tipX = -1, tipY = -1;
    const bool tipHit = ComputeMenuPointer(tipX, tipY);
    if (s_laserFocus && !tipHit && nowMs - s_lastLaserMove > 1000)
        s_laserFocus = false;   // left the panel without telling us
    int aimX = -1, aimY = -1;
    if (s_laserFocus && s_laserX >= 0)
    {
        aimX = s_laserX;
        aimY = s_laserY;
    }
    else if (tipHit)
    {
        aimX = tipX;
        aimY = tipY;
    }
    m_MenuAimX = aimX;
    m_MenuAimY = aimY;
    m_MenuLaserOnPanel = s_laserFocus;

    // Which pointer is actually driving the cursor, and where. "doesn't use the
    // vr pointer" needs separating into: no aim computed at all, aim computed
    // from the wrong source, or aim computed but not reaching the game.
    {
        static DWORD s_lastAimLog = 0;
        const DWORD an = GetTickCount();
        if (s_lastAimLog == 0 || (an - s_lastAimLog) >= 1000)
        {
            s_lastAimLog = an;
            const bool inMapNow = m_Game && m_Game->IsInMap();
            Game::logMsg("AIMSRC %s inMap=%d overlayMoves=%d tip=%d aim=(%d,%d) cursor=(%d,%d) live=%d",
                         s_laserFocus ? "laser" : (tipHit ? "controller-ray" : "none"),
                         (int)inMapNow, overlayMoves, (int)tipHit,
                         aimX, aimY, g_lastCursorX, g_lastCursorY, (int)cursorLive);
        }
    }

    if (armTrace) Game::logMsg("  f=%d -> publishing aim (%d,%d)", s_menuFrames, aimX, aimY);
    if (cursorLive && aimX >= 0)
        DriveGameCursor(hwnd, aimX, aimY, input, m_MenuUseVguiInternal);
    if (armTrace) Game::logMsg("  f=%d <- aim published", s_menuFrames);

    g_pendHwnd = hwnd;

    // One click path. The laser's trigger arrives as an overlay ButtonDown AND,
    // often a frame apart, as the trigger action: both used to click, so one
    // pull could click twice. Now every source shares one 350 ms cooldown, and
    // nothing clicks unless the pointer is actually on the panel -- a click
    // with no aim used to land wherever the cursor was last.
    static ULONGLONG s_lastClickMs = 0;
    auto click = [&](const char *source) {
        if (!inputLive || aimX < 0 || nowMs - s_lastClickMs <= 350)
            return;
        s_lastClickMs = nowMs;
        g_pendMouseDown = true;
        g_pendMouseUp = true;
        if (m_MenuUseVguiInternal)
            SafeVguiClick(input);
        Game::logMsg("Menu click (%s) at (%d,%d)", source, aimX, aimY);
    };
    if (overlayDown)
        click("laser");

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
        static int s_moveTotal = 0, s_downTotal = 0;
        s_moveTotal += overlayMoves;
        s_downTotal += overlayDown ? 1 : 0;
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
            Game::logMsg("MenuHealth live=%d moves=%d down=%d | sel=%d(act=%d err=%d) atk=%d(act=%d err=%d) LEGACY=%d(%.2f) | interactive=%d vis=%d tip=%d ctrlPose=%d aim=(%d,%d)",
                         (int)inputLive, s_moveTotal, s_downTotal,
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
    if (pressed)
        click("trigger");
    const DWORD now = GetTickCount();

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

    // Swing to attack: with the slappers or the hunting knife out and the gun
    // in hand, chopping the right controller faster than SwingSpeed presses
    // attack for a moment. The cooldown makes one chop one hit; the trigger
    // still works too. The throwing knife (v_tknife) is thrown the same way,
    // in the direction of the flick.
    bool swingAttack = false;
    const std::string &held = m_Game ? m_Game->m_ActiveWeaponModel : std::string();
    const bool throwWeapon = held.find("/v_tknife.") != std::string::npos;
    const bool swingWeapon = held.find("slapper") != std::string::npos ||
                             held.find("/v_knife.") != std::string::npos || throwWeapon;
    if (m_TrackedWeapon && m_SwingMelee && m_System && swingWeapon)
    {
        static ULONGLONG s_swingUntil = 0, s_lastSwing = 0;
        const ULONGLONG now = GetTickCount64();
        const vr::TrackedDeviceIndex_t hand =
            m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        if (hand < vr::k_unMaxTrackedDeviceCount && m_Poses[hand].bPoseIsValid)
        {
            const vr::HmdVector3_t &v = m_Poses[hand].vVelocity;
            const float speed = sqrtf(v.v[0] * v.v[0] + v.v[1] * v.v[1] + v.v[2] * v.v[2]);
            if (speed > m_SwingSpeed && now - s_lastSwing > 450)
            {
                s_lastSwing = now;
                s_swingUntil = now + 150;
                if (throwWeapon && speed > 0.01f)
                {
                    // OpenVR velocity -> Source axes (as GetPoseData), then
                    // into the turned game frame like the hand positions.
                    Vector d(-v.v[2] / speed, -v.v[0] / speed, v.v[1] / speed);
                    if (fabsf(m_RotationOffset) > 0.01f)
                        d = VectorRotate(d, Vector(0.0f, 0.0f, 1.0f), m_RotationOffset);
                    m_ThrowDir = d;
                    // Past GE:S's release delay, and short of its refire.
                    m_ThrowAimUntil = now + 700;
                    static int s_thrown = 0;
                    if (s_thrown < 10)
                    {
                        Game::logMsg("Throw %.1f m/s dir=(%.2f,%.2f,%.2f) head=(%.2f,%.2f,%.2f)", speed,
                                     d.x, d.y, d.z, m_HmdForward.x, m_HmdForward.y, m_HmdForward.z);
                        ++s_thrown;
                    }
                }
                static int s_logged = 0;
                if (s_logged < 10)
                {
                    Game::logMsg("Swing %.1f m/s -> attack", speed);
                    ++s_logged;
                }
            }
        }
        swingAttack = now < s_swingUntil;
    }

    // With the gun in hand the view angles only swing to the barrel while
    // attacking, and a command queued here runs in the NEXT frame's usercmd
    // with whatever angles the last RenderView set. So on the first press the
    // attack waits one frame: this frame arms the barrel angles, the next
    // RenderView applies them, then +attack goes out and the first shot lands
    // on the dot. Held 150 ms after release so a burst's last shots do too.
    bool wantAttack = PressedDigitalAction(m_ActionPrimaryAttack) || swingAttack;
    if (m_TrackedWeapon && m_AimWithGun)
    {
        if (wantAttack)
            m_AttackAimUntil = (std::max)(m_AttackAimUntil, GetTickCount64() + 150);
        wantAttack = wantAttack && m_AttackAimApplied;
    }
    if (wantAttack)
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

    // GE:S aim mode is +aimmode (SHIFT on desktop), not +attack2 -- MOUSE2 is
    // +aimdetonate. Aim mode is what zooms the sniper scope.
    if (ScopeHeld())
        MoveCmd("+aimmode");
    else
        MoveCmd("-aimmode");

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
    const bool wantFullHud = PressedDigitalAction(m_ShowHUD) || PressedDigitalAction(m_Scoreboard) || m_GameHudMode == 2;
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
        // Includes height: sitting captures seat Z, standing increases
        // TrackedDevicePos.z (Source up) so the camera rises with you.
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

    // Eye separation in game units: your real IPD times VRScale. This is what
    // sets how big the world feels -- the brain reads distances against its
    // own IPD, so eye height feels like (game eye height / VRScale): 64 units
    // at VRScale 40 is 1.6 m.
    //
    // It used to be clamped to at least 1.8 per eye "so depth is obvious".
    // That pinned it at 1.8 whatever VRScale said (every HMD log line read
    // halfIpd=1.80), i.e. ~3.6 u for a 6.4 cm IPD = 56 units per metre, which
    // made eye height feel like ~1.15 m -- the "height feels too low" report
    // -- and made the World scale setting do nothing at all. The clamp is now
    // only a sanity range; use IPDScale to exaggerate depth deliberately.
    float halfIpd = (m_Ipd > 0.001f) ? (m_Ipd * m_IpdScale * m_VRScale) * 0.5f : 1.28f;
    if (halfIpd < 0.5f) halfIpd = 0.5f;
    if (halfIpd > 4.0f) halfIpd = 4.0f;

    Vector eyeOrigin = setup.origin + m_HmdPosLocalInWorld;
    eyeOrigin.z += m_HeightOffsetMeters * m_VRScale;

    left.origin = eyeOrigin + (m_HmdRight * (-halfIpd));
    right.origin = eyeOrigin + (m_HmdRight * (halfIpd));
    left.angles = hmdAng;
    right.angles = hmdAng;

    // Scope zoom. The engine narrows setup.fov to zoom; the eyes render at the
    // HMD's FOV and would ignore it. Apply the same tan-ratio to the eye FOV,
    // which magnifies the whole view by the scope's power. The ratio is immune
    // to Source widening the FOV for aspect, which scales both tans equally.
    // Base FOV is only relearned after the grip has been up for a moment, so a
    // zoom still animating back out is not mistaken for the unzoomed FOV.
    float eyeFov = m_Fov;
    const bool scopeHeld = ScopeHeld();
    // Zoomed in: the reticle shows even with Reticle off.
    dxvk::g_GESVR_ReticleForce = scopeHeld && m_ScopeBaseFov > 1.0f && setup.fov < m_ScopeBaseFov - 1.0f;
    if (!scopeHeld)
    {
        if (++m_ScopeReleasedFrames > 30 || m_ScopeBaseFov < 1.0f)
            m_ScopeBaseFov = setup.fov;
    }
    else
    {
        m_ScopeReleasedFrames = 0;
    }
    float scopeRatio = 1.0f;
    if (m_ScopeZoom && scopeHeld && setup.fov > 1.0f && m_ScopeBaseFov > setup.fov)
    {
        const float d2r = 3.14159265f / 180.0f;
        scopeRatio = tanf(setup.fov * 0.5f * d2r) / tanf(m_ScopeBaseFov * 0.5f * d2r);
        if (scopeRatio < 0.95f)
            eyeFov = 2.0f * atanf(tanf(m_Fov * 0.5f * d2r) * scopeRatio) / d2r;
    }
    static bool s_wasScoped = false;
    static float s_loggedRatio = 1.0f;
    if (scopeHeld != s_wasScoped || (scopeHeld && fabsf(scopeRatio - s_loggedRatio) > 0.1f))
    {
        Game::logMsg("Scope held=%d engineFov=%.1f baseFov=%.1f ratio=%.2f eyeFov=%.1f",
                     (int)scopeHeld, setup.fov, m_ScopeBaseFov, scopeRatio, eyeFov);
        s_wasScoped = scopeHeld;
        s_loggedRatio = scopeRatio;
    }

    left.fov = eyeFov;
    right.fov = eyeFov;
    // A 106-degree viewmodel FOV drags the weapon toward the centre of view and
    // makes it disagree with world-space muzzle effects. 0 keeps the world FOV.
    //
    // With the gun in hand it must be the eye's own FOV, zoom included: the
    // gun is placed in the world, and a pass drawn at a wider FOV than the eye
    // put it somewhere else -- off its own aim dot as soon as the scope zoomed.
    const float vmFov = m_TrackedWeapon ? eyeFov : (m_ViewmodelFov > 1.0f) ? m_ViewmodelFov : m_Fov;
    left.fovViewmodel = vmFov;
    right.fovViewmodel = vmFov;
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

        // The tracked gun needs the per-weapon table: it says where each
        // model's grip sits relative to its origin, which is what puts the
        // grip in your hand rather than the model's origin.
        PositionAngle pose = (m_PerWeaponOffsets || m_TrackedWeapon)
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


// Left grip = GE:S aim mode (scope). The Scope action is new, and SteamVR
// keeps using a player's cached bindings until they are reset, so in the
// 21:11 run it never fired once. Those bindings already have the left grip on
// TwoHand, which only means anything with motion controls on -- so in head-aim
// mode it counts as the scope too.
bool VR::ScopeHeld()
{
    if (PressedDigitalAction(m_ActionScope))
        return true;
    if (!m_MotionControls && PressedDigitalAction(m_ActionTwoHand))
        return true;
    static bool s_checked = false;
    if (!s_checked && m_Input && m_ActionScope != vr::k_ulInvalidActionHandle)
    {
        vr::InputDigitalActionData_t d{};
        if (m_Input->GetDigitalActionData(m_ActionScope, &d, sizeof(d), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None)
        {
            s_checked = true;
            Game::logMsg("Scope action %s", d.bActive ? "bound"
                         : "NOT bound (old cached SteamVR bindings) -- using the TwoHand grip instead");
        }
    }
    return false;
}

void VR::ResetPosition()
{
    m_CameraAnchor += m_SetupOrigin - m_HmdPosAbs;
    m_HeightOffset += m_SetupOrigin.z - m_HmdPosAbs.z;
    m_HaveSeatPose = false;
    Game::logMsg("ResetPosition: seat recapture on next pose (stand/sit recenter)");
}

void VR::CreateWristOverlays()
{
    if (!m_Overlay)
        return;

    struct { vr::VROverlayHandle_t *handle; const char *key; } overlays[] = {
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

// Source 2007 RecvProp is 60 bytes. This stub used to stop at m_Offset (48),
// so indexing m_pProps[i] walked off into garbage after the first prop and
// m_iHealth was never found -- every session, since the day it was written.
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
    int m_ElementStride;
    int m_nElements;
    const char *m_pParentArrayPropName;
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

static bool NameHas(const char *name, const char *part)
{
    for (const char *p = name; *p; ++p)
    {
        const char *a = p, *b = part;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
        if (!*b)
            return true;
    }
    return false;
}

// First of several candidate names that resolves in this table.
static int FindAnyNetvar(RecvTableStub *table, std::initializer_list<const char *> names)
{
    for (const char *n : names)
    {
        const int off = FindNetvarInTable(table, n);
        if (off >= 0)
            return off;
    }
    return -1;
}

// Every prop name in a table (top level), for discovering what a class
// networks. Only used on the few classes that might hold a round timer.
static void LogTableProps(const char *className, RecvTableStub *table)
{
    if (!table || !ReadablePtr(table, sizeof(RecvTableStub)) || table->m_nProps <= 0 || table->m_nProps > 512)
        return;
    std::string line;
    for (int i = 0; i < table->m_nProps; ++i)
    {
        RecvPropStub *prop = &table->m_pProps[i];
        if (!ReadablePtr(prop, sizeof(RecvPropStub)) || !ReadableCString(prop->m_pVarName))
            continue;
        line += prop->m_pVarName;
        line += ' ';
        if (line.size() > 900)
            break;
    }
    Game::logMsg("NETVARS %s: %s", className, line.c_str());
}

// Round timer, if GE:S networks one. Found by class name and prop name, since
// neither is known for certain; everything found is logged either way.
static ClientClassStub *g_timerClass = nullptr;
static int g_timerEndOff = -1, g_timerRemainOff = -1, g_timerPausedOff = -1, g_timerDisabledOff = -1;
static int g_timerEnabledOff = -1, g_timerStartedOff = -1;
// The local player's m_flSimulationTime: server time of its last update,
// which is "now" for comparing against the timer's end time.
static int g_playerSimTimeOff = -1;

void VR::ResolvePlayerNetvars()
{
    if (m_HealthNetvar >= 0)
        return;

    // Failure is cached too, with a few retries in case the class list is not
    // populated yet at map load: one early build re-walked every table every
    // frame, 598 full scans in 80 seconds.
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

    // One pass over every class: the local player's values, the weapon's clip
    // (the same offset in every weapon class, all derive from
    // CBaseCombatWeapon) and any round timer.
    int player = -1;
    for (ClientClassStub *cc = head; cc && ReadablePtr(cc, sizeof(ClientClassStub)); cc = cc->m_pNext)
    {
        if (!ReadableCString(cc->m_pNetworkName))
            break;
        const char *name = cc->m_pNetworkName;
        RecvTableStub *table = cc->m_pRecvTable;

        const bool isPlayer = strstr(name, "Player") != nullptr && strstr(name, "Resource") == nullptr;
        if (isPlayer)
        {
            const int health = FindNetvarInTable(table, "m_iHealth");
            const int armor = (health >= 0)
                ? FindAnyNetvar(table, { "m_ArmorValue", "m_iArmor", "m_Armor", "m_iArmorValue" }) : -1;
            // The class list is in registration order, so CBasePlayer can come
            // before GE:S's own player class -- and only the derived class
            // sends armour. Take the first match, but let a later class that
            // does network armour replace one that did not.
            if (health >= 0 && (player < 0 || (m_ArmorNetvar < 0 && armor >= 0)))
            {
                player = health;
                m_ArmorNetvar = armor;
                m_MaxHealthNetvar = FindAnyNetvar(table, { "m_iMaxHealth" });
                m_MaxArmorNetvar = FindAnyNetvar(table, { "m_iMaxArmor", "m_iMaxArmorValue" });
                m_ActiveWeaponNetvar = FindAnyNetvar(table, { "m_hActiveWeapon" });
                m_AmmoNetvar = FindAnyNetvar(table, { "m_iAmmo" });
                m_TickBaseNetvar = FindAnyNetvar(table, { "m_nTickBase" });
                g_playerSimTimeOff = FindAnyNetvar(table, { "m_flSimulationTime" });
                Game::logMsg("Player netvars on %s: health=%d armor=%d maxHealth=%d maxArmor=%d activeWeapon=%d ammo=%d tickBase=%d simTime=%d",
                             name, health, m_ArmorNetvar, m_MaxHealthNetvar, m_MaxArmorNetvar,
                             m_ActiveWeaponNetvar, m_AmmoNetvar, m_TickBaseNetvar, g_playerSimTimeOff);
            }
        }

        if (m_Clip1Netvar < 0)
        {
            const int clip = FindNetvarInTable(table, "m_iClip1");
            if (clip >= 0)
            {
                m_Clip1Netvar = clip;
                m_PrimaryAmmoTypeNetvar = FindAnyNetvar(table, { "m_iPrimaryAmmoType" });
                m_ViewModelIndexNetvar = FindAnyNetvar(table, { "m_iViewModelIndex" });
                Game::logMsg("Weapon netvars on %s: clip1=%d primaryAmmoType=%d viewModelIndex=%d",
                             name, clip, m_PrimaryAmmoTypeNetvar, m_ViewModelIndexNetvar);
            }
        }

        if (NameHas(name, "timer") || NameHas(name, "gamerules") || NameHas(name, "round"))
        {
            LogTableProps(name, table);
            if (NameHas(name, "timer"))
            {
                const int end = FindAnyNetvar(table, { "m_flTimerEndTime", "m_flEndTime", "m_flTimerEnd", "m_flRoundEndTime" });
                // A round timer beats a match timer; either beats nothing.
                if (end >= 0 && (!g_timerClass || NameHas(name, "round")))
                {
                    g_timerClass = cc;
                    g_timerEndOff = end;
                    // CGEGameTimer (GE:S) networks m_bEnabled m_bStarted m_bPaused
                    // m_flPauseTimeRemaining m_flLength m_flEndTime.
                    g_timerRemainOff = FindAnyNetvar(table, { "m_flPauseTimeRemaining", "m_flTimeRemaining", "m_flTimerRemaining", "m_flTimeLeft" });
                    g_timerPausedOff = FindAnyNetvar(table, { "m_bPaused", "m_bTimerPaused", "m_bIsPaused" });
                    g_timerDisabledOff = FindAnyNetvar(table, { "m_bIsDisabled", "m_bDisabled" });
                    g_timerEnabledOff = FindAnyNetvar(table, { "m_bEnabled", "m_bIsEnabled" });
                    g_timerStartedOff = FindAnyNetvar(table, { "m_bStarted", "m_bIsStarted" });
                    Game::logMsg("Round timer candidate %s: end=%d remaining=%d paused=%d disabled=%d enabled=%d",
                                 name, end, g_timerRemainOff, g_timerPausedOff, g_timerDisabledOff, g_timerEnabledOff);
                }
            }
        }
    }
    if (player >= 0 && !g_timerClass)
        Game::logMsg("No round timer class found (watch will show NO TIME LIMIT)");
    m_HealthNetvar = player;
}

static ClientClassStub *CallGetClientClass(void *networkable)
{
    __try
    {
        void **vt = *reinterpret_cast<void ***>(networkable);
        return reinterpret_cast<ClientClassStub *(__thiscall *)(void *)>(vt[2])(networkable);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

static bool ReadI32(const void *base, int off, int &out)
{
    if (!base || off < 0 || !ReadablePtr((const char *)base + off, sizeof(int)))
        return false;
    out = *reinterpret_cast<const int *>((const char *)base + off);
    return true;
}

static bool ReadF32(const void *base, int off, float &out)
{
    if (!base || off < 0 || !ReadablePtr((const char *)base + off, sizeof(float)))
        return false;
    out = *reinterpret_cast<const float *>((const char *)base + off);
    return true;
}

int VR::ReadRoundTimeLeft(void *player)
{
    if (!g_timerClass || !m_Game || !m_Game->m_ClientEntityList)
        return -1;
    IClientEntityList *list = m_Game->m_ClientEntityList;

    // GE:S has two ge_game_timer entities: the match timer and the round timer
    // (CGEMPRules creates them in that order, back to back, so the match timer
    // has the lower index). Its own HUD shows the round time while a round
    // timer runs, else the match time -- and only if round time is enabled
    // ("rounds are controlled some other way" otherwise). The watch does the
    // same. Rescanned every 2 s until found, and whenever a cached index stops
    // matching (map change).
    static int s_index[2] = { -1, -1 };   // match, round
    static ULONGLONG s_lastScan = 0;
    const ULONGLONG now = GetTickCount64();
    for (int t = 0; t < 2; ++t)
    {
        if (s_index[t] < 0)
            continue;
        void *net = list->GetClientNetworkable(s_index[t]);
        if (!net || CallGetClientClass(net) != g_timerClass)
            s_index[0] = s_index[1] = -1;
    }
    if (s_index[0] < 0 || s_index[1] < 0)
    {
        if (now - s_lastScan < 2000 && s_index[0] < 0)
            return -1;
        if (now - s_lastScan >= 2000)
        {
            s_lastScan = now;
            int found[2] = { -1, -1 }, n = 0;
            int highest = list->GetHighestEntityIndex();
            if (highest > 4096) highest = 4096;
            for (int i = 1; i <= highest && n < 2; ++i)
            {
                void *net = list->GetClientNetworkable(i);
                if (net && ReadablePtr(net, sizeof(void *)) && CallGetClientClass(net) == g_timerClass)
                    found[n++] = i;
            }
            if (found[0] != s_index[0] || found[1] != s_index[1])
                Game::logMsg("Game timers: match at %d, round at %d", found[0], found[1]);
            s_index[0] = found[0];
            s_index[1] = found[1];
        }
        if (s_index[0] < 0)
            return -1;
    }

    // "Now" in server time. The player's simulation time is exact; the tick
    // base times Source's default 15 ms tick is the fallback. (Measuring the
    // tick rate instead came out at 1/67 s and drifted a few seconds an hour.)
    float nowGame = 0.0f;
    int tick = 0;
    if (!ReadF32(player, g_playerSimTimeOff, nowGame) || nowGame <= 0.0f)
    {
        if (!ReadI32(player, m_TickBaseNetvar, tick) || tick <= 0)
            return -1;
        nowGame = tick * 0.015f;
    }

    // One timer's state, as CGEGameTimer::GetTimeRemaining works it out.
    struct TimerState { bool present, enabled, started, paused; float remaining; };
    auto readTimer = [&](int index) {
        TimerState st{ false, false, false, false, 0.0f };
        void *timer = index >= 0 ? list->GetClientEntity(index) : nullptr;
        if (!timer)
            return st;
        st.present = true;
        int flag = 0;
        st.enabled = !(g_timerEnabledOff >= 0 && ReadI32(timer, g_timerEnabledOff, flag) && !(flag & 0xFF));
        if (g_timerDisabledOff >= 0 && ReadI32(timer, g_timerDisabledOff, flag) && (flag & 0xFF))
            st.enabled = false;
        st.paused = g_timerPausedOff >= 0 && ReadI32(timer, g_timerPausedOff, flag) && (flag & 0xFF);
        float end = 0.0f;
        const bool haveEnd = ReadF32(timer, g_timerEndOff, end) && end > 0.0f;
        st.started = (g_timerStartedOff >= 0) ? (ReadI32(timer, g_timerStartedOff, flag) && (flag & 0xFF))
                                              : (haveEnd || st.paused);
        if (!st.started)
            return st;
        if (st.paused)
            ReadF32(timer, g_timerRemainOff, st.remaining);
        else if (haveEnd)
            st.remaining = end - nowGame;
        if (st.remaining < 0.0f)
            st.remaining = 0.0f;
        return st;
    };
    const TimerState match = readTimer(s_index[0]);
    const TimerState round = readTimer(s_index[1]);

    float remaining = -1.0f;
    const char *which = "none";
    if (!round.present)
    {
        // Only one timer: treat it as the match timer, as before.
        if (match.enabled && match.started)
        {
            remaining = match.remaining;
            which = "only";
        }
    }
    else if (round.started && round.enabled)
    {
        remaining = round.remaining;   // rounds always finish, even after match time runs out
        which = "round";
    }
    else if (match.started && !match.paused && round.enabled)
    {
        remaining = match.remaining;
        which = "match";
    }

    static ULONGLONG s_lastLog = 0;
    if (now - s_lastLog > 10000)
    {
        s_lastLog = now;
        Game::logMsg("Game timers: showing %s %.1f s (match %d/%d/%.1f, round %d/%d/%.1f enabled/started/left) now=%.2f (%s)",
                     which, remaining, (int)match.enabled, (int)match.started, match.remaining,
                     (int)round.enabled, (int)round.started, round.remaining, nowGame, tick ? "tickbase" : "simtime");
    }
    if (remaining <= 0.0f || remaining > 36000.0f)
        return -1;
    return (int)ceilf(remaining);
}

// The held weapon's viewmodel path, from the weapon's own m_iViewModelIndex.
// The old source -- whichever v_ model DrawModelExecute saw last -- named the
// shotgun "slappers": GE:S draws the slapper hands after the gun. 10 Hz.
void VR::RefreshActiveWeapon()
{
    static ULONGLONG s_last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_last < 100)
        return;
    s_last = now;

    ResolvePlayerNetvars();
    if (m_ViewModelIndexNetvar < 0 || m_ActiveWeaponNetvar < 0 || !m_Game || !m_Game->m_EngineClient ||
        !m_Game->m_ClientEntityList || !m_Game->m_ModelInfo)
        return;
    CBaseEntity *ent = m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
    int handle = -1, index = 0;
    if (!ent || !ReadI32(ent, m_ActiveWeaponNetvar, handle) || handle == -1)
        return;
    void *weapon = m_Game->m_ClientEntityList->GetClientEntityFromHandle(handle);
    if (!weapon || !ReadablePtr(weapon, sizeof(void *)) ||
        !ReadI32(weapon, m_ViewModelIndexNetvar, index) || index <= 0 || index > 16384)
        return;
    void *model = m_Game->m_ModelInfo->GetModel(index);
    if (!model)
        return;
    const char *name = m_Game->m_ModelInfo->GetModelName(model);
    if (!name || !ReadablePtr(name, 1))
        return;
    char buf[160];
    size_t n = 0;
    for (; n + 1 < sizeof(buf) && ReadablePtr(name + n, 1) && name[n]; ++n)
        buf[n] = name[n];
    buf[n] = 0;
    if (n < 4)
        return;
    if (m_Game->m_ActiveWeaponModel != buf)
    {
        Game::logMsg("Active weapon %s (viewmodel index %d)", buf, index);
        m_Game->m_ActiveWeaponModel = buf;
    }
    m_WeaponFromNetvar = true;
}

// --- Tracked gun aim -----------------------------------------------------
//
// IEngineTrace::TraceRay in Source 2007, checked against this engine.dll: it
// is vtable slot 4 (the SDK header here says 5, from a later engine), and its
// Ray_t has no m_pWorldAxisTransform -- slot 5, SetupLeafAndEntityListRay,
// reads m_IsSwept at +0x41. The header's Ray_t would put it at +0x45 and its
// filter calls C_BasePlayer methods by L4D2 slot numbers. So all three are
// our own here, in the 2007 shape.
struct alignas(16) TraceVec4 { float x, y, z, w; };
struct TraceRay2007
{
    TraceVec4 start, delta, startOffset, extents;
    bool isRay, isSwept;
};
static_assert(offsetof(TraceRay2007, isRay) == 0x40, "Source 2007 Ray_t layout");

// ITraceFilter: ShouldHitEntity then GetTraceType, no destructor.
class SkipOneEntityFilter
{
public:
    explicit SkipOneEntityFilter(const void *skip) : m_skip(skip) {}
    virtual bool ShouldHitEntity(void *entity, int /*contentsMask*/) { return entity != m_skip; }
    virtual int GetTraceType() const { return 0; }   // TRACE_EVERYTHING
private:
    const void *m_skip;
};

typedef void(__thiscall *tTraceRay2007)(void *self, const TraceRay2007 *ray, unsigned int mask, void *filter, void *trace);

static tTraceRay2007 TraceRayFunction(void *engineTrace)
{
    static int s_state = 0;   // 0 unchecked, 1 verified, -1 unusable
    static tTraceRay2007 s_fn = nullptr;
    if (s_state == 0 && engineTrace)
    {
        s_state = -1;
        void **vt = *reinterpret_cast<void ***>(engineTrace);
        const unsigned char *p = static_cast<const unsigned char *>(vt[4]);
        // push ebp / mov ebp,esp / and esp,-16 / mov eax,10D4h (its big stack frame)
        static const unsigned char kPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0xB8, 0xD4, 0x10, 0x00, 0x00 };
        if (memcmp(p, kPrologue, sizeof(kPrologue)) == 0)
        {
            s_fn = reinterpret_cast<tTraceRay2007>(vt[4]);
            s_state = 1;
        }
        Game::logMsg("IEngineTrace::TraceRay %s", s_state == 1 ? "verified at vtable[4]"
                                                               : "NOT recognised -- gun aim uses the barrel direction only");
    }
    return s_state == 1 ? s_fn : nullptr;
}

// Where the barrel points, and the aim that sends a shot there. Shots leave
// from the player's eye, not the gun, so aiming the VIEW down the barrel would
// miss by the head-to-hand distance: instead trace from the muzzle along the
// barrel and aim the eye at what it hits. Then each eye gets the aim dot.
//
// Face aim traces too: from the game's eye along the view, which is where its
// shots go. The reticle used to sit at the plain centre of each eye -- the same
// spot in both, which reads as infinitely far away -- so it seemed to pass
// through anything nearer. Drawn at the hit point in each eye it sits on the
// surface, as in free aim.
void VR::UpdateGunAim(const CViewSetup &left, const CViewSetup &right)
{
    dxvk::g_GESVR_ReticleUseAim = false;
    if (!m_Game || !m_Game->m_EngineClient)
        return;
    const bool freeAim = m_TrackedWeapon;

    Vector start, f;
    const bool gun = freeAim && GESVR_MuzzleWorld(start, f);
    // A knife flick: aim along the flick until the knife has left.
    const bool throwing = freeAim && !gun && GetTickCount64() < m_ThrowAimUntil;
    if (!freeAim)
    {
        start = m_SetupOrigin;
        Vector r, u;
        QAngle::AngleVectors(left.angles, &f, &r, &u);
    }
    else if (!gun)
    {
        start = GetRightControllerAbsPos();
        Vector r, u;
        QAngle::AngleVectors(GetRecommendedViewmodelAbsAngle(), &f, &r, &u);
        if (throwing)
            f = m_ThrowDir;
    }
    const float kRange = 8192.0f;
    float traceFraction = -1.0f;
    Vector hit(start.x + f.x * kRange, start.y + f.y * kRange, start.z + f.z * kRange);

    tTraceRay2007 trace = TraceRayFunction(m_Game->m_EngineTrace);
    if (trace)
    {
        TraceRay2007 ray{};
        ray.start = { start.x, start.y, start.z, 0.0f };
        ray.delta = { hit.x - start.x, hit.y - start.y, hit.z - start.z, 0.0f };
        ray.isRay = true;
        ray.isSwept = true;
        alignas(16) unsigned char result[256] = {};   // CGameTrace is 84 bytes in 2007
        SkipOneEntityFilter filter(m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer()));
        trace(m_Game->m_EngineTrace, &ray, MASK_SHOT, &filter, result);
        const float fraction = *reinterpret_cast<const float *>(result + 44);
        traceFraction = fraction;
        if (fraction >= 0.0f && fraction <= 1.0f)
            hit = Vector(start.x + ray.delta.x * fraction, start.y + ray.delta.y * fraction, start.z + ray.delta.z * fraction);
    }
    m_GunAimPoint = hit;

    // Only while attacking. The game walks along the view angles too, so
    // holding them on the barrel every frame made the stick walk you wherever
    // the gun pointed (the 23:12 run). Otherwise they stay on the head, as
    // ApplyHeadAndIpd just set them, and walking, use and the flashlight
    // follow your head.
    m_AttackAimApplied = false;
    if (freeAim && m_AimWithGun && (GetTickCount64() < m_AttackAimUntil || throwing))
    {
        const Vector d(hit.x - m_SetupOrigin.x, hit.y - m_SetupOrigin.y, hit.z - m_SetupOrigin.z);
        const float flat = sqrtf(d.x * d.x + d.y * d.y);
        QAngle aim(-atan2f(d.z, flat) * 57.2957795f, atan2f(d.y, d.x) * 57.2957795f, 0.0f);
        if (aim.x > 89.0f) aim.x = 89.0f;
        if (aim.x < -89.0f) aim.x = -89.0f;
        m_Game->m_EngineClient->SetViewAngles(aim);
        m_AttackAimApplied = true;
    }

    // The dot, in each eye, using that eye's own projection (fov is horizontal,
    // aspect sets the vertical) so it stays right when the scope zooms.
    dxvk::g_GESVR_ReticleUseAim = true;
    const CViewSetup *eyes[2] = { &left, &right };
    // Why each eye's dot is hidden: 0 shown, 1 no gun drawn lately (melee,
    // throwables, or the gun missed the tracked draw), 2 hit behind the eye,
    // 3 outside the view.
    int why[2] = { 0, 0 };
    float nxs[2] = { 0.0f, 0.0f }, nys[2] = { 0.0f, 0.0f };
    for (int e = 0; e < 2; ++e)
    {
        dxvk::g_GESVR_ReticleAimValid[e] = false;
        if (freeAim && !gun)
        {
            why[e] = 1;
            continue;   // free aim with melee and throwables: no dot
        }
        Vector ef, er, eu;
        QAngle::AngleVectors(eyes[e]->angles, &ef, &er, &eu);
        const Vector v(hit.x - eyes[e]->origin.x, hit.y - eyes[e]->origin.y, hit.z - eyes[e]->origin.z);
        const float zc = v.x * ef.x + v.y * ef.y + v.z * ef.z;
        if (zc < 1.0f)
        {
            why[e] = 2;
            continue;
        }
        const float t = tanf(eyes[e]->fov * 0.5f * 3.14159265f / 180.0f);
        const float aspect = eyes[e]->m_flAspectRatio > 0.1f ? eyes[e]->m_flAspectRatio : m_Aspect;
        const float nx = (v.x * er.x + v.y * er.y + v.z * er.z) / (zc * t);
        const float ny = (v.x * eu.x + v.y * eu.y + v.z * eu.z) * aspect / (zc * t);
        nxs[e] = nx;
        nys[e] = ny;
        if (fabsf(nx) > 1.2f || fabsf(ny) > 1.2f)
        {
            why[e] = 3;
            continue;
        }
        dxvk::g_GESVR_ReticleAimU[e] = 0.5f + 0.5f * nx;
        dxvk::g_GESVR_ReticleAimV[e] = 0.5f - 0.5f * ny;
        dxvk::g_GESVR_ReticleAimValid[e] = true;
    }

    // Diagnostics for "the dot goes wonky and disappears until I shoot":
    // every change in whether the dot shows (and why not), plus a sample every
    // two seconds. Capped so a long session cannot flood the log.
    {
        const float dist = sqrtf((hit.x - start.x) * (hit.x - start.x) + (hit.y - start.y) * (hit.y - start.y) +
                                 (hit.z - start.z) * (hit.z - start.z));
        const QAngle gunAng = GetRecommendedViewmodelAbsAngle();
        const ULONGLONG now = GetTickCount64();
        static int s_lastWhy[2] = { -1, -1 };
        static int s_edges = 0, s_samples = 0;
        static ULONGLONG s_nextSample = 0;
        const bool edge = why[0] != s_lastWhy[0] || why[1] != s_lastWhy[1];
        const bool sample = now >= s_nextSample;
        if ((edge && s_edges < 150) || (sample && s_samples < 90))
        {
            if (edge) ++s_edges; else ++s_samples;
            if (sample) s_nextSample = now + 2000;
            Game::logMsg("Gun aim%s: gun=%d why=%d/%d frac=%.3f dist=%.0f start=(%.1f,%.1f,%.1f) dir=(%.2f,%.2f,%.2f) "
                         "gunAng=(%.0f,%.0f,%.0f) head=(%.0f,%.0f) L=(%.2f,%.2f) R=(%.2f,%.2f) fov=%.0f attack=%d",
                         edge ? " CHANGE" : "", (int)gun, why[0], why[1], traceFraction, dist,
                         start.x, start.y, start.z, f.x, f.y, f.z, gunAng.x, gunAng.y, gunAng.z,
                         left.angles.x, left.angles.y, nxs[0], nys[0], nxs[1], nys[1], left.fov,
                         (int)m_AttackAimApplied);
        }
        s_lastWhy[0] = why[0];
        s_lastWhy[1] = why[1];
    }
}

// GE:S's own crosshair (CGEViewEffects::DrawCrosshair) is the classic red
// sprite, drawn 1200 units down the centre of the CURRENT VIEW while in aim
// mode (holding the aim button -- the left grip here). In VR that pins it to
// your head, so with the gun in hand it is a second reticle pointing the wrong
// way (the scoped-weapon report). Hide its material while free aim is on:
// CMaterial::DrawMesh skips any material with MATERIAL_VAR_NO_DRAW set, and
// the flag lives on the material, so this costs nothing per frame.
//
// IMaterialSystem::FindMaterial is called by raw slot: this engine's (2007)
// is slot 70, verified by disassembly of materialsystem.dll -- the function
// that prints 'material "%s" not found.' and returns 16 bytes of arguments --
// and by its prologue here before the first call. The header in sdk/ is
// L4D2's and puts it elsewhere. The IMaterial slots the header gives
// (GetName 0, SetMaterialVarFlag 29, GetMaterialVarFlag 30, IsErrorMaterial
// 42) match this engine's CMaterial and CMaterial_QueueFriendly vtables.
void VR::UpdateGameCrosshair()
{
    static ULONGLONG s_next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < s_next || !m_Game || !m_Game->m_MaterialSystem)
        return;
    s_next = now + 1000;

    typedef IMaterial *(__thiscall *tFindMaterial)(void *self, const char *name, const char *group,
                                                    bool complain, const char *complainPrefix);
    static int s_state = 0;   // 0 unchecked, 1 verified, -1 not this engine's FindMaterial
    static tFindMaterial s_find = nullptr;
    if (s_state == 0)
    {
        s_state = -1;
        void **vt = *reinterpret_cast<void ***>(m_Game->m_MaterialSystem);
        static const unsigned char kPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24, 0x8B, 0x45,
                                                   0x08, 0x53, 0x56, 0x8B, 0xD9, 0x57, 0x89, 0x5D };
        if (vt && ReadablePtr(vt[70], sizeof(kPrologue)) && memcmp(vt[70], kPrologue, sizeof(kPrologue)) == 0)
        {
            s_find = reinterpret_cast<tFindMaterial>(vt[70]);
            s_state = 1;
        }
        Game::logMsg("IMaterialSystem::FindMaterial %s", s_state == 1 ? "verified at vtable[70]"
                                                                     : "NOT recognised -- GE:S's crosshair stays visible");
    }
    if (s_state != 1)
        return;

    IMaterial *mat = s_find(m_Game->m_MaterialSystem, "sprites/crosshair", "VGUI textures", false, nullptr);
    if (!mat || mat->IsErrorMaterial())
        return;
    const bool hide = m_TrackedWeapon;
    if (mat->GetMaterialVarFlag(MATERIAL_VAR_NO_DRAW) != hide)
    {
        mat->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, hide);
        Game::logMsg("GE:S crosshair (sprites/crosshair) %s", hide ? "hidden: free aim is on" : "shown");
    }
}

// Numpad tuning of where the held weapon sits in the hand (WeaponTuning and
// free aim on; off by default, the positions ship in weapons.cpp's table)
// in play). Move mode: 8/2 forward/back, 4/6 left/right, 9/3 up/down. Rotate
// mode: the same keys pitch, yaw, roll. 5 switches mode, +/- change the step,
// 0 saves every weapon to VR/weapons.txt, . resets the held one. Each weapon
// is its own entry; the head-locked toast shows where you are.
void VR::ProcessTuneKeys()
{
    static unsigned s_seen[13] = {};
    static bool s_init = false;
    int presses[13];
    bool any = false;
    for (int k = 0; k < 13; ++k)
    {
        const unsigned now = MenuInput::g_tunePress[k].load();
        presses[k] = s_init ? (int)(now - s_seen[k]) : 0;
        s_seen[k] = now;
        any = any || presses[k] != 0;
    }
    s_init = true;
    if (!any || !m_WeaponTuning || !m_TrackedWeapon || !m_Game || m_Game->m_ActiveWeaponModel.empty())
        return;

    static bool s_rotate = false;
    static int s_step = 2;
    static bool s_unsaved = false;
    static const float kMove[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f };   // game units
    static const float kTurn[] = { 0.5f, 1.0f, 2.5f, 5.0f, 10.0f };   // degrees

    const std::string model = m_Game->m_ActiveWeaponModel;
    const std::string key = Weapons::Key(model);
    std::wstring status;

    if (presses[5] & 1)
        s_rotate = !s_rotate;
    s_step += presses[10] - presses[11];
    if (s_step < 0) s_step = 0;
    if (s_step > 4) s_step = 4;

    PositionAngle p = Weapons::GetOffset(model);
    const int fwd = presses[8] - presses[2], side = presses[6] - presses[4], vert = presses[9] - presses[3];
    if (fwd || side || vert)
    {
        // The stored offset is the controller's position relative to the
        // model's anchor, so moving the weapon forward shrinks it.
        if (!s_rotate)
        {
            p.position.x -= fwd * kMove[s_step];
            p.position.y -= side * kMove[s_step];
            p.position.z -= vert * kMove[s_step];
        }
        else
        {
            p.angle.x += fwd * kTurn[s_step];
            p.angle.y += side * kTurn[s_step];
            p.angle.z += vert * kTurn[s_step];
        }
        Weapons::SetOverride(key, p);
        s_unsaved = true;
    }
    if (presses[12])
    {
        Weapons::ClearOverride(key);
        p = Weapons::GetOffset(model);
        s_unsaved = true;
        status = L"  RESET";
    }
    if (presses[0])
    {
        char path[MAX_STR_LEN];
        MakeVRPath(path, sizeof(path), "weapons.txt");
        const bool ok = Weapons::SaveOverrides(path);
        s_unsaved = s_unsaved && !ok;
        status = ok ? L"  SAVED" : L"  SAVE FAILED";
        Game::logMsg("Weapon positions %s to %s", ok ? "saved" : "NOT saved", path);
    }

    wchar_t title[160], detail[200];
    swprintf(title, 160, L"%ls  %ls  step %g%ls%ls", s_rotate ? L"ROTATE" : L"MOVE",
             VRWatch::WeaponName(model).c_str(), s_rotate ? kTurn[s_step] : kMove[s_step],
             s_unsaved ? L"  (unsaved)" : L"", status.c_str());
    swprintf(detail, 200, L"pos %.2f %.2f %.2f   ang %.1f %.1f %.1f     5 mode  +/- step  0 save",
             p.position.x, p.position.y, p.position.z, p.angle.x, p.angle.y, p.angle.z);
    VRToast::Show(title, detail);
}

void VR::ReadWatchStats(WatchStats &s)
{
    s = WatchStats{};
    if (!m_Game)
        return;
    RefreshActiveWeapon();
    s.weaponModel = m_Game->m_ActiveWeaponModel;

    ResolvePlayerNetvars();
    if (m_HealthNetvar < 0 || !m_Game->m_EngineClient)
        return;
    CBaseEntity *ent = m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
    if (!ent)
        return;

    int v = 0;
    if (ReadI32(ent, m_HealthNetvar, v) && v >= 0 && v <= 1000) s.health = v;
    if (ReadI32(ent, m_ArmorNetvar, v) && v >= 0 && v <= 1000) s.armor = v;
    if (ReadI32(ent, m_MaxHealthNetvar, v) && v > 0 && v <= 1000) s.maxHealth = v;
    if (ReadI32(ent, m_MaxArmorNetvar, v) && v > 0 && v <= 1000) s.maxArmor = v;

    int handle = -1;
    if (m_Game->m_ClientEntityList && ReadI32(ent, m_ActiveWeaponNetvar, handle) && handle != -1)
    {
        void *weapon = m_Game->m_ClientEntityList->GetClientEntityFromHandle(handle);
        if (weapon && ReadablePtr(weapon, sizeof(void *)))
        {
            if (ReadI32(weapon, m_Clip1Netvar, v) && v >= 0 && v < 1000) s.clip = v;
            int type = -1;
            if (m_AmmoNetvar >= 0 && ReadI32(weapon, m_PrimaryAmmoTypeNetvar, type) && type >= 0 && type < 32 &&
                ReadI32(ent, m_AmmoNetvar + type * 4, v) && v >= 0 && v < 10000)
                s.reserve = v;
        }
    }

    s.timeLeft = ReadRoundTimeLeft(ent);
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

// Run user-supplied console commands once the map is up.
//
// The menu is sharp until the world loads and sharp again on the very last
// frame before the game stops rendering -- both are moments when no 3D scene
// exists. Source's post-processing only runs when there IS one, so the blur is
// in the frame we capture, not in how the overlay displays it. Which effect is
// responsible is a question about GE:S's renderer that only testing answers, so
// this makes the list a config key rather than a rebuild.
// Picture settings the headset benefits from, set on GE:S itself (and so
// saved in its own settings). Filtering is a sampler state -- no texture
// reload, safe mid-map. Texture DETAIL is deliberately not touched:
// mat_picmip -1 hung the NVIDIA driver loading a map in this 32-bit process.
void VR::ApplyGraphicsCvars()
{
    m_GraphicsDirty = false;
    if (!m_Game)
        return;
    if (m_TextureFiltering > 0)
    {
        char cmd[48];
        snprintf(cmd, sizeof(cmd), "mat_forceaniso %d", m_TextureFiltering);
        m_Game->ClientCmd_Unrestricted(cmd);
        m_Game->ClientCmd_Unrestricted("mat_trilinear 1");
    }
    m_Game->ClientCmd_Unrestricted(m_Bloom ? "mat_disable_bloom 0" : "mat_disable_bloom 1");
    Game::logMsg("Graphics: texture filtering %s, bloom %s",
                 m_TextureFiltering > 0 ? (std::to_string(m_TextureFiltering) + "x anisotropic + trilinear").c_str()
                                        : "left to the game",
                 m_Bloom ? "on" : "off");
}

void VR::ApplyExtraCvars()
{
    if (m_ExtraCvarsDone)
        return;

    // Let the map finish coming up; cvars set mid-load can be overwritten.
    const unsigned now = (unsigned)GetTickCount();
    if (m_InMapSinceMs == 0)
    {
        m_InMapSinceMs = now;
        return;
    }
    if ((now - m_InMapSinceMs) < 1500)
        return;

    m_ExtraCvarsDone = true;
    if (!m_Game)
        return;

    // VR settings for GE:S itself. Both are saved in GE:S's own config.
    if (m_WeaponFastSwitch)
    {
        m_Game->ClientCmd_Unrestricted("hud_fastswitch 1");
        Game::logMsg("VR cvars: hud_fastswitch 1 (weapons switch as you press)");
    }
    if (!m_DeathCamFirstPerson)
    {
        m_Game->ClientCmd_Unrestricted("ge_fp_ragdoll 0");
        Game::logMsg("VR cvars: ge_fp_ragdoll 0 (death camera off the ragdoll's head)");
    }
    ApplyGraphicsCvars();

    size_t start = 0;
    while (start < m_ExtraCvars.size())
    {
        size_t end = m_ExtraCvars.find(';', start);
        if (end == std::string::npos)
            end = m_ExtraCvars.size();
        std::string cmd = m_ExtraCvars.substr(start, end - start);
        size_t b = cmd.find_first_not_of(" \t");
        size_t e = cmd.find_last_not_of(" \t");
        if (b != std::string::npos && e != std::string::npos)
        {
            cmd = cmd.substr(b, e - b + 1);
            if (!cmd.empty())
            {
                m_Game->ClientCmd_Unrestricted(cmd.c_str());
                Game::logMsg("ExtraCvars: %s", cmd.c_str());
            }
        }
        start = end + 1;
    }
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
    const bool show = (recentlyHurt || lowHealth) && !m_LookingAtWrist && m_GameHudMode == 1 && health != 0;

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
    m_InGameMenuPanel = CfgBool(userConfig, "InGameMenuPanel", m_InGameMenuPanel);
    m_AlwaysCaptureOverlay = CfgBool(userConfig, "AlwaysCaptureOverlay", m_AlwaysCaptureOverlay);
    {
        auto it = userConfig.find("ExtraCvars");
        if (it != userConfig.end())
            m_ExtraCvars = it->second;
    }
    m_InGameMenuDistance = CfgFloat(userConfig, "InGameMenuDistance", m_InGameMenuDistance);
    m_MenuScaleWithRes = CfgBool(userConfig, "MenuScaleWithRes", m_MenuScaleWithRes);
    m_UseEyeRenderTargets = CfgBool(userConfig, "EyeRenderTargets", m_UseEyeRenderTargets);
    m_EyeHudPass = CfgBool(userConfig, "EyeHudPass", m_EyeHudPass);
    m_MonoEye = CfgBool(userConfig, "MonoEye", m_MonoEye);
    m_EyeCropLegacyV = CfgBool(userConfig, "EyeCropLegacyV", m_EyeCropLegacyV);
    dxvk::g_GESVR_SwapEyeSurfaces = CfgBool(userConfig, "SwapEyeSurfaces", false);
    m_MonoEyeSource = (int)CfgFloat(userConfig, "MonoEyeSource", (float)m_MonoEyeSource);
    m_SharedEyeTarget = CfgBool(userConfig, "SharedEyeTarget", m_SharedEyeTarget);
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
    m_ViewmodelFov = CfgFloat(userConfig, "ViewmodelFov", m_ViewmodelFov);
    m_ViewmodelUserOffset = CfgVec(userConfig, "ViewmodelOffset", m_ViewmodelUserOffset);
    m_ViewmodelAngleOffset = CfgVec(userConfig, "ViewmodelAngleOffset", m_ViewmodelAngleOffset);
    m_PerWeaponOffsets = CfgBool(userConfig, "PerWeaponOffsets", m_PerWeaponOffsets);
    m_TwoHandedGrip = CfgBool(userConfig, "TwoHandedGrip", m_TwoHandedGrip);
    m_TwoHandedNeedsGrip = CfgBool(userConfig, "TwoHandedNeedsGrip", m_TwoHandedNeedsGrip);
    m_ScopeZoom = CfgBool(userConfig, "ScopeZoom", m_ScopeZoom);
    m_TrackedWeapon = CfgBool(userConfig, "TrackedWeapon", m_TrackedWeapon);
    m_FixViewmodelAspect = CfgBool(userConfig, "FixViewmodelAspect", m_FixViewmodelAspect);
    m_SwingMelee = CfgBool(userConfig, "SwingMelee", m_SwingMelee);
    m_AimWithGun = CfgBool(userConfig, "AimWithGun", m_AimWithGun);
    m_SwingSpeed = CfgFloat(userConfig, "SwingSpeed", m_SwingSpeed);
    m_MeleeHideArm = CfgBool(userConfig, "MeleeHideArm", m_MeleeHideArm);
    m_MeleeAngleOffset = CfgVec(userConfig, "MeleeAngleOffset", m_MeleeAngleOffset);
    m_HeightOffsetMeters = CfgFloat(userConfig, "HeightOffsetMeters", m_HeightOffsetMeters);
    m_MenuUseWin32 = CfgBool(userConfig, "MenuInputWin32", m_MenuUseWin32);
    m_DrawMenuCursor = CfgBool(userConfig, "DrawMenuCursor", m_DrawMenuCursor);
    dxvk::g_GESVR_DrawReticle = CfgBool(userConfig, "VRReticle", true);
    dxvk::g_GESVR_ReticleScale = CfgFloat(userConfig, "VRReticleSize", 0.0007f);
    dxvk::g_GESVR_ReticleAspect = CfgFloat(userConfig, "VRReticleAspect", 0.0f);
    {
        // Same shape as the MenuAimSource key above: this config has no
        // string helper, only CfgBool/CfgFloat/CfgVec.
        auto it = userConfig.find("VRReticleStyle");
        if (it != userConfig.end())
        {
            const std::string &v = it->second;
            dxvk::g_GESVR_ReticleStyle =
                (v.find("classic") != std::string::npos) ? 4 :
                (v.find("cross") != std::string::npos) ? 0 :
                (v.find("ringdot") != std::string::npos || v.find("ring+dot") != std::string::npos) ? 3 :
                (v.find("ring") != std::string::npos) ? 2 : 1;
        }
    }
    {
        auto it = userConfig.find("VRReticleColor");
        if (it != userConfig.end())
        {
            static const char *names[] = { "yellow", "white", "green", "red", "cyan" };
            for (int i = 0; i < 5; ++i)
                if (it->second.find(names[i]) != std::string::npos)
                    dxvk::g_GESVR_ReticleColor = i;
        }
    }
    dxvk::g_GESVR_ForceMenuOpaque = CfgBool(userConfig, "ForceMenuOpaque", true);

    m_MenuDriveCursor = CfgBool(userConfig, "MenuDriveCursor", m_MenuDriveCursor);
    m_MenuKeepaliveMs = (int)CfgFloat(userConfig, "MenuKeepaliveMs", (float)m_MenuKeepaliveMs);
    m_ShowMirrorWindow = CfgBool(userConfig, "ShowMirrorWindow", m_ShowMirrorWindow);
    m_TheaterHideThrottleMs = (int)CfgFloat(userConfig, "TheaterHideThrottleMs", (float)m_TheaterHideThrottleMs);
    g_menuDriveCursor = m_MenuDriveCursor;
    MenuInput::g_enabled.store(m_MenuDriveCursor);
    MenuInput::g_useSetCursorPos.store(CfgBool(userConfig, "MenuUseSetCursorPos", true));
    MenuInput::g_fitWindow.store(CfgBool(userConfig, "KeepWindowOnPrimaryMonitor", true));
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
    m_GameHudMode = m_HudAlwaysVisible ? 2 : 1;
    {
        auto it = userConfig.find("GameHUD");
        if (it != userConfig.end())
        {
            const std::string &v = it->second;
            m_GameHudMode = (v.find("always") != std::string::npos) ? 2 :
                            (v.find("hurt") != std::string::npos) ? 1 :
                            (v.find("off") != std::string::npos) ? 0 : m_GameHudMode;
        }
    }
    m_FaceAimGunSpread = CfgFloat(userConfig, "FaceAimGunSpread", m_FaceAimGunSpread);
    m_BloodCurtainScale = CfgFloat(userConfig, "BloodCurtainScale", m_BloodCurtainScale);
    m_WeaponFastSwitch = CfgBool(userConfig, "WeaponFastSwitch", m_WeaponFastSwitch);
    m_WeaponTuning = CfgBool(userConfig, "WeaponTuning", m_WeaponTuning);
    m_TextureFiltering = (int)CfgFloat(userConfig, "TextureFiltering", (float)m_TextureFiltering);
    if (m_TextureFiltering < 0) m_TextureFiltering = 0;
    if (m_TextureFiltering > 16) m_TextureFiltering = 16;
    m_Bloom = CfgBool(userConfig, "Bloom", m_Bloom);
    m_DeathCamFirstPerson = CfgBool(userConfig, "DeathCamFirstPerson", m_DeathCamFirstPerson);

    m_ShowWristHUD = CfgBool(userConfig, "ShowWristHUD", m_ShowWristHUD);
    m_WristLookMaxDistance = CfgFloat(userConfig, "WristLookMaxDistance", m_WristLookMaxDistance);
    m_WristLookMinDot = CfgFloat(userConfig, "WristLookMinDot", m_WristLookMinDot);
    m_WatchAlwaysVisible = CfgBool(userConfig, "WatchAlwaysVisible", m_WatchAlwaysVisible);
    m_WatchWidth = CfgFloat(userConfig, "WatchWidth", m_WatchWidth);
    m_WatchOffset = CfgVec(userConfig, "WatchOffset", m_WatchOffset);
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