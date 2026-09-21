#pragma once
#include "openvr.h"
#include <Windows.h>
#include <string>

// Two SteamVR overlays that take turns showing one image panel.
//
// SetOverlayFromFile loads asynchronously, and the overlay shows nothing until
// the new image is in -- so reloading the one on show flashes it blank on
// every redraw (the settings panel on each hover change, the watch every
// second). Instead each new image goes into the hidden overlay and the two
// swap when SteamVR reports it loaded, or after a timeout in case that event
// never arrives. Render thread only.
class FlipOverlay
{
public:
    bool Create(vr::IVROverlay *ov, const char *key, const char *name)
    {
        for (int i = 0; i < 2; ++i)
        {
            const std::string k = std::string(key) + (i ? "B" : "A");
            const std::string n = std::string(name) + (i ? " B" : " A");
            if (ov->CreateOverlay(k.c_str(), n.c_str(), &m_h[i]) != vr::VROverlayError_None)
            {
                m_h[0] = m_h[1] = vr::k_ulOverlayHandleInvalid;
                return false;
            }
            ov->HideOverlay(m_h[i]);
        }
        return true;
    }

    bool Valid() const { return m_h[0] != vr::k_ulOverlayHandleInvalid; }
    vr::VROverlayHandle_t Front() const { return m_h[m_front]; }
    vr::VROverlayHandle_t Back() const { return m_h[m_front ^ 1]; }
    vr::VROverlayHandle_t Handle(int i) const { return m_h[i]; }

    // Queue an image. If one is still loading, the newest waits its turn.
    void Load(vr::IVROverlay *ov, const std::string &png)
    {
        if (m_loading) { m_pending = png; return; }
        StartLoad(ov, png);
    }

    // Swap once the back image is in. Call every frame the panel is live.
    // Returns the error of a load that just failed, else VROverlayError_None.
    vr::EVROverlayError Pump(vr::IVROverlay *ov)
    {
        vr::EVROverlayError failed = vr::VROverlayError_None;
        if (!m_loading)
            return failed;
        bool done = false;
        vr::VREvent_t ev{};
        while (ov->PollNextOverlayEvent(Back(), &ev, sizeof(ev)))
        {
            if (ev.eventType == vr::VREvent_ImageLoaded)
                done = true;
            else if (ev.eventType == vr::VREvent_ImageFailed)
            {
                done = true;
                failed = vr::VROverlayError_UnableToLoadFile;
            }
        }
        if (!done && GetTickCount64() - m_loadStart > 250)
            done = true;   // never heard back; swap anyway rather than freeze the panel
        if (!done)
            return failed;

        m_loading = false;
        if (failed == vr::VROverlayError_None)
        {
            m_front ^= 1;
            m_haveImage = true;
            if (m_visible)
            {
                ov->ShowOverlay(Front());
                ov->HideOverlay(Back());
            }
        }
        if (!m_pending.empty())
        {
            const std::string next = m_pending;
            m_pending.clear();
            StartLoad(ov, next);
        }
        return failed;
    }

    void Show(vr::IVROverlay *ov)
    {
        if (m_visible || !m_haveImage)
        {
            m_visible = true;
            return;
        }
        m_visible = true;
        ov->ShowOverlay(Front());
        ov->HideOverlay(Back());
    }

    void Hide(vr::IVROverlay *ov)
    {
        if (!m_visible)
            return;
        m_visible = false;
        ov->HideOverlay(m_h[0]);
        ov->HideOverlay(m_h[1]);
    }

    bool Visible() const { return m_visible; }

private:
    void StartLoad(vr::IVROverlay *ov, const std::string &png)
    {
        m_lastError = ov->SetOverlayFromFile(Back(), png.c_str());
        m_loading = (m_lastError == vr::VROverlayError_None);
        m_loadStart = GetTickCount64();
    }

    vr::VROverlayHandle_t m_h[2] = { vr::k_ulOverlayHandleInvalid, vr::k_ulOverlayHandleInvalid };
    int m_front = 0;
    bool m_loading = false;
    bool m_haveImage = false;
    bool m_visible = false;
    ULONGLONG m_loadStart = 0;
    std::string m_pending;

public:
    vr::EVROverlayError m_lastError = vr::VROverlayError_None;
};
