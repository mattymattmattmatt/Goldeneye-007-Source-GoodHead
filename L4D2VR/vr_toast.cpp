#include "vr_toast.h"
#include "vr.h"
#include <Windows.h>
#include "game.h"
#include "vr_canvas.h"
#include "vr_flipoverlay.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace VRToast
{
static const int W = 900, H = 170, SS = 2;

static VR *g_vr = nullptr;
static FlipOverlay g_panel;
static bool g_placed = false;

static std::mutex g_mtx;
static std::condition_variable g_cv;
static bool g_dirty = false;
static std::wstring g_title, g_detail;
static ULONGLONG g_hideAt = 0;

static std::mutex g_imageMtx;
static std::string g_readyImage;

static bool InsidePanel(float x, float y)
{
    const float r = 22.0f;
    float dx = 0.0f, dy = 0.0f;
    if (x < r) dx = r - x; else if (x > W - r) dx = x - (W - r);
    if (y < r) dy = r - y; else if (y > H - r) dy = y - (H - r);
    return dx * dx + dy * dy <= r * r;
}

static void DrawThread()
{
    Canvas c;
    if (!c.Create(W, H, SS))
        return;
    HFONT titleFont = c.Font(L"Bahnschrift SemiBold Condensed", 44, FW_NORMAL);
    HFONT detailFont = c.Font(L"Bahnschrift SemiBold Condensed", 34, FW_NORMAL);
    if (c.FaceName(titleFont).find(L"Bahnschrift") != 0)
    {
        titleFont = c.Font(L"Segoe UI", 40, FW_BOLD);
        detailFont = c.Font(L"Segoe UI", 30, FW_NORMAL);
    }
    char tempDir[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tempDir);
    std::vector<uint8_t> rgba;
    int flip = 0;
    while (true)
    {
        std::wstring title, detail;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return g_dirty; });
            g_dirty = false;
            title = g_title;
            detail = g_detail;
        }
        c.Clear(RGB(10, 20, 14));
        c.Fill({ 0, 0, 10, H }, RGB(132, 255, 176));
        c.Text(titleFont, RGB(132, 255, 176), title, { 34, 14, W - 24, 84 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        c.Text(detailFont, RGB(230, 236, 232), detail, { 34, 88, W - 24, 156 }, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        c.Resolve(rgba, InsidePanel);
        char png[MAX_PATH];
        snprintf(png, sizeof(png), "%sgesvr_toast_%d.png", tempDir, flip);
        flip ^= 1;
        if (WritePng(png, rgba, W, H))
        {
            std::lock_guard<std::mutex> lk(g_imageMtx);
            g_readyImage = png;
        }
        Sleep(30);
    }
}

void Init(VR *vr)
{
    g_vr = vr;
    if (!vr || !vr->m_Overlay || !g_panel.Create(vr->m_Overlay, "GESVRToastKey", "GESVR Toast"))
        return;
    for (int i = 0; i < 2; ++i)
    {
        vr->m_Overlay->SetOverlayInputMethod(g_panel.Handle(i), vr::VROverlayInputMethod_None);
        vr->m_Overlay->SetOverlayFlag(g_panel.Handle(i), vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);
        vr->m_Overlay->SetOverlaySortOrder(g_panel.Handle(i), 60);
        vr->m_Overlay->SetOverlayWidthInMeters(g_panel.Handle(i), 0.55f);
    }
    std::thread(DrawThread).detach();
}

void Show(const std::wstring &title, const std::wstring &detail, int millis)
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_title = title;
        g_detail = detail;
        g_dirty = true;
        g_hideAt = GetTickCount64() + (ULONGLONG)millis;
    }
    g_cv.notify_one();
}

void Update()
{
    if (!g_vr || !g_vr->m_Overlay || !g_panel.Valid())
        return;
    vr::IVROverlay *ov = g_vr->m_Overlay;
    if (!g_placed)
    {
        // Head-locked, a metre out and a little below the centre of view.
        vr::HmdMatrix34_t xf = { 1.0f, 0.0f, 0.0f, 0.0f,
                                 0.0f, 1.0f, 0.0f, -0.28f,
                                 0.0f, 0.0f, 1.0f, -1.0f };
        for (int i = 0; i < 2; ++i)
            ov->SetOverlayTransformTrackedDeviceRelative(g_panel.Handle(i), vr::k_unTrackedDeviceIndex_Hmd, &xf);
        g_placed = true;
    }
    std::string image;
    {
        std::lock_guard<std::mutex> lk(g_imageMtx);
        image.swap(g_readyImage);
    }
    if (!image.empty())
        g_panel.Load(ov, image);
    g_panel.Pump(ov);

    ULONGLONG hideAt;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        hideAt = g_hideAt;
    }
    if (GetTickCount64() < hideAt)
        g_panel.Show(ov);
    else
        g_panel.Hide(ov);
}
} // namespace VRToast
