#pragma once
#include <string>

class VR;

// A short, head-locked text panel just below the centre of view, for feedback
// the player needs without taking the headset off (the numpad weapon tuning).
// Same plumbing as the watch: GDI on a worker thread, PNG, flip overlay.
namespace VRToast
{
    void Init(VR *vr);
    void Show(const std::wstring &title, const std::wstring &detail, int millis = 3000);
    void Update();   // every frame, render thread
}
