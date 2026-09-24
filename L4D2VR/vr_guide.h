#pragma once

// The throw guide's dots, handed from VR::UpdateThrowGuide (once a frame, on
// the render thread, just before the eyes are drawn) to the DXVK eye capture,
// which draws them into each eye image after the reticle. Same thread, same
// frame -- the reticle's aim point travels the same way.
namespace dxvk
{
struct GESVR_GuideDot
{
    float u, v;   // normalised image position in that eye, 0..1, top left origin
    float r;      // radius as a fraction of the eye image height
    int c;        // 0 the reticle colour, 1 spent (a burnt-down fuse), 2 danger
};

constexpr int kGESVRGuideMax = 160;

extern int g_GESVR_GuideCount[2];
extern GESVR_GuideDot g_GESVR_Guide[2][kGESVRGuideMax];
}
