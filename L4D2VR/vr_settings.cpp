#include "vr_settings.h"
#include "vr.h"
#include <Windows.h>
#include "game.h"
#include "vr_canvas.h"
#include "vr_flipoverlay.h"
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <vector>
#include <string>
#include <fstream>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dxvk { extern bool g_GESVR_DrawReticle; extern float g_GESVR_ReticleScale;
                 extern int g_GESVR_ReticleStyle; extern int g_GESVR_ReticleColor; }
void GESVR_RequestMenuReplace();

namespace VRSettings
{
// ---------------------------------------------------------------------------
// Opening from the GE:S main menu.
//
// GameMenu.res runs "engine echo gesvr_vrsettings". Echo goes through tier0's
// spew output, and tier0 exports the getter and setter for that function, so
// chaining in costs two exports and no engine structures at all. Our marker
// is swallowed; everything else goes on to the previous function untouched.
// ---------------------------------------------------------------------------
typedef int (*SpewOutputFunc_t)(int spewType, const char *msg);
static SpewOutputFunc_t g_prevSpew = nullptr;
static std::atomic<bool> g_openRequested{ false };

static thread_local int t_spewDepth = 0;

static int GesvrSpew(int spewType, const char *msg)
{
    if (msg && strstr(msg, "gesvr_vrsettings"))
    {
        g_openRequested.store(true);
        return 1; // SPEW_CONTINUE. 0 would be SPEW_DEBUGGER: a breakpoint.
    }
    // InstallMenuHook re-chains periodically. If something else has wrapped
    // the spew function after us, it chains back here: pass each message on
    // once, never round the loop again.
    SpewOutputFunc_t prev = g_prevSpew;
    if (!prev || prev == GesvrSpew || t_spewDepth > 0)
        return 1;
    ++t_spewDepth;
    const int r = prev(spewType, msg);
    --t_spewDepth;
    return r;
}

void InstallMenuHook()
{
    HMODULE tier0 = GetModuleHandleA("tier0.dll");
    if (!tier0)
        return;
    auto getFn = (SpewOutputFunc_t (*)())GetProcAddress(tier0, "GetSpewOutputFunc");
    auto setFn = (void (*)(SpewOutputFunc_t))GetProcAddress(tier0, "SpewOutputFunc");
    if (!getFn || !setFn)
    {
        static bool s_logged = false;
        if (!s_logged) { s_logged = true; Game::logMsg("VRSettings: tier0 spew exports missing; menu entry inactive (left X still opens)"); }
        return;
    }
    SpewOutputFunc_t cur = getFn();
    if (cur == GesvrSpew)
        return;
    g_prevSpew = cur;
    setFn(GesvrSpew);
    Game::logMsg("VRSettings: menu hook installed in tier0 spew (previous=%p)", (void *)cur);
}

bool ConsumeOpenRequest()
{
    return g_openRequested.exchange(false);
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------
enum class Kind { Choice, Toggle, Button };

struct Item
{
    std::wstring label;
    std::wstring hint;
    Kind kind = Kind::Choice;
    std::vector<std::wstring> names;   // one per choice
    bool wrap = false;                 // enums wrap round; numbers stop at the ends
    std::function<int()> get;          // current choice index
    std::function<void(int)> set;      // apply + save (render thread)
    std::function<void()> action;      // Button only
};

struct Tab
{
    std::wstring name;
    std::vector<Item> items;
};

static VR *g_vr = nullptr;
static std::vector<Tab> g_tabs;

// Panel size in pixels. Layout below is in these units.
static const int W = 1280;
static const int H = 900;
static const int SS = 2;       // drawn at 2x and filtered down, for smooth text and edges
static const float kWidthMeters = 1.2f;
static const float kDistanceMeters = 1.25f;

// Shared between the render thread (input) and the draw thread.
static std::mutex g_mtx;
static std::condition_variable g_cv;
static bool g_dirty = false;
static std::vector<std::pair<std::string, std::string>> g_pendingSaves;
static int g_tab = 0;
static int g_hover = -1;

static std::atomic<bool> g_open{ false };
static bool g_placed = false;
static bool g_legacyPrev = true;   // true: a trigger still held from opening is not a click
// Written by the draw thread, handed to SteamVR by the render thread, so every
// IVROverlay call stays on the one thread that already makes them.
static std::mutex g_imageMtx;
static std::string g_readyImage;
static FlipOverlay g_panel;

static void MarkDirty()
{
    { std::lock_guard<std::mutex> lk(g_mtx); g_dirty = true; }
    g_cv.notify_one();
}

static void QueueSave(const char *key, const std::string &value)
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        bool merged = false;
        for (auto &kv : g_pendingSaves)
            if (kv.first == key) { kv.second = value; merged = true; }
        if (!merged)
            g_pendingSaves.emplace_back(key, value);
        g_dirty = true;
    }
    g_cv.notify_one();
}

static std::string FloatStr(float v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

static int Nearest(const std::vector<float> &vals, float v)
{
    int best = 0;
    for (int i = 1; i < (int)vals.size(); ++i)
        if (fabsf(vals[i] - v) < fabsf(vals[best] - v))
            best = i;
    return best;
}

static Item Numeric(const wchar_t *label, const wchar_t *hint, float *target, const char *key,
                    std::vector<float> vals, std::function<std::wstring(float)> fmt,
                    std::function<void()> after = nullptr)
{
    Item it;
    it.label = label; it.hint = hint; it.kind = Kind::Choice;
    for (float v : vals) it.names.push_back(fmt(v));
    it.get = [target, vals]() { return Nearest(vals, *target); };
    it.set = [target, vals, key, after](int i) {
        *target = vals[i];
        QueueSave(key, FloatStr(vals[i]));
        if (after) after();
    };
    return it;
}

static Item Toggle(const wchar_t *label, const wchar_t *hint, bool *target, const char *key)
{
    Item it;
    it.label = label; it.hint = hint; it.kind = Kind::Toggle;
    it.names = { L"Off", L"On" };
    it.get = [target]() { return *target ? 1 : 0; };
    it.set = [target, key](int i) { *target = (i != 0); QueueSave(key, *target ? "true" : "false"); };
    return it;
}

// A choice between named values stored in an int (or bool) with config words.
static Item Named(const wchar_t *label, const wchar_t *hint, std::vector<std::wstring> names,
                  std::function<int()> get, std::function<void(int)> apply,
                  const char *key, std::vector<const char *> words)
{
    Item it;
    it.label = label; it.hint = hint; it.kind = Kind::Choice; it.wrap = true;
    it.names = names;
    it.get = get;
    it.set = [apply, key, words](int i) { apply(i); QueueSave(key, words[i]); };
    return it;
}

static std::wstring Fmt(const wchar_t *f, float v)
{
    wchar_t buf[64];
    swprintf(buf, 64, f, v);
    return buf;
}

static std::vector<float> Range(float lo, float hi, float step)
{
    std::vector<float> v;
    for (float x = lo; x <= hi + step * 0.5f; x += step)
        v.push_back(roundf(x * 1000.0f) / 1000.0f);
    return v;
}

static void BuildModel(VR *vr)
{
    g_tabs.clear();

    Tab comfort{ L"Comfort" };
    comfort.items.push_back(Named(L"Turning", L"Snap turns in steps; smooth turns continuously.",
        { L"Smooth", L"Snap" },
        [vr]() { return vr->m_SnapTurning ? 1 : 0; },
        [vr](int i) { vr->m_SnapTurning = (i != 0); },
        "SnapTurning", { "false", "true" }));
    comfort.items.push_back(Numeric(L"Snap turn angle", L"Degrees per snap.",
        &vr->m_SnapTurnAngle, "SnapTurnAngle", { 15.f, 22.5f, 30.f, 45.f, 60.f, 90.f },
        [](float v) { return Fmt(L"%g\x00B0", v); }));
    // TurnSpeed is degrees per millisecond of stick; show it per second.
    comfort.items.push_back(Numeric(L"Smooth turn speed", L"Degrees per second at full stick.",
        &vr->m_TurnSpeed, "TurnSpeed",
        { 0.06f, 0.09f, 0.12f, 0.15f, 0.18f, 0.24f, 0.30f, 0.35f, 0.45f, 0.60f },
        [](float v) { return Fmt(L"%.0f\x00B0/s", v * 1000.0f); }));
    comfort.items.push_back(Numeric(L"Height offset", L"Raises the camera only. Shots land this far below the reticle.",
        &vr->m_HeightOffsetMeters, "HeightOffsetMeters", Range(-0.30f, 0.50f, 0.05f),
        [](float v) { return (fabsf(v) < 0.001f) ? std::wstring(L"0.00 m") : Fmt(L"%+.2f m", v); }));
    comfort.items.push_back(Named(L"Dominant hand", L"Which hand points at menus.",
        { L"Right", L"Left" },
        [vr]() { return vr->m_LeftHanded ? 1 : 0; },
        [vr](int i) { vr->m_LeftHanded = (i != 0); },
        "LeftHanded", { "false", "true" }));
    {
        Item recenter;
        recenter.label = L"Recenter";
        recenter.hint = L"Takes your current head position as the new standing spot.";
        recenter.kind = Kind::Button;
        recenter.names = { L"Recenter now" };
        recenter.action = [vr]() { vr->ResetPosition(); };
        comfort.items.push_back(recenter);
    }
    g_tabs.push_back(comfort);

    Tab aim{ L"Aiming" };
    aim.items.push_back(Toggle(L"Reticle", L"Aiming dot drawn in the centre of your view.",
        &dxvk::g_GESVR_DrawReticle, "VRReticle"));
    aim.items.push_back(Named(L"Reticle style", L"",
        { L"Dot", L"Cross", L"Ring", L"Ring + dot" },
        []() { const int s = dxvk::g_GESVR_ReticleStyle; return s == 1 ? 0 : s == 0 ? 1 : s == 2 ? 2 : 3; },
        [](int i) { static const int map[] = { 1, 0, 2, 3 }; dxvk::g_GESVR_ReticleStyle = map[i]; },
        "VRReticleStyle", { "dot", "cross", "ring", "ringdot" }));
    {
        static const std::vector<float> sizes = { 0.001f, 0.0015f, 0.002f, 0.003f, 0.004f, 0.005f, 0.0065f, 0.008f };
        Item size = Numeric(L"Reticle size", L"",
            &dxvk::g_GESVR_ReticleScale, "VRReticleSize", sizes, [](float) { return std::wstring(); });
        for (size_t i = 0; i < size.names.size(); ++i)
            size.names[i] = std::to_wstring(i + 1);
        aim.items.push_back(size);
    }
    aim.items.push_back(Named(L"Reticle colour", L"",
        { L"Yellow", L"White", L"Green", L"Red", L"Cyan" },
        []() { return dxvk::g_GESVR_ReticleColor; },
        [](int i) { dxvk::g_GESVR_ReticleColor = i; },
        "VRReticleColor", { "yellow", "white", "green", "red", "cyan" }));
    aim.items.push_back(Toggle(L"Scope zoom", L"Hold left grip to aim. Scoped weapons zoom the whole view.",
        &vr->m_ScopeZoom, "ScopeZoom"));
    g_tabs.push_back(aim);

    Tab display{ L"Display" };
    display.items.push_back(Toggle(L"Wrist watch", L"Health, armour, ammo and round time on your off hand.",
        &vr->m_ShowWristHUD, "ShowWristHUD"));
    display.items.push_back(Named(L"Watch shows", L"",
        { L"When you look at it", L"Always" },
        [vr]() { return vr->m_WatchAlwaysVisible ? 1 : 0; },
        [vr](int i) { vr->m_WatchAlwaysVisible = (i != 0); },
        "WatchAlwaysVisible", { "false", "true" }));
    display.items.push_back(Numeric(L"Menu distance", L"How far away the main menu floats.",
        &vr->m_MenuDistanceMeters, "MenuDistanceMeters", Range(1.0f, 3.0f, 0.2f),
        [](float v) { return Fmt(L"%.1f m", v); }, []() { GESVR_RequestMenuReplace(); }));
    display.items.push_back(Numeric(L"Menu size", L"Width of the main menu panel.",
        &vr->m_MenuWidthMeters, "MenuWidthMeters", Range(1.2f, 3.6f, 0.2f),
        [](float v) { return Fmt(L"%.1f m", v); }, []() { GESVR_RequestMenuReplace(); }));
    display.items.push_back(Numeric(L"World scale", L"Game units per metre. Lower makes you feel taller. Default 40.",
        &vr->m_VRScale, "VRScale", Range(34.0f, 48.0f, 1.0f),
        [](float v) { return Fmt(L"%.0f", v); }));
    g_tabs.push_back(display);
}

// ---------------------------------------------------------------------------
// Layout. One function answers both "where is it drawn" and "what did the
// laser hit", so the two can never disagree.
// ---------------------------------------------------------------------------
enum
{
    ID_TAB = 100,      // + tab index
    ID_ROW = 200,      // + row*4 + part
    PART_LEFT = 0, PART_RIGHT = 1, PART_MAIN = 2,
    ID_CLOSE = 900,
};

static const int kTitleH = 96;
static const int kTabTop = 112, kTabH = 60;
static const int kRowTop = 196, kRowH = 104;
static const int kMargin = 48;
static const int kCtrlL = 780, kCtrlR = W - kMargin;
static const int kFooterTop = 830;

struct Hit { int id; RECT rc; };

static void Layout(int tab, std::vector<Hit> &out)
{
    out.clear();
    const int tabW = (W - 2 * kMargin) / (int)g_tabs.size();
    for (int i = 0; i < (int)g_tabs.size(); ++i)
        out.push_back({ ID_TAB + i, { kMargin + i * tabW, kTabTop, kMargin + (i + 1) * tabW, kTabTop + kTabH } });

    const auto &items = g_tabs[tab].items;
    for (int r = 0; r < (int)items.size(); ++r)
    {
        const int top = kRowTop + r * kRowH;
        const int base = ID_ROW + r * 4;
        switch (items[r].kind)
        {
        case Kind::Choice:
            out.push_back({ base + PART_LEFT,  { kCtrlL, top + 22, kCtrlL + 80, top + 82 } });
            out.push_back({ base + PART_MAIN,  { kCtrlL + 80, top + 22, kCtrlR - 80, top + 82 } });
            out.push_back({ base + PART_RIGHT, { kCtrlR - 80, top + 22, kCtrlR, top + 82 } });
            break;
        case Kind::Toggle:
            out.push_back({ base + PART_MAIN, { kCtrlR - 150, top + 25, kCtrlR, top + 79 } });
            break;
        case Kind::Button:
            out.push_back({ base + PART_MAIN, { kCtrlR - 300, top + 22, kCtrlR, top + 82 } });
            break;
        }
    }
    out.push_back({ ID_CLOSE, { W - kMargin - 200, kFooterTop + 4, W - kMargin, kFooterTop + 58 } });
}

static int HitTest(int tab, int x, int y)
{
    if (x < 0 || y < 0)
        return -1;
    std::vector<Hit> hits;
    Layout(tab, hits);
    for (const Hit &h : hits)
        if (x >= h.rc.left && x < h.rc.right && y >= h.rc.top && y < h.rc.bottom)
            return h.id;
    return -1;
}

// ---------------------------------------------------------------------------
// Drawing (draw thread only). GDI into a 2x DIB, filtered down, then PNG.
// ---------------------------------------------------------------------------
static const COLORREF C_BG      = RGB(17, 19, 23);
static const COLORREF C_TITLE   = RGB(27, 30, 36);
static const COLORREF C_GOLD    = RGB(214, 178, 72);
static const COLORREF C_TEXT    = RGB(236, 236, 236);
static const COLORREF C_HINT    = RGB(140, 146, 158);
static const COLORREF C_CTRL    = RGB(38, 42, 50);
static const COLORREF C_HOVER   = RGB(62, 68, 82);
static const COLORREF C_LINE    = RGB(34, 37, 44);
static const COLORREF C_OFF     = RGB(70, 74, 84);

struct Fonts { HFONT title, tab, label, hint, value; };

static Fonts MakeFonts(const Canvas &c)
{
    const wchar_t *ui = L"Segoe UI";
    return { c.Font(ui, 46, FW_BOLD), c.Font(ui, 30, FW_SEMIBOLD), c.Font(ui, 32, FW_SEMIBOLD),
             c.Font(ui, 21, FW_NORMAL), c.Font(ui, 30, FW_SEMIBOLD) };
}

static void Arrow(Canvas &c, RECT r, bool right, COLORREF col)
{
    const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f;
    const float a = 11.0f;
    if (right) c.Poly({ { cx - a / 2, cy - a }, { cx + a / 2 + a / 3, cy }, { cx - a / 2, cy + a } }, col);
    else       c.Poly({ { cx + a / 2, cy - a }, { cx - a / 2 - a / 3, cy }, { cx + a / 2, cy + a } }, col);
}

static RECT FindRect(const std::vector<Hit> &hits, int id)
{
    for (const Hit &h : hits)
        if (h.id == id)
            return h.rc;
    return { 0, 0, 0, 0 };
}

static void DrawPanel(Canvas &c, const Fonts &f, int tab, int hover)
{
    std::vector<Hit> hits;
    Layout(tab, hits);

    c.Fill({ 0, 0, W, H }, C_BG);
    c.Fill({ 0, 0, W, kTitleH }, C_TITLE);
    c.Fill({ 0, kTitleH - 3, W, kTitleH }, C_GOLD);
    c.Text(f.title, C_TEXT, L"VR SETTINGS", { kMargin, 0, W / 2, kTitleH - 3 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    c.Text(f.hint, C_HINT, L"GoldenEye: Source VR", { W / 2, 0, W - kMargin, kTitleH - 3 }, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    for (int i = 0; i < (int)g_tabs.size(); ++i)
    {
        const RECT rc = FindRect(hits, ID_TAB + i);
        const bool sel = (i == tab);
        if (hover == ID_TAB + i && !sel)
            c.Fill(rc, C_CTRL);
        c.Text(f.tab, sel ? C_GOLD : C_HINT, g_tabs[i].name, rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        c.Fill({ rc.left, rc.bottom - (sel ? 4 : 1), rc.right, rc.bottom }, sel ? C_GOLD : C_LINE);
    }

    const auto &items = g_tabs[tab].items;
    for (int r = 0; r < (int)items.size(); ++r)
    {
        const Item &it = items[r];
        const int top = kRowTop + r * kRowH;
        const int base = ID_ROW + r * 4;
        const bool hasHint = !it.hint.empty();
        c.Text(f.label, C_TEXT, it.label,
             { kMargin, top + (hasHint ? 14 : 0), kCtrlL - 20, hasHint ? top + 58 : top + kRowH },
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (hasHint)
            c.Text(f.hint, C_HINT, it.hint, { kMargin, top + 58, kCtrlL - 20, top + 90 },
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        const int cur = it.get ? it.get() : 0;
        switch (it.kind)
        {
        case Kind::Choice:
        {
            const RECT l = FindRect(hits, base + PART_LEFT), m = FindRect(hits, base + PART_MAIN),
                       rr = FindRect(hits, base + PART_RIGHT);
            c.Round({ l.left, l.top, rr.right, rr.bottom }, 12, C_CTRL);
            const bool atStart = !it.wrap && cur <= 0;
            const bool atEnd = !it.wrap && cur >= (int)it.names.size() - 1;
            if (hover == base + PART_LEFT && !atStart)  c.Round(l, 12, C_HOVER);
            if (hover == base + PART_RIGHT && !atEnd)   c.Round(rr, 12, C_HOVER);
            if (hover == base + PART_MAIN)              c.Fill(m, C_HOVER);
            Arrow(c, l, false, atStart ? C_OFF : C_GOLD);
            Arrow(c, rr, true, atEnd ? C_OFF : C_GOLD);
            if (cur >= 0 && cur < (int)it.names.size())
                c.Text(f.value, C_TEXT, it.names[cur], m, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        case Kind::Toggle:
        {
            const RECT m = FindRect(hits, base + PART_MAIN);
            const bool on = (cur != 0);
            COLORREF track = on ? C_GOLD : C_OFF;
            if (hover == base + PART_MAIN)
                track = on ? RGB(234, 200, 96) : C_HOVER;
            c.Round(m, (m.bottom - m.top) / 2, track);
            const int kd = (m.bottom - m.top) - 10;
            const int kx = on ? (m.right - 5 - kd) : (m.left + 5);
            c.Round({ kx, m.top + 5, kx + kd, m.top + 5 + kd }, kd / 2, on ? RGB(24, 24, 24) : C_TEXT);
            c.Text(f.value, on ? C_TEXT : C_HINT, on ? L"On" : L"Off",
                 { m.left - 140, m.top, m.left - 18, m.bottom }, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        case Kind::Button:
        {
            const RECT m = FindRect(hits, base + PART_MAIN);
            c.Round(m, 12, hover == base + PART_MAIN ? C_HOVER : C_CTRL);
            c.Round({ m.left, m.bottom - 4, m.right, m.bottom }, 2, C_GOLD);
            c.Text(f.value, C_TEXT, it.names.empty() ? it.label : it.names[0], m,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        }
        if (r + 1 < (int)items.size())
            c.Fill({ kMargin, top + kRowH - 1, W - kMargin, top + kRowH }, C_LINE);
    }

    c.Fill({ 0, kFooterTop - 8, W, kFooterTop - 7 }, C_LINE);
    c.Text(f.hint, C_HINT, L"Changes apply and save immediately.   B / Y / left X: close",
         { kMargin, kFooterTop, W - kMargin - 220, kFooterTop + 62 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const RECT close = FindRect(hits, ID_CLOSE);
    c.Round(close, 12, hover == ID_CLOSE ? RGB(234, 200, 96) : C_GOLD);
    c.Text(f.tab, RGB(20, 20, 20), L"Close", close, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// Rounded panel corners, as the alpha mask for Canvas::Resolve.
static bool InsidePanel(float x, float y)
{
    const float r = 28.0f;
    float dx = 0.0f, dy = 0.0f;
    if (x < r) dx = r - x; else if (x > W - r) dx = x - (W - r);
    if (y < r) dy = r - y; else if (y > H - r) dy = y - (H - r);
    return dx * dx + dy * dy <= r * r;
}

// config.txt write-back: replace every live "key=" line (the parser keeps the
// LAST duplicate, so all of them must change) or append. Written to a temp
// file and swapped in, so the hot-reload thread never reads half a file.
static void WriteConfigValues(const std::vector<std::pair<std::string, std::string>> &kvs)
{
    char path[MAX_STR_LEN];
    VR::MakeVRPath(path, sizeof(path), "config.txt");
    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string nl = (text.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        pos = e + 1;
    }
    for (const auto &kv : kvs)
    {
        const std::string prefix = kv.first + "=";
        bool found = false;
        for (std::string &line : lines)
            if (line.compare(0, prefix.size(), prefix) == 0)
            {
                line = prefix + kv.second;
                found = true;
            }
        if (!found)
            lines.push_back(prefix + kv.second);
        Game::logMsg("VRSettings: saved %s=%s", kv.first.c_str(), kv.second.c_str());
    }
    std::string outText;
    for (const std::string &line : lines)
        outText += line + nl;

    const std::string tmp = std::string(path) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << outText;
    }
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        if (MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING))
            return;
        Sleep(30);   // the hot-reload thread may have it open for reading
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << outText;
    DeleteFileA(tmp.c_str());
}

static void DrawThread()
{
    Canvas canvas;
    if (!canvas.Create(W, H, SS))
    {
        Game::logMsg("VRSettings: could not create the draw surface; panel disabled");
        return;
    }
    const Fonts f = MakeFonts(canvas);

    char tempDir[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tempDir);
    std::vector<uint8_t> rgba;
    int flip = 0;

    while (true)
    {
        std::vector<std::pair<std::string, std::string>> saves;
        bool draw = false;
        int tab = 0, hover = -1;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return g_dirty || !g_pendingSaves.empty(); });
            saves.swap(g_pendingSaves);
            draw = g_dirty && g_open.load();
            g_dirty = false;
            tab = g_tab;
            hover = g_hover;
        }

        if (!saves.empty())
            WriteConfigValues(saves);

        if (draw && g_panel.Valid())
        {
            DrawPanel(canvas, f, tab, hover);
            canvas.Resolve(rgba, InsidePanel);
            // Alternate two files: SteamVR loads asynchronously and may cache
            // by path, so never rewrite the one it could still be reading.
            char png[MAX_PATH];
            snprintf(png, sizeof(png), "%sgesvr_settings_%d.png", tempDir, flip);
            flip ^= 1;
            if (WritePng(png, rgba, W, H))
            {
                std::lock_guard<std::mutex> lk(g_imageMtx);
                g_readyImage = png;
            }
            else
                Game::logMsg("VRSettings: could not write %s", png);
            Sleep(30);   // at most ~30 redraws a second while the laser sweeps
        }
    }
}

// ---------------------------------------------------------------------------
// Render-thread side
// ---------------------------------------------------------------------------
void Init(VR *vr)
{
    g_vr = vr;
    if (!vr || !vr->m_Overlay)
        return;
    BuildModel(vr);
    if (!g_panel.Create(vr->m_Overlay, "GESVRSettingsKey", "GESVR Settings"))
    {
        Game::logMsg("VRSettings: CreateOverlay failed; panel disabled");
        return;
    }
    for (int i = 0; i < 2; ++i)
    {
        vr->m_Overlay->SetOverlayWidthInMeters(g_panel.Handle(i), kWidthMeters);
        vr->m_Overlay->SetOverlaySortOrder(g_panel.Handle(i), 40);
        vr->m_Overlay->SetOverlayInputMethod(g_panel.Handle(i), vr::VROverlayInputMethod_Mouse);
    }
    std::thread(DrawThread).detach();
    InstallMenuHook();
    Game::logMsg("VRSettings: panel ready (%d tabs)", (int)g_tabs.size());
}

bool IsOpen()
{
    return g_open.load();
}

void Open()
{
    if (!g_vr || !g_panel.Valid() || g_open.load())
        return;
    g_open.store(true);
    g_placed = false;
    g_legacyPrev = true;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_hover = -1;
    }
    // Dim the game menu behind us. ShowMenuPanel sets alpha only once, when it
    // configures the panel, so this holds until Close puts it back.
    if (g_vr->m_Overlay && g_vr->m_MainMenuHandle)
        g_vr->m_Overlay->SetOverlayAlpha(g_vr->m_MainMenuHandle, 0.35f);
    MarkDirty();
    Game::logMsg("VRSettings: opened");
}

void Close()
{
    if (!g_open.exchange(false))
        return;
    if (g_vr && g_vr->m_Overlay)
    {
        g_panel.Hide(g_vr->m_Overlay);
        if (g_vr->m_MainMenuHandle)
            g_vr->m_Overlay->SetOverlayAlpha(g_vr->m_MainMenuHandle, 1.0f);
    }
    Game::logMsg("VRSettings: closed");
}

static void Place()
{
    const vr::TrackedDevicePose_t &hmd = g_vr->m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (!hmd.bPoseIsValid || !vr::VRCompositor())
        return;
    const vr::HmdMatrix34_t &m = hmd.mDeviceToAbsoluteTracking;
    float fx = -m.m[0][2], fz = -m.m[2][2];
    const float len = sqrtf(fx * fx + fz * fz);
    if (len > 0.001f) { fx /= len; fz /= len; } else { fx = 0.0f; fz = -1.0f; }
    // Yaw only, level, a little below eye height. Columns: right, up, back.
    vr::HmdMatrix34_t xf{};
    xf.m[0][0] = -fz; xf.m[0][1] = 0.0f; xf.m[0][2] = -fx;
    xf.m[1][0] = 0.0f; xf.m[1][1] = 1.0f; xf.m[1][2] = 0.0f;
    xf.m[2][0] = fx;  xf.m[2][1] = 0.0f; xf.m[2][2] = -fz;
    xf.m[0][3] = m.m[0][3] + fx * kDistanceMeters;
    xf.m[1][3] = m.m[1][3] - 0.12f;
    xf.m[2][3] = m.m[2][3] + fz * kDistanceMeters;
    for (int i = 0; i < 2; ++i)
    {
        g_vr->m_Overlay->SetOverlayTransformAbsolute(g_panel.Handle(i), vr::VRCompositor()->GetTrackingSpace(), &xf);
        g_vr->m_Overlay->SetOverlayWidthInMeters(g_panel.Handle(i), kWidthMeters);
    }
    g_placed = true;
}

static bool RayToPanel(const vr::HmdMatrix34_t &ray, int &x, int &y)
{
    vr::VROverlayIntersectionParams_t ip{};
    ip.eOrigin = vr::VRCompositor()->GetTrackingSpace();
    ip.vSource.v[0] = ray.m[0][3]; ip.vSource.v[1] = ray.m[1][3]; ip.vSource.v[2] = ray.m[2][3];
    ip.vDirection.v[0] = -ray.m[0][2]; ip.vDirection.v[1] = -ray.m[1][2]; ip.vDirection.v[2] = -ray.m[2][2];
    vr::VROverlayIntersectionResults_t ir{};
    if (!g_vr->m_Overlay->ComputeOverlayIntersection(g_panel.Front(), &ip, &ir))
        return false;
    const float u = ir.vUVs.v[0], v = 1.0f - ir.vUVs.v[1];
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
        return false;
    x = (int)(u * (W - 1));
    y = (int)(v * (H - 1));
    return true;
}

static void Activate(int id)
{
    if (id < 0)
        return;
    if (id == ID_CLOSE)
    {
        Close();
        return;
    }
    if (id >= ID_TAB && id < ID_TAB + (int)g_tabs.size())
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_tab = id - ID_TAB;
        g_hover = -1;
        g_dirty = true;
    }
    else if (id >= ID_ROW)
    {
        int tab;
        { std::lock_guard<std::mutex> lk(g_mtx); tab = g_tab; }
        const int row = (id - ID_ROW) / 4, part = (id - ID_ROW) % 4;
        auto &items = g_tabs[tab].items;
        if (row >= (int)items.size())
            return;
        Item &it = items[row];
        if (it.kind == Kind::Button)
        {
            if (it.action) it.action();
            Game::logMsg("VRSettings: %ls", it.label.c_str());
        }
        else
        {
            const int n = (int)it.names.size();
            const int cur = it.get();
            int next = cur;
            if (it.kind == Kind::Toggle)
                next = cur ? 0 : 1;
            else if (part == PART_LEFT)
                next = it.wrap ? (cur + n - 1) % n : (cur > 0 ? cur - 1 : cur);
            else   // right arrow, or the value itself: step forward
                next = it.wrap ? (cur + 1) % n : (cur < n - 1 ? cur + 1 : cur);
            if (next != cur)
                it.set(next);
        }
    }
    MarkDirty();
}

void Frame()
{
    if (!g_open.load() || !g_vr || !g_vr->m_Overlay || !g_panel.Valid())
        return;
    vr::IVROverlay *ov = g_vr->m_Overlay;

    if (!g_placed)
        Place();

    std::string image;
    {
        std::lock_guard<std::mutex> lk(g_imageMtx);
        image.swap(g_readyImage);
    }
    if (!image.empty())
    {
        g_panel.Load(ov, image);
        static int s_logged = 0;
        if (s_logged < 3 || g_panel.m_lastError != vr::VROverlayError_None)
        {
            Game::logMsg("VRSettings: panel image %s err=%d", image.c_str(), (int)g_panel.m_lastError);
            ++s_logged;
        }
    }
    if (g_panel.Pump(ov) != vr::VROverlayError_None)
        Game::logMsg("VRSettings: SteamVR could not load the panel image");

    // Re-asserted every frame, like the game menu: overlay input routing did
    // not stay put when it was set once (see ProcessMenuInput). Both halves
    // of the flip pair, since either can be the one on show.
    const vr::HmdVector2_t scale = { (float)W, (float)H };
    for (int i = 0; i < 2; ++i)
    {
        ov->SetOverlayMouseScale(g_panel.Handle(i), &scale);
        ov->SetOverlayFlag(g_panel.Handle(i), vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
        ov->SetOverlayInputMethod(g_panel.Handle(i), vr::VROverlayInputMethod_Mouse);
    }
    g_panel.Show(ov);

    // Same pointer rules as the game menu: SteamVR's laser point while its
    // laser is on the panel (kept while it holds still -- MouseMove only comes
    // on movement), otherwise our own ray from the same controller tip. Never
    // the head: a head ray used to take over after 400 ms of a still laser.
    static int s_laserX = -1, s_laserY = -1;
    static bool s_laserFocus = false;
    static ULONGLONG s_lastLaser = 0;
    const ULONGLONG now = GetTickCount64();
    static ULONGLONG s_lastRun = 0;
    if (now - s_lastRun > 250)
    {
        s_laserFocus = false;   // closed and reopened: nothing carries over
        s_laserX = s_laserY = -1;
    }
    s_lastRun = now;
    bool laserDown = false;
    vr::VREvent_t ev{};
    // The laser only reaches the half on show; its image-load events are
    // polled by Pump on the hidden half, so only the front is read here.
    while (ov->PollNextOverlayEvent(g_panel.Front(), &ev, sizeof(ev)))
    {
        switch (ev.eventType)
        {
        case vr::VREvent_MouseMove:
            s_laserX = (int)ev.data.mouse.x;
            s_laserY = H - (int)ev.data.mouse.y;
            s_laserFocus = true;
            s_lastLaser = now;
            break;
        case vr::VREvent_FocusEnter:
            s_laserFocus = true;
            break;
        case vr::VREvent_FocusLeave:
            s_laserFocus = false;
            break;
        case vr::VREvent_MouseButtonDown:
            laserDown = true;
            break;
        default:
            break;
        }
    }
    // The game menu behind us still gets laser events when pointed at. Drain
    // them, or ProcessMenuInput replays a stale click the moment we close.
    if (g_vr->m_MainMenuHandle)
        while (ov->PollNextOverlayEvent(g_vr->m_MainMenuHandle, &ev, sizeof(ev))) {}

    int rayX = -1, rayY = -1;
    vr::HmdMatrix34_t ray{};
    const bool rayHit = g_vr->GetPointerPose(ray) && RayToPanel(ray, rayX, rayY);
    if (s_laserFocus && !rayHit && now - s_lastLaser > 1000)
        s_laserFocus = false;   // left the panel without telling us
    int s_x = -1, s_y = -1;
    if (s_laserFocus && s_laserX >= 0)
    {
        s_x = s_laserX;
        s_y = s_laserY;
    }
    else if (rayHit)
    {
        s_x = rayX;
        s_y = rayY;
    }

    int tab;
    { std::lock_guard<std::mutex> lk(g_mtx); tab = g_tab; }
    const int hover = HitTest(tab, s_x, s_y);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (hover != g_hover)
        {
            g_hover = hover;
            g_dirty = true;
            g_cv.notify_one();
        }
    }

    const bool legacyNow = g_vr->LegacyTriggerDown();
    const bool legacyEdge = legacyNow && !g_legacyPrev;
    g_legacyPrev = legacyNow;
    const bool pressed = laserDown || legacyEdge
        || g_vr->PressedDigitalAction(g_vr->m_MenuSelect, true)
        || g_vr->PressedDigitalAction(g_vr->m_ActionPrimaryAttack, true);
    static ULONGLONG s_lastClick = 0;
    // Only while pointing at something: a click off the panel does nothing.
    if (pressed && hover >= 0 && now - s_lastClick > 300)
    {
        s_lastClick = now;
        Activate(hover);
        if (!g_open.load())
            return;
    }

    if (g_vr->PressedDigitalAction(g_vr->m_MenuBack, true) || g_vr->PressedDigitalAction(g_vr->m_Pause, true))
        Close();
}
} // namespace VRSettings
