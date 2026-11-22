#include "tray_icon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <vector>

namespace {

// Signed distance to rounded box centered at (0,0) with half-extents (bx, by) and corner radius r
float SdRoundedBox(float px, float py, float bx, float by, float r) {
    float qx = std::fabs(px) - bx + r;
    float qy = std::fabs(py) - by + r;
    float ox = std::max(qx, 0.0f);
    float oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

// Signed distance to a line segment from (ax,ay) to (bx,by)
float SdSegment(float px, float py, float ax, float ay, float bx, float by) {
    float pax = px - ax, pay = py - ay;
    float bax = bx - ax, bay = by - ay;
    float denom = bax * bax + bay * bay;
    float h = (denom > 1e-6f) ? std::clamp((pax * bax + pay * bay) / denom, 0.0f, 1.0f) : 0.0f;
    float dx = pax - bax * h;
    float dy = pay - bay * h;
    return std::sqrt(dx * dx + dy * dy);
}

float SmoothCoverage(float dist, float aaWidth) {
    return std::clamp(0.5f - dist / aaWidth, 0.0f, 1.0f);
}

} // namespace

TrayIcon::~TrayIcon() {
    Hide();
    if (m_icon) {
        DestroyIcon(m_icon);
        m_icon = nullptr;
    }
}

HICON TrayIcon::CreateAppIcon(int sizePx) {
    if (sizePx < 16) sizePx = 16;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = sizePx;
    bmi.bmiHeader.biHeight      = -sizePx; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screenDC = GetDC(nullptr);
    HBITMAP hColor = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screenDC);
    if (!hColor || !bits) return nullptr;

    auto* pixels = static_cast<uint32_t*>(bits);
    float invSize = 1.0f / static_cast<float>(sizePx);
    float aa = 1.5f * invSize;

    for (int y = 0; y < sizePx; ++y) {
        for (int x = 0; x < sizePx; ++x) {
            float u = (static_cast<float>(x) + 0.5f) * invSize - 0.5f;
            float v = (static_cast<float>(y) + 0.5f) * invSize - 0.5f;

            // Rounded Windows 11 blue badge background
            float dBg = SdRoundedBox(u, v, 0.46f, 0.46f, 0.14f);
            float bgCov = SmoothCoverage(dBg, aa);

            // Vertical gradient (#0082E0 -> #005CB2)
            float grad = std::clamp(v + 0.5f, 0.0f, 1.0f);
            float r = 0.0f;
            float g = 130.0f * (1.0f - grad) + 92.0f * grad;
            float b = 224.0f * (1.0f - grad) + 178.0f * grad;

            // Left arrow: Up (↑)
            float stroke = 0.058f;
            float dUp = std::min({
                SdSegment(u, v, -0.17f,  0.24f, -0.17f, -0.22f),
                SdSegment(u, v, -0.17f, -0.22f, -0.31f, -0.06f),
                SdSegment(u, v, -0.17f, -0.22f, -0.03f, -0.06f),
            }) - stroke;

            // Right arrow: Down (↓)
            float dDn = std::min({
                SdSegment(u, v,  0.17f, -0.24f,  0.17f,  0.22f),
                SdSegment(u, v,  0.17f,  0.22f,  0.03f,  0.06f),
                SdSegment(u, v,  0.17f,  0.22f,  0.31f,  0.06f),
            }) - stroke;

            float fgCov = SmoothCoverage(std::min(dUp, dDn), aa);

            float outR = r * (1.0f - fgCov) + 255.0f * fgCov;
            float outG = g * (1.0f - fgCov) + 255.0f * fgCov;
            float outB = b * (1.0f - fgCov) + 255.0f * fgCov;
            float outA = bgCov * 255.0f;

            uint32_t a8 = static_cast<uint32_t>(std::clamp(std::lround(outA), 0L, 255L));
            uint32_t r8 = static_cast<uint32_t>(std::clamp(std::lround(outR), 0L, 255L));
            uint32_t g8 = static_cast<uint32_t>(std::clamp(std::lround(outG), 0L, 255L));
            uint32_t b8 = static_cast<uint32_t>(std::clamp(std::lround(outB), 0L, 255L));

            pixels[y * sizePx + x] = (a8 << 24) | (r8 << 16) | (g8 << 8) | b8;
        }
    }

    // Explicitly zero-initialized 1bpp AND mask (word-aligned rows) so alpha channel controls transparency cleanly
    int maskRowBytes = ((sizePx + 15) / 16) * 2;
    std::vector<BYTE> maskBits(static_cast<size_t>(maskRowBytes * sizePx), 0);
    HBITMAP hMask = CreateBitmap(sizePx, sizePx, 1, 1, maskBits.data());

    ICONINFO ii{};
    ii.fIcon    = TRUE;
    ii.hbmMask  = hMask;
    ii.hbmColor = hColor;

    HICON hIcon = CreateIconIndirect(&ii);
    if (hMask) DeleteObject(hMask);
    DeleteObject(hColor);
    return hIcon;
}

bool TrayIcon::Show(HWND ownerHwnd, UINT callbackMsg, const std::wstring& tooltip) {
    Hide();
    m_ownerHwnd   = ownerHwnd;
    m_callbackMsg = callbackMsg;
    if (!m_icon) {
        int iconSize = GetSystemMetrics(SM_CXSMICON);
        if (iconSize < 16) iconSize = 16;
        m_icon = CreateAppIcon(std::max(iconSize, 32));
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = m_ownerHwnd;
    nid.uID              = 1;
    nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = m_callbackMsg;
    nid.hIcon            = m_icon ? m_icon : LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(nid.szTip, tooltip.c_str(), _TRUNCATE);

    m_visible = (Shell_NotifyIconW(NIM_ADD, &nid) == TRUE);
    return m_visible;
}

void TrayIcon::UpdateTooltip(const std::wstring& tooltip) {
    if (!m_visible || !m_ownerHwnd) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_ownerHwnd;
    nid.uID    = 1;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, tooltip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::Hide() {
    if (!m_visible || !m_ownerHwnd) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_ownerHwnd;
    nid.uID    = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    m_visible = false;
}
