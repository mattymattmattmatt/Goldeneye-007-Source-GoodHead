#pragma once

class VR;

// In-headset VR settings panel.
//
// Opened from a "VR Settings" entry that the launcher adds to GE:S's
// resource/GameMenu.res. That entry runs "engine echo gesvr_vrsettings", which
// we catch in tier0's spew output -- no ConCommand registration, so no engine
// ABI to get wrong. Left X in any menu toggles it too, as a fallback.
//
// The panel is its own OpenVR overlay. It is drawn with GDI on a worker thread
// and handed to SteamVR as a PNG file, so it never touches D3D or the Vulkan
// queue that DXVK and the compositor share. Every change is applied at once
// and written back to config.txt.
namespace VRSettings
{
    // Chain our spew function into tier0. Cheap; call periodically, since the
    // engine may install its own spew function after us.
    void InstallMenuHook();

    // Create the overlay and start the draw thread. Needs VR::m_Overlay.
    void Init(VR *vr);

    bool IsOpen();
    void Open();
    void Close();

    // True once per "VR Settings" menu click.
    bool ConsumeOpenRequest();

    // Per menu frame, render thread, while open: placement and input.
    void Frame();
}
