#pragma once

// Eye-pass diagnostics, shared between the mod and DXVK.
//
// The per-eye render target path has failed three times on the same symptom:
// the SECOND RenderView of a frame only fills the top ~58% of its target. Every
// earlier attempt reasoned about Source from the outside. This asks the D3D9
// device instead, which is our own code: while a traced eye pass runs, DXVK
// logs every render target, viewport, scissor and clear the engine actually
// sets, who set it (module+offset), and a summary of the state each draw ran
// under. The mod supplies the one thing DXVK cannot know -- which eye.
//
// Off unless EyeDiag=true in config.txt. Outside a traced frame the cost is a
// load and a branch per device call.
namespace dxvk
{
    // 0 = not inside a stereo eye pass, 1 = left eye, 2 = right eye.
    extern int  g_GESVR_EyePass;
    // True only for the frames being traced.
    extern bool g_GESVR_EyeTrace;
    // Test: in a traced menu frame, re-upload each managed texture from its
    // CPU copy before it is drawn (config EyeDiagForceUpload).
    extern bool g_GESVR_DiagForceUpload;

    // Bracket one eye's RenderView. eyeW/eyeH is the target the pass is
    // meant to fill (0 when rendering straight to the backbuffer), so the
    // device can flag any viewport or scissor that does not cover it.
    void GESVR_EyeTraceBeginPass(int eye, unsigned eyeW, unsigned eyeH);
    void GESVR_EyeTraceEndPass(int eye);

    // Log the device's render target, viewport and scissor right now -- the
    // state the engine's RenderView is about to inherit.
    void GESVR_EyeTraceSnapshot(const char *label);

    // Free-form line for material-system hooks: text, the caller as
    // module+offset, and optionally a filtered stack scan (Source modules only).
    void GESVR_EyeTraceNote(const char *text, const void *caller, bool withStack);

    // One line in vrmod_log with its caller and a filtered stack, whether or
    // not a trace is running.
    void GESVR_LogWithStack(const char *text, const void *caller);
}

// Test hook for the GPU error path (config FakeSubmitOOM, see vr.h): the next
// n vkQueueSubmit calls report VK_ERROR_OUT_OF_HOST_MEMORY without reaching
// the driver. Defined in d3d9_vr.cpp, consumed in dxvk_cmdlist.cpp.
void GESVR_FakeSubmitOOM(int n);
