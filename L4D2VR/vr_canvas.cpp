#include "vr_canvas.h"
#include <cstdio>

Canvas::~Canvas()
{
    if (m_bmp) DeleteObject(m_bmp);
    if (m_dc) DeleteDC(m_dc);
}

bool Canvas::Create(int w, int h, int ss)
{
    m_w = w; m_h = h; m_ss = ss;
    m_dc = CreateCompatibleDC(nullptr);
    if (!m_dc)
        return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w * ss;
    bi.bmiHeader.biHeight = -h * ss;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    m_bmp = CreateDIBSection(m_dc, &bi, DIB_RGB_COLORS, &m_bits, nullptr, 0);
    if (!m_bmp || !m_bits)
        return false;
    SelectObject(m_dc, m_bmp);
    SetBkMode(m_dc, TRANSPARENT);
    return true;
}

HFONT Canvas::Font(const wchar_t *face, int px, int weight) const
{
    return CreateFontW(-px * m_ss, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, face);
}

void Canvas::Clear(COLORREF c)
{
    Fill({ 0, 0, m_w, m_h }, c);
}

void Canvas::Fill(RECT r, COLORREF c)
{
    RECT s = { r.left * m_ss, r.top * m_ss, r.right * m_ss, r.bottom * m_ss };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(m_dc, &s, b);
    DeleteObject(b);
}

void Canvas::Round(RECT r, int radius, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(m_dc, b);
    HGDIOBJ op = SelectObject(m_dc, GetStockObject(NULL_PEN));
    RoundRect(m_dc, r.left * m_ss, r.top * m_ss, r.right * m_ss + 1, r.bottom * m_ss + 1,
              radius * 2 * m_ss, radius * 2 * m_ss);
    SelectObject(m_dc, op);
    SelectObject(m_dc, ob);
    DeleteObject(b);
}

void Canvas::Circle(float cx, float cy, float r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(m_dc, b);
    HGDIOBJ op = SelectObject(m_dc, GetStockObject(NULL_PEN));
    Ellipse(m_dc, (int)((cx - r) * m_ss), (int)((cy - r) * m_ss),
            (int)((cx + r) * m_ss) + 1, (int)((cy + r) * m_ss) + 1);
    SelectObject(m_dc, op);
    SelectObject(m_dc, ob);
    DeleteObject(b);
}

void Canvas::Poly(const std::vector<Pt> &pts, COLORREF c)
{
    std::vector<POINT> p(pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
        p[i] = { (LONG)(pts[i].x * m_ss), (LONG)(pts[i].y * m_ss) };
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(m_dc, b);
    HGDIOBJ op = SelectObject(m_dc, GetStockObject(NULL_PEN));
    Polygon(m_dc, p.data(), (int)p.size());
    SelectObject(m_dc, op);
    SelectObject(m_dc, ob);
    DeleteObject(b);
}

void Canvas::Line(const std::vector<Pt> &pts, float width, COLORREF c)
{
    std::vector<POINT> p(pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
        p[i] = { (LONG)(pts[i].x * m_ss), (LONG)(pts[i].y * m_ss) };
    LOGBRUSH lb = { BS_SOLID, c, 0 };
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                            (DWORD)(width * m_ss), &lb, 0, nullptr);
    HGDIOBJ op = SelectObject(m_dc, pen);
    Polyline(m_dc, p.data(), (int)p.size());
    SelectObject(m_dc, op);
    DeleteObject(pen);
}

void Canvas::Text(HFONT f, COLORREF c, const std::wstring &s, RECT r, UINT fmt)
{
    RECT sr = { r.left * m_ss, r.top * m_ss, r.right * m_ss, r.bottom * m_ss };
    SelectObject(m_dc, f);
    SetTextColor(m_dc, c);
    DrawTextW(m_dc, s.c_str(), -1, &sr, fmt | DT_NOPREFIX);
}

SIZE Canvas::TextSize(HFONT f, const std::wstring &s)
{
    SelectObject(m_dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(m_dc, s.c_str(), (int)s.size(), &sz);
    sz.cx /= m_ss;
    sz.cy /= m_ss;
    return sz;
}

int Canvas::Descent(HFONT f)
{
    SelectObject(m_dc, f);
    TEXTMETRICW tm{};
    GetTextMetricsW(m_dc, &tm);
    return tm.tmDescent / m_ss;
}

std::wstring Canvas::FaceName(HFONT f)
{
    SelectObject(m_dc, f);
    wchar_t name[LF_FACESIZE] = {};
    GetTextFaceW(m_dc, LF_FACESIZE, name);
    return name;
}

void Canvas::Resolve(std::vector<uint8_t> &rgba, const std::function<bool(float, float)> &inside)
{
    GdiFlush();
    rgba.assign((size_t)m_w * m_h * 4, 0);
    const uint8_t *bgra = (const uint8_t *)m_bits;
    const int bw = m_w * m_ss;
    const int n = m_ss * m_ss;
    const float inv = 1.0f / (float)m_ss;
    for (int y = 0; y < m_h; ++y)
        for (int x = 0; x < m_w; ++x)
        {
            int r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < m_ss; ++sy)
                for (int sx = 0; sx < m_ss; ++sx)
                {
                    const int px = x * m_ss + sx, py = y * m_ss + sy;
                    const uint8_t *p = bgra + ((size_t)py * bw + px) * 4;
                    b += p[0]; g += p[1]; r += p[2];
                    if (inside((px + 0.5f) * inv, (py + 0.5f) * inv))
                        a += 255;
                }
            uint8_t *o = &rgba[((size_t)y * m_w + x) * 4];
            o[0] = (uint8_t)(r / n); o[1] = (uint8_t)(g / n); o[2] = (uint8_t)(b / n); o[3] = (uint8_t)(a / n);
        }
}

static uint32_t g_crcTable[256];
static bool g_crcReady = false;

static uint32_t Crc(const uint8_t *p, size_t n)
{
    if (!g_crcReady)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            g_crcTable[i] = c;
        }
        g_crcReady = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
        c = g_crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void Be32(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

static void Chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
{
    Be32(out, (uint32_t)data.size());
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    Be32(out, Crc(&out[start], out.size() - start));
}

bool WritePng(const char *path, const std::vector<uint8_t> &rgba, int w, int h)
{
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 4 + 1) * h);
    for (int y = 0; y < h; ++y)
    {
        raw.push_back(0);   // filter: none
        raw.insert(raw.end(), rgba.begin() + (size_t)y * w * 4, rgba.begin() + (size_t)(y + 1) * w * 4);
    }
    std::vector<uint8_t> z = { 0x78, 0x01 };
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t c : raw) { s1 = (s1 + c) % 65521; s2 = (s2 + s1) % 65521; }
    for (size_t pos = 0; pos < raw.size(); )
    {
        const size_t n = (raw.size() - pos > 65535) ? 65535 : raw.size() - pos;
        const bool last = (pos + n == raw.size());
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    Be32(z, (s2 << 16) | s1);

    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    std::vector<uint8_t> ihdr;
    Be32(ihdr, (uint32_t)w); Be32(ihdr, (uint32_t)h);
    ihdr.insert(ihdr.end(), { 8, 6, 0, 0, 0 });   // 8-bit RGBA
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});

    FILE *f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        return false;
    const bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
    fclose(f);
    return ok;
}
