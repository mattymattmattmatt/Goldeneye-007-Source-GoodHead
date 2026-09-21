#pragma once
#include <Windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// GDI drawing surface for the panels we hand to SteamVR as images (the VR
// settings panel, the wrist watch). Everything is drawn at SS times the output
// size and box-filtered down, which is what gives GDI's hard edges and text
// their anti-aliasing. All coordinates passed in are OUTPUT pixels.
//
// Draw-thread only: never touch one from the render thread.
class Canvas
{
public:
    struct Pt { float x, y; };

    ~Canvas();
    bool Create(int w, int h, int ss);
    int Width() const { return m_w; }
    int Height() const { return m_h; }

    HFONT Font(const wchar_t *face, int px, int weight) const;
    void Clear(COLORREF c);
    void Fill(RECT r, COLORREF c);
    void Round(RECT r, int radius, COLORREF c);
    void Circle(float cx, float cy, float r, COLORREF c);
    void Poly(const std::vector<Pt> &pts, COLORREF c);
    void Line(const std::vector<Pt> &pts, float width, COLORREF c);
    void Text(HFONT f, COLORREF c, const std::wstring &s, RECT r, UINT fmt);
    SIZE TextSize(HFONT f, const std::wstring &s);
    int Descent(HFONT f);                    // output pixels below the baseline
    std::wstring FaceName(HFONT f);          // what GDI actually gave us

    // Filter down to RGBA. inside(x, y) gets a sample position in output
    // pixels and decides that sample's alpha (GDI never writes alpha).
    void Resolve(std::vector<uint8_t> &rgba, const std::function<bool(float, float)> &inside);

private:
    int m_w = 0, m_h = 0, m_ss = 1;
    HDC m_dc = nullptr;
    HBITMAP m_bmp = nullptr;
    void *m_bits = nullptr;
};

// RGBA PNG with deflate "stored" blocks (no compression). SteamVR only takes
// SetOverlayFromFile images as PNG or JPG, and JPG has no alpha.
bool WritePng(const char *path, const std::vector<uint8_t> &rgba, int w, int h);
