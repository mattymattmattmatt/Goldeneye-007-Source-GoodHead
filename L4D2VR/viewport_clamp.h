#pragma once

// Lets the per-eye render targets be larger than the game window.
//
// THE BUG THIS FIXES, found 2026-09-28 by tracing the D3D9 device during an
// eye pass (vr_eyediag.h): CShaderAPIDx8::SetViewports in shaderapidx9.dll
// clamps every viewport to the BACKBUFFER's dimensions:
//
//     shaderapidx9+0x1A7A8  call GetBackBufferDimensions(&w, &h)
//     shaderapidx9+0x1A819  cmp width,  w ; jbe skip ; width  = w
//     shaderapidx9+0x1A825  cmp height, h ; jbe skip ; height = h
//     (and the same pair again at +0x1A835 / +0x1A843 on the other branch)
//
// So an eye target bigger than the window was drawn into only its top-left
// window-sized corner: a 2688x2688 target with a 2560x1440 window got a
// 2560x1440 viewport. The material system does nothing wrong -- it pushes our
// target at its full 2688x2688 and hands SetViewports exactly that.
//
// This is what the project chased three times as "the SECOND RenderView of a
// frame draws at the backbuffer's viewport". It was never about the second
// view: a 1806x1873 target under a 1920x1080 window is clamped to 1806x1080 --
// full width, top 58% -- which is exactly the recorded symptom.
//
// The fix turns the four conditional jumps that skip the clamp into
// unconditional ones, ONLY for the duration of our eye passes, and puts them
// back straight after. Under DXVK a viewport larger than its target is
// harmless (Vulkan clips it), and rendering is single-threaded here
// (mat_queue_mode 0), so nothing else can be inside SetViewports while the
// bytes are flipped. Every byte is verified against the expected original
// before anything is written; any mismatch disables the patch for good.
namespace ViewportClamp
{
    // Find and verify the code. Safe to call repeatedly; cheap after the first.
    bool Available();
    // true = our eye passes may use viewports bigger than the window.
    void SetUnclamped(bool unclamped);
}
