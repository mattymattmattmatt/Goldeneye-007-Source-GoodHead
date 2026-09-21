#include "vr_watch.h"
#include "vr.h"
#include <Windows.h>
#include "game.h"
#include "vr_canvas.h"
#include "vr_flipoverlay.h"
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>
#include <cctype>

namespace VRWatch
{
// Output size in pixels. The face is a circle inscribed in it.
static const int kSize = 520;
static const int kSS = 2;
static const float C = kSize * 0.5f;

static VR *g_vr = nullptr;
static FlipOverlay g_face;
static float g_widthSet = -1.0f;

static std::mutex g_mtx;
static std::condition_variable g_cv;
static bool g_dirty = false;
static WatchStats g_pending;
static int g_pendingMaxHealth = 100, g_pendingMaxArmor = 100;

static std::mutex g_imageMtx;
static std::string g_readyImage;

// ---------------------------------------------------------------------------
// Weapon names, as GoldenEye 64's watch lists them. Keyed by the viewmodel's
// file name after "v_"; most specific first, since matching is by substring.
// ---------------------------------------------------------------------------
std::wstring WeaponName(const std::string &viewmodel)
{
    if (viewmodel.empty())
        return L"";
    std::string base = viewmodel;
    for (char &ch : base)
        ch = (char)tolower((unsigned char)ch);
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos)
        base = base.substr(slash + 1);
    if (base.compare(0, 2, "v_") == 0)
        base = base.substr(2);
    const size_t dot = base.find('.');
    if (dot != std::string::npos)
        base = base.substr(0, dot);

    // Every v_*.mdl GE:S ships (models/weapons/*), matched exactly. The first
    // version matched substrings and had guessed names: the automatic shotgun
    // is "autosg", the grenade launcher "gl", throwing knives "tknife".
    static const struct { const char *key; const wchar_t *name; } kNames[] = {
        { "pp7", L"PP7 SPECIAL ISSUE" },       { "pp7_s", L"PP7 (SILENCED)" },
        { "pp7_gold", L"GOLD PP7" },           { "pp7_silver", L"SILVER PP7" },
        { "dd44", L"DD44 DOSTOVEI" },          { "klobb", L"KLOBB" },
        { "kf7", L"KF7 SOVIET" },              { "zmg", L"ZMG (9MM)" },
        { "d5k", L"D5K DEUTSCHE" },            { "d5k_silenced", L"D5K (SILENCED)" },
        { "phantom", L"PHANTOM" },             { "ar33", L"AR33 ASSAULT RIFLE" },
        { "rcp90", L"RC-P90" },                { "shotgun", L"SHOTGUN" },
        { "autosg", L"AUTOMATIC SHOTGUN" },    { "sniperrifle", L"SNIPER RIFLE" },
        { "cougar_magnum", L"COUGAR MAGNUM" }, { "goldengun", L"GOLDEN GUN" },
        { "moonraker", L"MOONRAKER LASER" },   { "gl", L"GRENADE LAUNCHER" },
        { "rocket_launcher", L"ROCKET LAUNCHER" }, { "grenade", L"HAND GRENADES" },
        { "remotemine", L"REMOTE MINES" },     { "proximitymine", L"PROXIMITY MINES" },
        { "timedmine", L"TIMED MINES" },       { "knife", L"HUNTING KNIFE" },
        { "tknife", L"THROWING KNIVES" },      { "slappers", L"SLAPPERS" },
        { "tazerboy", L"TASER" },              { "briefcasetoken", L"BRIEFCASE" },
        { "flagtoken", L"FLAG" },              { "keytoken", L"KEY" },
    };
    for (const auto &n : kNames)
        if (base == n.key)
            return n.name;

    std::wstring out;
    for (char ch : base)
        out += (ch == '_') ? L' ' : (wchar_t)toupper((unsigned char)ch);
    return out;
}

// ---------------------------------------------------------------------------
// Drawing (draw thread only)
// ---------------------------------------------------------------------------
struct Fonts { HFONT name, label, big, mid, time, corner; };

// Bahnschrift's condensed cut is the closest thing Windows ships to the
// watch's LCD lettering. Fall back to Arial Narrow if it is missing.
static HFONT Condensed(Canvas &c, int px)
{
    HFONT f = c.Font(L"Bahnschrift SemiBold Condensed", px, FW_NORMAL);
    if (c.FaceName(f).find(L"Bahnschrift") == 0)
        return f;
    DeleteObject(f);
    return c.Font(L"Arial Narrow", px, FW_BOLD);
}

static Canvas::Pt At(float deg, float r)
{
    const float a = deg * 3.14159265f / 180.0f;
    return { C + r * cosf(a), C - r * sinf(a) };
}

static std::vector<Canvas::Pt> Sector(float a0, float a1, float ri, float ro)
{
    std::vector<Canvas::Pt> p;
    const int steps = 6;
    for (int i = 0; i <= steps; ++i) p.push_back(At(a0 + (a1 - a0) * i / steps, ro));
    for (int i = steps; i >= 0; --i) p.push_back(At(a0 + (a1 - a0) * i / steps, ri));
    return p;
}

static COLORREF Lerp3(const COLORREF *stops, int n, float t)
{
    if (t <= 0.0f) return stops[0];
    if (t >= 1.0f) return stops[n - 1];
    const float f = t * (n - 1);
    const int i = (int)f;
    const float u = f - i;
    auto ch = [&](int shift) {
        const int a = (stops[i] >> shift) & 0xFF, b = (stops[i + 1] >> shift) & 0xFF;
        return (int)(a + (b - a) * u + 0.5f);
    };
    return RGB(ch(0), ch(8), ch(16));
}

static COLORREF Dim(COLORREF c, float k)
{
    return RGB((int)(GetRValue(c) * k), (int)(GetGValue(c) * k), (int)(GetBValue(c) * k));
}

// GoldenEye 64 bar: 12 blocks round an arc, filled from the bottom, spent
// blocks left dimly lit. Blocks thicken towards the top, as on the watch.
static void Bar(Canvas &c, float bottomDeg, float topDeg, int value, int maxValue,
                const COLORREF *stops, int nStops)
{
    const int kBlocks = 12;
    const float per = (topDeg - bottomDeg) / kBlocks;
    const float gap = 2.2f * (per > 0 ? 1.0f : -1.0f);
    int lit = 0;
    if (value > 0 && maxValue > 0)
    {
        lit = (int)ceilf((float)value * kBlocks / (float)maxValue);
        if (lit > kBlocks) lit = kBlocks;
    }
    for (int i = 0; i < kBlocks; ++i)
    {
        const float a0 = bottomDeg + per * i + gap * 0.5f;
        const float a1 = bottomDeg + per * (i + 1) - gap * 0.5f;
        const float ri = 162.0f, ro = ri + 34.0f + i * 1.6f;
        const COLORREF col = Lerp3(stops, nStops, (float)i / (kBlocks - 1));
        c.Poly(Sector(a0, a1, ri, ro), i < lit ? col : Dim(col, 0.3f));
    }
}

static void Pill(Canvas &c, float cx, float cy, float w, float h, COLORREF col)
{
    const int r = (int)((w < h ? w : h) * 0.5f);
    c.Round({ (LONG)(cx - w / 2), (LONG)(cy - h / 2), (LONG)(cx + w / 2), (LONG)(cy + h / 2) }, r, col);
}

static void Heart(Canvas &c, float cx, float cy)
{
    const COLORREF red = RGB(226, 46, 40);
    c.Circle(cx - 9.5f, cy - 5.0f, 12.0f, red);
    c.Circle(cx + 9.5f, cy - 5.0f, 12.0f, red);
    c.Poly({ { cx - 21.0f, cy - 1.0f }, { cx + 21.0f, cy - 1.0f }, { cx, cy + 22.0f } }, red);
    c.Line({ { cx - 17, cy + 1 }, { cx - 8, cy + 1 }, { cx - 4, cy - 8 }, { cx + 1, cy + 9 },
             { cx + 5, cy - 3 }, { cx + 8, cy + 1 }, { cx + 17, cy + 1 } }, 2.4f, RGB(250, 250, 250));
}

static void DrawWatch(Canvas &c, const Fonts &f, const WatchStats &s, int maxHealth, int maxArmor)
{
    c.Clear(RGB(0, 0, 0));

    // Brushed steel bezel with a knurled edge, then the black face.
    c.Circle(C, C, 259.0f, RGB(58, 60, 64));
    c.Circle(C, C, 256.0f, RGB(172, 174, 178));
    c.Circle(C, C, 251.0f, RGB(214, 216, 219));
    c.Circle(C, C, 246.0f, RGB(138, 140, 144));
    for (int i = 0; i < 180; ++i)
        c.Line({ At(i * 2.0f, 247.5f), At(i * 2.0f, 255.5f) }, 1.3f, RGB(98, 100, 104));
    c.Circle(C, C, 242.0f, RGB(92, 94, 98));
    c.Circle(C, C, 238.0f, RGB(6, 6, 8));
    for (int i = 0; i < 12; ++i)
        c.Line({ At(i * 30.0f, 221.0f), At(i * 30.0f, 231.0f) }, 3.0f, RGB(118, 118, 122));

    // Health: red at the top through orange to pale yellow at the bottom.
    static const COLORREF kHealth[] = { RGB(255, 238, 150), RGB(255, 204, 72), RGB(255, 150, 42),
                                        RGB(236, 78, 36), RGB(192, 40, 34) };
    // Armour: light cyan at the bottom to deep blue at the top.
    static const COLORREF kArmour[] = { RGB(132, 236, 255), RGB(84, 192, 255), RGB(52, 132, 250),
                                        RGB(36, 82, 236) };
    Bar(c, 244.0f, 116.0f, s.health, maxHealth, kHealth, 5);
    Bar(c, -64.0f, 64.0f, s.armor, maxArmor, kArmour, 4);

    // Green screen: a few concentric fills stand in for the LCD's glow.
    const RECT scr = { 140, 148, 380, 372 };
    const int kGlow = 16;
    for (int i = 0; i < kGlow; ++i)
    {
        const int k = i * 3;
        const float t = (float)i / (kGlow - 1);
        c.Round({ scr.left + k, scr.top + k, scr.right - k, scr.bottom - k }, 26 - i,
                RGB((int)(9 + 13 * t), (int)(46 + 42 * t), (int)(25 + 22 * t)));
    }

    // Grey studs and pills sitting on the screen's edge, as on the watch.
    const COLORREF stud = RGB(206, 206, 212);
    for (float a : { 128.0f, 163.0f, 197.0f, 232.0f, 52.0f, 17.0f, -17.0f, -52.0f })
    {
        const Canvas::Pt p = At(a, 142.0f);
        c.Circle(p.x, p.y, 8.0f, stud);
    }
    Pill(c, C - 124.0f, C, 34.0f, 12.0f, RGB(184, 186, 190));
    Pill(c, C + 124.0f, C, 34.0f, 12.0f, RGB(184, 186, 190));
    Pill(c, C, C + 166.0f, 14.0f, 40.0f, RGB(226, 226, 230));

    Heart(c, C, 74.0f);
    c.Text(f.corner, RGB(245, 245, 245), L"007", { 200, 94, 320, 124 }, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    wchar_t buf[64];
    if (s.health >= 0 && maxHealth > 0)
    {
        swprintf(buf, 64, L"%d%%", (s.health * 100 + maxHealth / 2) / maxHealth);
        c.Text(f.corner, RGB(255, 226, 110), buf, { 176, 436, 250, 468 }, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (s.armor >= 0 && maxArmor > 0)
    {
        swprintf(buf, 64, L"%d%%", (s.armor * 100 + maxArmor / 2) / maxArmor);
        c.Text(f.corner, RGB(112, 212, 255), buf, { 270, 436, 344, 468 }, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    const COLORREF lcd = RGB(132, 255, 176), lcdDim = RGB(52, 132, 86);
    const std::wstring weapon = WeaponName(s.weaponModel);
    c.Text(f.name, lcd, weapon.empty() ? L"NO WEAPON" : weapon, { scr.left + 12, 158, scr.right - 12, 184 },
           DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    c.Text(f.label, lcdDim, L"AMMO", { scr.left, 184, scr.right, 204 }, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Big clip count with the reserve beside it, sharing a baseline. Weapons
    // with no clip (grenades, mines) show their count as the big number.
    std::wstring big, mid;
    if (s.clip >= 0)
    {
        big = std::to_wstring(s.clip);
        if (s.reserve >= 0)
            mid = L"/" + std::to_wstring(s.reserve);
    }
    else if (s.reserve > 0)
        big = std::to_wstring(s.reserve);
    else
        big = L"--";
    const SIZE bs = c.TextSize(f.big, big);
    const SIZE ms = mid.empty() ? SIZE{ 0, 0 } : c.TextSize(f.mid, mid);
    const int gapPx = mid.empty() ? 0 : 6;
    const int total = bs.cx + gapPx + ms.cx;
    const int x0 = (int)C - total / 2;
    const int bigBottom = 296;
    c.Text(f.big, lcd, big, { x0, bigBottom - bs.cy, x0 + bs.cx + 2, bigBottom }, DT_LEFT | DT_TOP | DT_SINGLELINE);
    if (!mid.empty())
    {
        const int midBottom = bigBottom - (c.Descent(f.big) - c.Descent(f.mid));
        c.Text(f.mid, lcd, mid, { x0 + bs.cx + gapPx, midBottom - ms.cy, x0 + total + 2, midBottom },
               DT_LEFT | DT_TOP | DT_SINGLELINE);
    }

    if (s.timeLeft >= 0)
    {
        swprintf(buf, 64, L"TIME  %d:%02d", s.timeLeft / 60, s.timeLeft % 60);
        c.Text(f.time, lcd, buf, { scr.left, 298, scr.right, 330 }, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    else
        c.Text(f.label, lcdDim, L"NO TIME LIMIT", { scr.left, 298, scr.right, 330 }, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // The watch's row of underlined mode icons, reduced to the underlines.
    for (int i = 0; i < 5; ++i)
    {
        const int x = 168 + i * 40;
        c.Fill({ x, 350, x + 28, 353 }, i == 0 ? lcd : lcdDim);
    }
}

static bool InsideFace(float x, float y)
{
    const float dx = x - C, dy = y - C;
    return dx * dx + dy * dy <= 259.5f * 259.5f;
}

static void DrawThread()
{
    Canvas canvas;
    if (!canvas.Create(kSize, kSize, kSS))
    {
        Game::logMsg("VRWatch: could not create the draw surface; watch disabled");
        return;
    }
    const Fonts f{ Condensed(canvas, 21), Condensed(canvas, 15), Condensed(canvas, 100),
                   Condensed(canvas, 46), Condensed(canvas, 28), Condensed(canvas, 27) };
    Game::logMsg("VRWatch: font %ls", canvas.FaceName(f.big).c_str());

    char tempDir[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tempDir);
    std::vector<uint8_t> rgba;
    int flip = 0;
    while (true)
    {
        WatchStats s;
        int maxH, maxA;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return g_dirty; });
            g_dirty = false;
            s = g_pending;
            maxH = g_pendingMaxHealth;
            maxA = g_pendingMaxArmor;
        }
        DrawWatch(canvas, f, s, maxH, maxA);
        canvas.Resolve(rgba, InsideFace);
        char png[MAX_PATH];
        snprintf(png, sizeof(png), "%sgesvr_watch_%d.png", tempDir, flip);
        flip ^= 1;
        if (WritePng(png, rgba, kSize, kSize))
        {
            std::lock_guard<std::mutex> lk(g_imageMtx);
            g_readyImage = png;
        }
        Sleep(60);   // ammo can change every shot; ~15 redraws a second is plenty
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
    if (!g_face.Create(vr->m_Overlay, "GESVRWatchKey", "GESVR Watch"))
    {
        Game::logMsg("VRWatch: CreateOverlay failed; watch disabled");
        return;
    }
    for (int i = 0; i < 2; ++i)
    {
        vr->m_Overlay->SetOverlayInputMethod(g_face.Handle(i), vr::VROverlayInputMethod_None);
        vr->m_Overlay->SetOverlayFlag(g_face.Handle(i), vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);
        vr->m_Overlay->SetOverlaySortOrder(g_face.Handle(i), 50);
    }
    std::thread(DrawThread).detach();
    Game::logMsg("VRWatch: ready");
}

void Hide()
{
    if (g_vr && g_vr->m_Overlay && g_face.Valid())
        g_face.Hide(g_vr->m_Overlay);
}

// Pin the face to the off hand at the configured wrist offset, turned to face
// the headset. The relative transform is inverse(hand) * wanted, so SteamVR
// keeps it glued to the controller at its own latest pose.
static void Place(vr::TrackedDeviceIndex_t hand, bool rightHand)
{
    VR *v = g_vr;
    const vr::HmdMatrix34_t &h = v->m_Poses[hand].mDeviceToAbsoluteTracking;
    const vr::HmdMatrix34_t &e = v->m_Poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;

    // Config offset is (forward, left, up); device space is (right, up, back).
    float o[3] = { -v->m_WatchOffset.y, v->m_WatchOffset.z, -v->m_WatchOffset.x };
    if (rightHand)
        o[0] = -o[0];
    float w[3];
    for (int i = 0; i < 3; ++i)
        w[i] = h.m[i][0] * o[0] + h.m[i][1] * o[1] + h.m[i][2] * o[2] + h.m[i][3];

    float z[3] = { e.m[0][3] - w[0], e.m[1][3] - w[1], e.m[2][3] - w[2] };
    float len = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (len < 1e-4f) return;
    for (float &k : z) k /= len;
    // Right = worldUp x z; straight overhead falls back to the headset's right.
    float x[3] = { z[2], 0.0f, -z[0] };
    len = sqrtf(x[0] * x[0] + x[2] * x[2]);
    if (len < 1e-3f) { x[0] = e.m[0][0]; x[1] = e.m[1][0]; x[2] = e.m[2][0]; len = 1.0f; }
    for (float &k : x) k /= len;
    const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };

    vr::HmdMatrix34_t rel{};
    const float *axes[3] = { x, y, z };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            rel.m[i][j] = h.m[0][i] * axes[j][0] + h.m[1][i] * axes[j][1] + h.m[2][i] * axes[j][2];
    for (int i = 0; i < 3; ++i)
        rel.m[i][3] = o[i];
    // Both halves: the hidden one may be swapped in on any frame.
    v->m_Overlay->SetOverlayTransformTrackedDeviceRelative(g_face.Handle(0), hand, &rel);
    v->m_Overlay->SetOverlayTransformTrackedDeviceRelative(g_face.Handle(1), hand, &rel);
}

void Update()
{
    if (!g_vr || !g_vr->m_Overlay || !g_face.Valid())
        return;
    VR *v = g_vr;
    vr::IVROverlay *ov = v->m_Overlay;

    std::string image;
    {
        std::lock_guard<std::mutex> lk(g_imageMtx);
        image.swap(g_readyImage);
    }
    if (!image.empty())
    {
        g_face.Load(ov, image);
        static int s_logged = 0;
        if (s_logged < 2 || g_face.m_lastError != vr::VROverlayError_None)
        {
            Game::logMsg("VRWatch: face image %s err=%d", image.c_str(), (int)g_face.m_lastError);
            ++s_logged;
        }
    }
    if (g_face.Pump(ov) != vr::VROverlayError_None)
        Game::logMsg("VRWatch: SteamVR could not load the face image");

    if (!v->m_ShowWristHUD || !v->m_System)
    {
        Hide();
        return;
    }

    // Numbers at 10 Hz: every read is a handful of VirtualQuery-guarded
    // pointer reads, and nothing on the watch changes faster than that.
    static ULONGLONG s_lastRead = 0;
    static WatchStats s_last;
    static bool s_haveLast = false;
    static int s_maxHealthSeen = 100, s_maxArmorSeen = 100;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastRead >= 100)
    {
        s_lastRead = now;
        WatchStats s;
        v->ReadWatchStats(s);
        // GE:S may not network the maximums; the largest value seen stands in.
        if (s.health > s_maxHealthSeen) s_maxHealthSeen = s.health;
        if (s.armor > s_maxArmorSeen) s_maxArmorSeen = s.armor;
        if (!s_haveLast || !(s == s_last))
        {
            s_last = s;
            s_haveLast = true;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_pending = s;
                g_pendingMaxHealth = s.maxHealth > 0 ? s.maxHealth : s_maxHealthSeen;
                g_pendingMaxArmor = s.maxArmor > 0 ? s.maxArmor : s_maxArmorSeen;
                g_dirty = true;
            }
            g_cv.notify_one();
        }
    }

    const bool rightHand = v->m_LeftHanded;   // the watch goes on the off hand
    const vr::TrackedDeviceIndex_t hand = v->m_System->GetTrackedDeviceIndexForControllerRole(
        rightHand ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
    if (hand >= vr::k_unMaxTrackedDeviceCount || !v->m_Poses[hand].bPoseIsValid ||
        !v->m_Poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
    {
        Hide();
        return;
    }
    if (!v->m_WatchAlwaysVisible && !v->IsLookingAtOffhandWatch())
    {
        Hide();
        return;
    }

    if (g_widthSet != v->m_WatchWidth)
    {
        ov->SetOverlayWidthInMeters(g_face.Handle(0), v->m_WatchWidth);
        ov->SetOverlayWidthInMeters(g_face.Handle(1), v->m_WatchWidth);
        g_widthSet = v->m_WatchWidth;
    }
    Place(hand, rightHand);
    g_face.Show(ov);
}
} // namespace VRWatch
