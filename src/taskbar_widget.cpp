#include "taskbar_widget.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace {

constexpr wchar_t kWidgetClassName[] = L"BarSpeed_TaskbarWidget";

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

bool FontExists(IDWriteFactory* factory, const wchar_t* familyName) {
    if (!factory) return false;
    IDWriteFontCollection* collection = nullptr;
    if (FAILED(factory->GetSystemFontCollection(&collection)) || !collection) {
        return false;
    }
    UINT32 idx = 0;
    BOOL exists = FALSE;
    collection->FindFamilyName(familyName, &idx, &exists);
    collection->Release();
    return exists == TRUE;
}

float MeasureTextWidth(IDWriteFactory* factory, IDWriteTextFormat* fmt, const std::wstring& text) {
    if (!factory || !fmt || text.empty()) return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    HRESULT hr = factory->CreateTextLayout(
        text.c_str(), static_cast<UINT32>(text.size()),
        fmt, 1000.0f, 100.0f, &layout
    );
    if (FAILED(hr) || !layout) return 0.0f;

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    layout->Release();
    return std::ceil(metrics.widthIncludingTrailingWhitespace);
}

} // namespace

TaskbarWidget::TaskbarWidget() = default;

TaskbarWidget::~TaskbarWidget() {
    Destroy();
}

bool TaskbarWidget::IsSystemTaskbarDark() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return true;
    }
    DWORD lightTheme = 0;
    DWORD size = sizeof(lightTheme);
    DWORD type = 0;
    LONG res = RegQueryValueExW(hKey, L"SystemUsesLightTheme", nullptr, &type,
                                reinterpret_cast<BYTE*>(&lightTheme), &size);
    RegCloseKey(hKey);
    if (res == ERROR_SUCCESS && type == REG_DWORD) {
        return lightTheme == 0;
    }
    return true;
}

bool TaskbarWidget::ResolveUseDarkTaskbar() const {
    if (m_cfg.themeMode == ThemeMode::DarkTaskbar) return true;
    if (m_cfg.themeMode == ThemeMode::LightTaskbar) return false;
    return m_systemDark;
}

bool TaskbarWidget::Initialize(HINSTANCE hInst, const AppConfig& cfg, Callbacks callbacks) {
    Destroy();
    m_hInst      = hInst;
    m_cfg        = cfg;
    m_callbacks  = std::move(callbacks);
    m_systemDark = IsSystemTaskbarDark();

    if (!EnsureD2DResources()) {
        return false;
    }
    if (!CreateWidgetWindow(hInst)) {
        return false;
    }

    SyncWithTaskbar();
    Render();
    return true;
}

void TaskbarWidget::Destroy() {
    if (m_tooltipHwnd && IsWindow(m_tooltipHwnd)) {
        DestroyWindow(m_tooltipHwnd);
    }
    m_tooltipHwnd = nullptr;

    if (m_hwnd && IsWindow(m_hwnd)) {
        DestroyWindow(m_hwnd);
    }
    m_hwnd          = nullptr;
    m_taskbarHwnd   = nullptr;
    m_attachedChild = false;
    m_mouseHovering = false;
    m_dragging      = false;
    m_lastX         = -99999;
    m_lastY         = -99999;

    ReleaseBackBuffer();
    DiscardD2DResources();
}

bool TaskbarWidget::CreateWidgetWindow(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = TaskbarWidget::WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWidgetClassName;
    RegisterClassExW(&wc);

    DWORD style   = WS_POPUP;
    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

    m_hwnd = CreateWindowExW(
        exStyle,
        kWidgetClassName,
        L"BarSpeedWidget",
        style,
        0, 0, m_width, m_height,
        nullptr, nullptr, hInst, this
    );
    if (!m_hwnd) return false;

    // Create multiline tooltip attached to the widget
    m_tooltipHwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASSW,
        nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        m_hwnd, nullptr, hInst, nullptr
    );
    if (m_tooltipHwnd) {
        SendMessageW(m_tooltipHwnd, TTM_SETMAXTIPWIDTH, 0, 420);
        TTTOOLINFOW ti{};
        ti.cbSize   = sizeof(ti);
        ti.uFlags   = TTF_SUBCLASS | TTF_IDISHWND;
        ti.hwnd     = m_hwnd;
        ti.uId      = reinterpret_cast<UINT_PTR>(m_hwnd);
        ti.lpszText = const_cast<LPWSTR>(L"BarSpeed");
        SendMessageW(m_tooltipHwnd, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }

    return true;
}

bool TaskbarWidget::EnsureD2DResources() {
    if (!m_d2dFactory) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2dFactory))) {
            return false;
        }
    }
    if (!m_dwriteFactory) {
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(&m_dwriteFactory)))) {
            return false;
        }
    }
    if (!m_renderTarget) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f,
            D2D1_RENDER_TARGET_USAGE_NONE,
            D2D1_FEATURE_LEVEL_DEFAULT
        );
        if (FAILED(m_d2dFactory->CreateDCRenderTarget(&props, &m_renderTarget))) {
            return false;
        }
        m_renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    if (!m_textFormat || !m_arrowFormat) {
        RebuildTextFormat();
    }
    return m_textFormat != nullptr && m_arrowFormat != nullptr;
}

void TaskbarWidget::DiscardD2DResources() {
    SafeRelease(m_arrowFormat);
    SafeRelease(m_textFormat);
    SafeRelease(m_renderTarget);
    SafeRelease(m_dwriteFactory);
    SafeRelease(m_d2dFactory);
}

void TaskbarWidget::RebuildTextFormat() {
    SafeRelease(m_textFormat);
    SafeRelease(m_arrowFormat);
    if (!m_dwriteFactory) return;

    const wchar_t* fontName = L"Segoe UI";
    if (FontExists(m_dwriteFactory, L"Segoe UI Variable Text")) {
        fontName = L"Segoe UI Variable Text";
    }

    // Direct2D render target is at 96 DPI (1 DIP = 1 physical pixel), so scale font pt by actual monitor DPI
    float fontPx = static_cast<float>(m_cfg.fontSizePt) * static_cast<float>(m_dpi) / 72.0f;

    m_dwriteFactory->CreateTextFormat(
        fontName,
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        fontPx,
        L"",
        &m_textFormat
    );
    if (m_textFormat) {
        m_textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    m_dwriteFactory->CreateTextFormat(
        fontName,
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        fontPx,
        L"",
        &m_arrowFormat
    );
    if (m_arrowFormat) {
        m_arrowFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_arrowFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

void TaskbarWidget::EnsureBackBuffer(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (m_memDC && m_dibBmp && m_bufW == width && m_bufH == height) return;

    ReleaseBackBuffer();

    HDC screenDC = GetDC(nullptr);
    m_memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    m_dibBmp = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screenDC);

    if (m_memDC && m_dibBmp) {
        m_oldBmp = static_cast<HBITMAP>(SelectObject(m_memDC, m_dibBmp));
        m_bufW   = width;
        m_bufH   = height;
    } else {
        ReleaseBackBuffer();
    }
}

void TaskbarWidget::ReleaseBackBuffer() {
    if (m_memDC) {
        if (m_oldBmp) {
            SelectObject(m_memDC, m_oldBmp);
            m_oldBmp = nullptr;
        }
        DeleteDC(m_memDC);
        m_memDC = nullptr;
    }
    if (m_dibBmp) {
        DeleteObject(m_dibBmp);
        m_dibBmp = nullptr;
    }
    m_bufW = 0;
    m_bufH = 0;
}

void TaskbarWidget::ComputeDesiredSize() {
    float scale = static_cast<float>(m_dpi) / 96.0f;

    std::wstring refSpeed = (m_cfg.speedUnit == SpeedUnit::BitsPerSec) ? L"999.9 Mbps" : L"999.9 MB/s";
    float arrowW = MeasureTextWidth(m_dwriteFactory, m_arrowFormat, L"\u2191:");
    float speedW = MeasureTextWidth(m_dwriteFactory, m_textFormat, refSpeed);

    float padX      = std::round(7.0f * scale);
    float arrowGap  = std::round(4.0f * scale);
    float totalW    = padX + arrowW + arrowGap + speedW + padX;

    if (m_cfg.showPing) {
        float pingW  = MeasureTextWidth(m_dwriteFactory, m_textFormat, L"999 ms");
        float divGap = std::round(7.0f * scale);
        float dotDia = std::round(7.0f * scale);
        float dotGap = std::round(5.0f * scale);
        totalW += divGap + 1.0f + divGap + dotDia + dotGap + pingW;
    }

    m_width = std::max(static_cast<int>(std::ceil(totalW)), static_cast<int>(std::round(76.0f * scale)));
}

void TaskbarWidget::ApplyConfig(const AppConfig& cfg) {
    bool fontChanged = (cfg.fontSizePt != m_cfg.fontSizePt);
    m_cfg = cfg;

    if (fontChanged) {
        RebuildTextFormat();
    }

    SyncWithTaskbar();
    Render();
    UpdateTooltipText();
}

void TaskbarWidget::RefreshTheme() {
    bool dark = IsSystemTaskbarDark();
    if (dark != m_systemDark) {
        m_systemDark = dark;
        Render();
    }
}

void TaskbarWidget::UpdateData(const NetworkSample& net, const PingStatus& ping) {
    m_net  = net;
    m_ping = ping;
    Render();
    UpdateTooltipText();
}

void TaskbarWidget::SyncWithTaskbar() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;

    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hTray || !IsWindow(hTray)) return;

    // Check DPI of taskbar monitor
    UINT trayDpi = GetDpiForWindow(hTray);
    if (trayDpi == 0) trayDpi = 96;
    if (trayDpi != m_dpi) {
        m_dpi = trayDpi;
        RebuildTextFormat();
    }

    RECT rcTray{};
    if (!GetWindowRect(hTray, &rcTray)) return;
    int trayW = rcTray.right - rcTray.left;
    int trayH = rcTray.bottom - rcTray.top;
    if (trayW <= 0 || trayH <= 0) return;

    // Compute widget dimensions
    ComputeDesiredSize();
    int vertMargin = MulDiv(4, static_cast<int>(m_dpi), 96);
    int desiredH   = std::max(trayH - vertMargin, MulDiv(28, static_cast<int>(m_dpi), 96));
    desiredH       = std::min(desiredH, trayH);
    bool sizeChanged = (desiredH != m_height || m_width != m_bufW);
    m_height = desiredH;

    // Find TrayNotifyWnd (the right-hand system tray + chevron area)
    HWND hNotify = FindWindowExW(hTray, nullptr, L"TrayNotifyWnd", nullptr);
    int scaledOffset = MulDiv(m_cfg.horizontalOffset, static_cast<int>(m_dpi), 96);

    int relX = 0;
    if (hNotify && IsWindow(hNotify)) {
        RECT rcNotify{};
        if (GetWindowRect(hNotify, &rcNotify) && rcNotify.right > rcNotify.left) {
            int notifyLeftRel = rcNotify.left - rcTray.left;
            relX = notifyLeftRel - m_width - scaledOffset;
        } else {
            relX = trayW - m_width - MulDiv(240, static_cast<int>(m_dpi), 96) - scaledOffset;
        }
    } else {
        relX = trayW - m_width - MulDiv(240, static_cast<int>(m_dpi), 96) - scaledOffset;
    }

    // Clamp cleanly inside taskbar horizontal bounds
    int margin = MulDiv(4, static_cast<int>(m_dpi), 96);
    relX = std::clamp(relX, margin, std::max(margin, trayW - m_width - margin));
    int relY = (trayH - m_height) / 2;

    // Attach to Shell_TrayWnd as a layered child window
    bool justAttached = false;
    if (!m_attachedChild || m_taskbarHwnd != hTray || GetParent(m_hwnd) != hTray) {
        LONG_PTR style = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
        style = (style & ~WS_POPUP) | WS_CHILD | WS_VISIBLE;
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, style);

        LONG_PTR exStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
        exStyle = (exStyle & ~WS_EX_TOPMOST) | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, exStyle);

        SetParent(m_hwnd, hTray);
        m_taskbarHwnd   = hTray;
        m_attachedChild = true;
        justAttached    = true;
        m_lastX = -99999;
        m_lastY = -99999;
    }

    // Keep above DesktopWindowContentBridge inside Shell_TrayWnd
    bool needsZFix = (GetWindow(m_hwnd, GW_HWNDPREV) != nullptr);
    if (relX != m_lastX || relY != m_lastY || sizeChanged || needsZFix || justAttached || !IsWindowVisible(m_hwnd)) {
        SetWindowPos(
            m_hwnd, HWND_TOP,
            relX, relY, m_width, m_height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW | (sizeChanged || justAttached ? SWP_FRAMECHANGED : 0)
        );
        m_lastX = relX;
        m_lastY = relY;
        if (sizeChanged || justAttached) {
            Render();
        }
    }
}

void TaskbarWidget::Render() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    if (!EnsureD2DResources()) return;

    EnsureBackBuffer(m_width, m_height);
    if (!m_memDC || !m_dibBmp) return;

    RECT bindRc{ 0, 0, m_width, m_height };
    if (FAILED(m_renderTarget->BindDC(m_memDC, &bindRc))) {
        return;
    }

    const bool dark = ResolveUseDarkTaskbar();
    const float scale = static_cast<float>(m_dpi) / 96.0f;
    const float w = static_cast<float>(m_width);
    const float h = static_cast<float>(m_height);

    m_renderTarget->BeginDraw();

    // 1/255 alpha makes the full widget bounds hit-testable for right-click & hover while visually transparent
    m_renderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f / 255.0f));

    // Native Windows 11 subtle hover pill highlight
    if (m_mouseHovering || m_dragging) {
        ID2D1SolidColorBrush* hoverBrush = nullptr;
        D2D1_COLOR_F hoverCol = dark
            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, m_dragging ? 0.14f : 0.075f)
            : D2D1::ColorF(0.0f, 0.0f, 0.0f, m_dragging ? 0.11f : 0.055f);
        if (SUCCEEDED(m_renderTarget->CreateSolidColorBrush(hoverCol, &hoverBrush))) {
            float rad = 4.0f * scale;
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(1.0f, 1.0f, w - 1.0f, h - 1.0f), rad, rad);
            m_renderTarget->FillRoundedRectangle(rr, hoverBrush);
            hoverBrush->Release();
        }
    }

    // Brushes for text, arrows, separator
    ID2D1SolidColorBrush* textBrush  = nullptr;
    ID2D1SolidColorBrush* upBrush    = nullptr;
    ID2D1SolidColorBrush* downBrush  = nullptr;
    ID2D1SolidColorBrush* sepBrush   = nullptr;

    D2D1_COLOR_F textCol = dark
        ? D2D1::ColorF(0.95f, 0.95f, 0.96f, 1.0f)
        : D2D1::ColorF(0.08f, 0.08f, 0.09f, 1.0f);
    D2D1_COLOR_F upCol = dark
        ? D2D1::ColorF(0.96f, 0.72f, 0.32f, 1.0f)  // warm amber-gold for upload arrow
        : D2D1::ColorF(0.75f, 0.38f, 0.00f, 1.0f);
    D2D1_COLOR_F downCol = dark
        ? D2D1::ColorF(0.40f, 0.80f, 0.98f, 1.0f)  // cool sky-blue for download arrow
        : D2D1::ColorF(0.00f, 0.42f, 0.78f, 1.0f);
    D2D1_COLOR_F sepCol = dark
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f)
        : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.16f);

    m_renderTarget->CreateSolidColorBrush(textCol, &textBrush);
    m_renderTarget->CreateSolidColorBrush(upCol, &upBrush);
    m_renderTarget->CreateSolidColorBrush(downCol, &downBrush);
    m_renderTarget->CreateSolidColorBrush(sepCol, &sepBrush);

    if (textBrush && upBrush && downBrush) {
        std::wstring upText   = NetworkMonitor::FormatSpeed(m_net.uploadBytesPerSec, m_cfg.speedUnit);
        std::wstring downText = NetworkMonitor::FormatSpeed(m_net.downloadBytesPerSec, m_cfg.speedUnit);

        std::wstring refSpeed = (m_cfg.speedUnit == SpeedUnit::BitsPerSec) ? L"999.9 Mbps" : L"999.9 MB/s";
        float arrowW   = MeasureTextWidth(m_dwriteFactory, m_arrowFormat, L"\u2191:");
        float speedW   = MeasureTextWidth(m_dwriteFactory, m_textFormat, refSpeed);
        float padX     = std::round(7.0f * scale);
        float arrowGap = std::round(4.0f * scale);

        // Two stacked lines tightly centered vertically
        float lineH = h * 0.46f;
        float topY  = (h - lineH * 2.0f) * 0.5f;

        float arrowLeft  = padX;
        float arrowRight = arrowLeft + arrowW + 2.0f;
        float speedLeft  = arrowLeft + arrowW + arrowGap;
        float speedRight = speedLeft + speedW;

        m_arrowFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);

        // Line 1: Upload (↑:)
        D2D1_RECT_F rcUpArrow = D2D1::RectF(arrowLeft, topY, arrowRight, topY + lineH);
        D2D1_RECT_F rcUpSpeed = D2D1::RectF(speedLeft, topY, speedRight, topY + lineH);
        m_renderTarget->DrawTextW(L"\u2191:", 2, m_arrowFormat, rcUpArrow, upBrush);
        m_renderTarget->DrawTextW(upText.c_str(), static_cast<UINT32>(upText.size()), m_textFormat, rcUpSpeed, textBrush);

        // Line 2: Download (↓:)
        D2D1_RECT_F rcDnArrow = D2D1::RectF(arrowLeft, topY + lineH, arrowRight, topY + lineH * 2.0f);
        D2D1_RECT_F rcDnSpeed = D2D1::RectF(speedLeft, topY + lineH, speedRight, topY + lineH * 2.0f);
        m_renderTarget->DrawTextW(L"\u2193:", 2, m_arrowFormat, rcDnArrow, downBrush);
        m_renderTarget->DrawTextW(downText.c_str(), static_cast<UINT32>(downText.size()), m_textFormat, rcDnSpeed, textBrush);

        // Optional Ping Indicator on the right
        if (m_cfg.showPing) {
            float divGap = std::round(7.0f * scale);
            float divX   = std::floor(speedRight + divGap) + 0.5f;

            if (sepBrush) {
                float sepPadY = std::round(7.0f * scale);
                m_renderTarget->DrawLine(
                    D2D1::Point2F(divX, sepPadY),
                    D2D1::Point2F(divX, h - sepPadY),
                    sepBrush, 1.0f
                );
            }

            // Choose status dot color
            D2D1_COLOR_F dotCol{};
            std::wstring pingStr;
            if (!m_ping.hasResult) {
                dotCol  = dark ? D2D1::ColorF(0.60f, 0.60f, 0.62f, 1.0f) : D2D1::ColorF(0.48f, 0.48f, 0.50f, 1.0f);
                pingStr = L"-- ms";
            } else if (!m_ping.reachable) {
                dotCol  = dark ? D2D1::ColorF(0.95f, 0.36f, 0.36f, 1.0f) : D2D1::ColorF(0.77f, 0.17f, 0.11f, 1.0f);
                pingStr = L"T/O";
            } else {
                pingStr = std::to_wstring(m_ping.rttMs) + L" ms";
                if (m_ping.rttMs < 50) {
                    dotCol = dark ? D2D1::ColorF(0.28f, 0.80f, 0.52f, 1.0f) : D2D1::ColorF(0.06f, 0.49f, 0.06f, 1.0f);
                } else if (m_ping.rttMs < 150) {
                    dotCol = dark ? D2D1::ColorF(0.96f, 0.72f, 0.28f, 1.0f) : D2D1::ColorF(0.76f, 0.49f, 0.00f, 1.0f);
                } else {
                    dotCol = dark ? D2D1::ColorF(0.95f, 0.36f, 0.36f, 1.0f) : D2D1::ColorF(0.77f, 0.17f, 0.11f, 1.0f);
                }
            }

            float dotRadius = 3.2f * scale;
            float dotCenterX = divX + divGap + dotRadius;
            float dotCenterY = h * 0.5f;

            ID2D1SolidColorBrush* dotBrush = nullptr;
            if (SUCCEEDED(m_renderTarget->CreateSolidColorBrush(dotCol, &dotBrush))) {
                m_renderTarget->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(dotCenterX, dotCenterY), dotRadius, dotRadius),
                    dotBrush
                );
                dotBrush->Release();
            }

            float pingTextLeft  = dotCenterX + dotRadius + std::round(5.0f * scale);
            float pingTextRight = w - padX;
            m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            D2D1_RECT_F rcPing = D2D1::RectF(pingTextLeft, 0.0f, pingTextRight, h);
            m_renderTarget->DrawTextW(pingStr.c_str(), static_cast<UINT32>(pingStr.size()), m_textFormat, rcPing, textBrush);
        }
    }

    SafeRelease(sepBrush);
    SafeRelease(downBrush);
    SafeRelease(upBrush);
    SafeRelease(textBrush);

    HRESULT hr = m_renderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        SafeRelease(m_renderTarget);
        return;
    }

    // Push premultiplied 32-bit BGRA surface to layered window
    HDC screenDC = GetDC(nullptr);
    POINT ptSrc{ 0, 0 };
    SIZE  sizeWnd{ m_width, m_height };
    BLENDFUNCTION blend{};
    blend.BlendOp             = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat         = AC_SRC_ALPHA;

    UpdateLayeredWindow(m_hwnd, screenDC, nullptr, &sizeWnd, m_memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screenDC);
}

void TaskbarWidget::UpdateTooltipText() {
    if (!m_tooltipHwnd || !IsWindow(m_tooltipHwnd)) return;

    std::wstring tip = L"Adapter: " + m_net.activeAdapterLabel + L"\r\n";
    tip += L"Upload: " + NetworkMonitor::FormatSpeed(m_net.uploadBytesPerSec, m_cfg.speedUnit)
         + L"  (Total: " + NetworkMonitor::FormatTotalBytes(m_net.sessionUpBytes) + L")\r\n";
    tip += L"Download: " + NetworkMonitor::FormatSpeed(m_net.downloadBytesPerSec, m_cfg.speedUnit)
         + L"  (Total: " + NetworkMonitor::FormatTotalBytes(m_net.sessionDownBytes) + L")";

    if (m_cfg.showPing) {
        tip += L"\r\nPing (" + m_cfg.pingHost + L"): ";
        if (!m_ping.hasResult) {
            tip += L"Measuring...";
        } else if (!m_ping.reachable) {
            tip += L"Timed out";
        } else {
            tip += std::to_wstring(m_ping.rttMs) + L" ms (min "
                 + std::to_wstring(m_ping.minRttMs) + L" / avg "
                 + std::to_wstring(m_ping.avgRttMs) + L" ms)";
        }
        if (m_ping.sent > 0 && m_ping.lost > 0) {
            double lossPct = 100.0 * static_cast<double>(m_ping.lost) / static_cast<double>(m_ping.sent);
            wchar_t buf[32]{};
            std::swprintf(buf, 32, L" [%.0f%% loss]", lossPct);
            tip += buf;
        }
    }
    tip += L"\r\nRight-click for menu \u2022 Shift+Drag to move";

    TTTOOLINFOW ti{};
    ti.cbSize   = sizeof(ti);
    ti.hwnd     = m_hwnd;
    ti.uId      = reinterpret_cast<UINT_PTR>(m_hwnd);
    ti.lpszText = const_cast<LPWSTR>(tip.c_str());
    SendMessageW(m_tooltipHwnd, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
}

LRESULT CALLBACK TaskbarWidget::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TaskbarWidget* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<TaskbarWidget*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TaskbarWidget*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TaskbarWidget::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_MOUSEMOVE: {
        if (!m_mouseHovering) {
            m_mouseHovering = true;
            TRACKMOUSEEVENT tme{};
            tme.cbSize    = sizeof(tme);
            tme.dwFlags   = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            Render();
        }
        if (m_dragging) {
            POINT pt{};
            GetCursorPos(&pt);
            int deltaScreenX = pt.x - m_dragStartPt.x;
            // Offset is distance LEFT of TrayNotifyWnd, so dragging right decreases offset
            int deltaLogical = MulDiv(deltaScreenX, 96, static_cast<int>(m_dpi));
            int newOffset = std::clamp(m_dragStartOffset - deltaLogical, -1000, 3000);
            if (newOffset != m_cfg.horizontalOffset) {
                m_cfg.horizontalOffset = newOffset;
                SyncWithTaskbar();
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        m_mouseHovering = false;
        Render();
        return 0;

    case WM_LBUTTONDOWN:
        if (GetKeyState(VK_SHIFT) & 0x8000) {
            m_dragging = true;
            GetCursorPos(&m_dragStartPt);
            m_dragStartOffset = m_cfg.horizontalOffset;
            SetCapture(hwnd);
            Render();
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (m_dragging) {
            m_dragging = false;
            ReleaseCapture();
            Render();
            if (m_callbacks.onOffsetChanged) {
                m_callbacks.onOffsetChanged(m_cfg.horizontalOffset);
            }
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
        if (m_dragging) {
            m_dragging = false;
            Render();
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        if (m_callbacks.onDoubleClick) {
            m_callbacks.onDoubleClick();
        }
        return 0;

    case WM_RBUTTONUP: {
        POINT pt{};
        GetCursorPos(&pt);
        if (m_callbacks.onContextMenu) {
            m_callbacks.onContextMenu(pt);
        }
        return 0;
    }

    case WM_DESTROY:
        m_hwnd          = nullptr;
        m_attachedChild = false;
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
