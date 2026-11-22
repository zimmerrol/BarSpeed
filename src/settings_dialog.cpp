#include "settings_dialog.h"
#include "resource.h"
#include "tray_icon.h"

#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <string>

namespace {

constexpr wchar_t kSettingsClassName[] = L"BarSpeed_SettingsWindow";

std::wstring GetControlText(HWND hCtrl) {
    int len = GetWindowTextLengthW(hCtrl);
    if (len <= 0) return {};
    std::wstring text(static_cast<size_t>(len + 1), L'\0');
    GetWindowTextW(hCtrl, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

} // namespace

SettingsDialog::~SettingsDialog() {
    Close();
    if (m_icon) {
        DestroyIcon(m_icon);
        m_icon = nullptr;
    }
}

int SettingsDialog::Scale(int logicalPx) const {
    return MulDiv(logicalPx, static_cast<int>(m_dpi), 96);
}

void SettingsDialog::Show(HINSTANCE hInst, const AppConfig& currentCfg, bool currentAutoStart, ApplyCallback onApply) {
    if (IsOpen()) {
        SetForegroundWindow(m_hwnd);
        return;
    }

    m_initialCfg       = currentCfg;
    m_initialAutoStart = currentAutoStart;
    m_onApply          = std::move(onApply);
    m_adapters         = NetworkMonitor::EnumerateAdapters();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = SettingsDialog::WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kSettingsClassName;
    if (!m_icon) {
        m_icon = TrayIcon::CreateAppIcon(32);
    }
    wc.hIcon   = m_icon;
    wc.hIconSm = m_icon;
    RegisterClassExW(&wc);

    // Determine DPI of monitor near cursor so initial sizing is exact
    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    UINT dpiX = 96, dpiY = 96;
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"));
        if (fn && SUCCEEDED(fn(hMon, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY))) {
            m_dpi = dpiX;
        }
        FreeLibrary(shcore);
    }

    const int clientW = Scale(430);
    const int clientH = Scale(450);

    DWORD style   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;

    RECT rc{ 0, 0, clientW, clientH };
    AdjustWindowRectExForDpi(&rc, style, FALSE, exStyle, m_dpi);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - winW) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - winH) / 2;

    m_hwnd = CreateWindowExW(
        exStyle,
        kSettingsClassName,
        L"BarSpeed Settings",
        style,
        x, y, winW, winH,
        nullptr, nullptr, hInst, this
    );

    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_SHOW);
        UpdateWindow(m_hwnd);
        SetForegroundWindow(m_hwnd);
    }
}

void SettingsDialog::Close() {
    if (m_hwnd && IsWindow(m_hwnd)) {
        DestroyWindow(m_hwnd);
    }
    m_hwnd = nullptr;
}

LRESULT CALLBACK SettingsDialog::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsDialog* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT SettingsDialog::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        m_dpi = GetDpiForWindow(hwnd);
        if (m_dpi == 0) m_dpi = 96;

        // Clean Segoe UI font scaled to current DPI
        LOGFONTW lf{};
        lf.lfHeight  = -MulDiv(9, static_cast<int>(m_dpi), 72);
        lf.lfWeight  = FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        m_font = CreateFontIndirectW(&lf);

        lf.lfWeight = FW_SEMIBOLD;
        m_boldFont = CreateFontIndirectW(&lf);

        CreateControls(hwnd);
        PopulateValues();
        return 0;
    }

    case WM_COMMAND: {
        int id   = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_CHECK_SHOW_PING && code == BN_CLICKED) {
            UpdatePingControlsEnabled();
            return 0;
        }
        if (id == IDC_BTN_APPLY || id == IDC_BTN_OK || id == IDOK) {
            bool autoStart = false;
            AppConfig cfg = ReadConfigFromControls(autoStart);
            if (m_onApply) {
                m_onApply(cfg, autoStart);
            }
            if (id == IDC_BTN_OK || id == IDOK) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        if (id == IDC_BTN_CANCEL || id == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (m_font) {
            DeleteObject(m_font);
            m_font = nullptr;
        }
        if (m_boldFont) {
            DeleteObject(m_boldFont);
            m_boldFont = nullptr;
        }
        m_hwnd = nullptr;
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void SettingsDialog::CreateControls(HWND hwnd) {
    auto makeCtrl = [&](const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle,
                        int x, int y, int w, int h, int id, bool bold = false) -> HWND {
        HWND hCtrl = CreateWindowExW(
            exStyle, cls, text, style | WS_CHILD | WS_VISIBLE,
            Scale(x), Scale(y), Scale(w), Scale(h),
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr
        );
        if (hCtrl) {
            SendMessageW(hCtrl, WM_SETFONT, reinterpret_cast<WPARAM>(bold ? m_boldFont : m_font), TRUE);
        }
        return hCtrl;
    };

    const int leftX   = 16;
    const int labelX  = 28;
    const int labelW  = 136;
    const int ctrlX   = 168;
    const int ctrlW   = 234;
    const int groupW  = 398;

    // --- Group 1: Network ---
    makeCtrl(L"BUTTON", L"Network", BS_GROUPBOX, 0, leftX, 12, groupW, 118, 0, true);

    makeCtrl(L"STATIC", L"Network Adapter:", SS_LEFT | SS_CENTERIMAGE, 0, labelX, 38, labelW, 22, 0);
    makeCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, ctrlX, 36, ctrlW, 200, IDC_COMBO_ADAPTER);

    makeCtrl(L"STATIC", L"Speed Unit:", SS_LEFT | SS_CENTERIMAGE, 0, labelX, 68, labelW, 22, 0);
    makeCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, ctrlX, 66, ctrlW, 120, IDC_COMBO_UNIT);

    makeCtrl(L"STATIC", L"Update Interval:", SS_LEFT | SS_CENTERIMAGE, 0, labelX, 98, labelW, 22, 0);
    makeCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, ctrlX, 96, ctrlW, 120, IDC_COMBO_UPDATE_INTERVAL);

    // --- Group 2: Ping Indicator ---
    makeCtrl(L"BUTTON", L"Ping Indicator", BS_GROUPBOX, 0, leftX, 140, groupW, 114, 0, true);

    makeCtrl(L"BUTTON", L"Show Ping Indicator in Taskbar", BS_AUTOCHECKBOX | WS_TABSTOP, 0,
             labelX, 164, 340, 22, IDC_CHECK_SHOW_PING);

    makeCtrl(L"STATIC", L"Target Host / IP:", SS_LEFT | SS_CENTERIMAGE, 0, labelX, 192, labelW, 22, 0);
    makeCtrl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ctrlX, 190, ctrlW, 24, IDC_EDIT_PING_HOST);

    makeCtrl(L"STATIC", L"Ping Interval:", SS_LEFT | SS_CENTERIMAGE, 0, labelX, 222, labelW, 22, 0);
    makeCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, ctrlX, 220, ctrlW, 120, IDC_COMBO_PING_INTERVAL);

    // --- Group 3: Taskbar Appearance & Startup ---
    makeCtrl(L"BUTTON", L"Appearance & Behavior", BS_GROUPBOX, 0, leftX, 264, groupW, 138, 0, true);

    makeCtrl(L"STATIC", L"Font Size:", SS_LEFT | SS_CENTERIMAGE, 0, labelX, 288, labelW, 22, 0);
    makeCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, ctrlX, 286, 100, 140, IDC_COMBO_FONT_SIZE);

    makeCtrl(L"STATIC", L"Text Theme:", SS_LEFT | SS_CENTERIMAGE, 0, labelX, 318, labelW, 22, 0);
    makeCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, ctrlX, 316, ctrlW, 120, IDC_COMBO_THEME);

    makeCtrl(L"STATIC", L"Horizontal Offset (px):", SS_LEFT | SS_CENTERIMAGE, 0, labelX, 348, labelW, 22, 0);
    makeCtrl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ctrlX, 346, 72, 24, IDC_EDIT_OFFSET_X);
    makeCtrl(L"STATIC", L"(Shift + drag widget)", SS_LEFT | SS_CENTERIMAGE, 0, ctrlX + 80, 348, 155, 22, 0);

    makeCtrl(L"BUTTON", L"Show System Tray Icon", BS_AUTOCHECKBOX | WS_TABSTOP, 0,
             labelX, 376, 180, 20, IDC_CHECK_TRAY_ICON);
    makeCtrl(L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX | WS_TABSTOP, 0,
             220, 376, 175, 20, IDC_CHECK_AUTOSTART);

    // --- Buttons ---
    makeCtrl(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 166, 412, 78, 26, IDC_BTN_OK);
    makeCtrl(L"BUTTON", L"Apply", BS_PUSHBUTTON | WS_TABSTOP, 0, 250, 412, 78, 26, IDC_BTN_APPLY);
    makeCtrl(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0, 334, 412, 78, 26, IDC_BTN_CANCEL);
}

void SettingsDialog::PopulateValues() {
    // Adapter combo
    HWND hAdapter = GetDlgItem(m_hwnd, IDC_COMBO_ADAPTER);
    SendMessageW(hAdapter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto (All Active Physical)"));
    int selectedAdapterIdx = 0;
    for (size_t i = 0; i < m_adapters.size(); ++i) {
        std::wstring label = m_adapters[i].alias;
        if (!m_adapters[i].description.empty()) {
            label += L"  -  " + m_adapters[i].description;
        }
        SendMessageW(hAdapter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (!m_initialCfg.adapterName.empty() && m_adapters[i].alias == m_initialCfg.adapterName) {
            selectedAdapterIdx = static_cast<int>(i + 1);
        }
    }
    SendMessageW(hAdapter, CB_SETCURSEL, selectedAdapterIdx, 0);

    // Unit combo
    HWND hUnit = GetDlgItem(m_hwnd, IDC_COMBO_UNIT);
    SendMessageW(hUnit, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bytes/s (KB/s, MB/s)"));
    SendMessageW(hUnit, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bits/s (Kbps, Mbps)"));
    SendMessageW(hUnit, CB_SETCURSEL, m_initialCfg.speedUnit == SpeedUnit::BitsPerSec ? 1 : 0, 0);

    // Update interval combo
    HWND hUpd = GetDlgItem(m_hwnd, IDC_COMBO_UPDATE_INTERVAL);
    SendMessageW(hUpd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"500 ms"));
    SendMessageW(hUpd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1000 ms (1 s)"));
    SendMessageW(hUpd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2000 ms (2 s)"));
    int updIdx = 1;
    if (m_initialCfg.updateIntervalMs <= 600) updIdx = 0;
    else if (m_initialCfg.updateIntervalMs >= 1500) updIdx = 2;
    SendMessageW(hUpd, CB_SETCURSEL, updIdx, 0);

    // Ping controls
    SendMessageW(GetDlgItem(m_hwnd, IDC_CHECK_SHOW_PING), BM_SETCHECK,
                 m_initialCfg.showPing ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(GetDlgItem(m_hwnd, IDC_EDIT_PING_HOST), m_initialCfg.pingHost.c_str());

    HWND hPingInt = GetDlgItem(m_hwnd, IDC_COMBO_PING_INTERVAL);
    SendMessageW(hPingInt, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1 second"));
    SendMessageW(hPingInt, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2 seconds"));
    SendMessageW(hPingInt, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"3 seconds"));
    SendMessageW(hPingInt, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"5 seconds"));
    int pingIdx = 1;
    if (m_initialCfg.pingIntervalMs <= 1200) pingIdx = 0;
    else if (m_initialCfg.pingIntervalMs <= 2500) pingIdx = 1;
    else if (m_initialCfg.pingIntervalMs <= 4000) pingIdx = 2;
    else pingIdx = 3;
    SendMessageW(hPingInt, CB_SETCURSEL, pingIdx, 0);
    UpdatePingControlsEnabled();

    // Font size combo (8..12)
    HWND hFont = GetDlgItem(m_hwnd, IDC_COMBO_FONT_SIZE);
    for (int pt = 8; pt <= 12; ++pt) {
        std::wstring s = std::to_wstring(pt) + L" pt";
        SendMessageW(hFont, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
    }
    int fontIdx = std::clamp(m_initialCfg.fontSizePt - 8, 0, 4);
    SendMessageW(hFont, CB_SETCURSEL, fontIdx, 0);

    // Theme combo
    HWND hTheme = GetDlgItem(m_hwnd, IDC_COMBO_THEME);
    SendMessageW(hTheme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto (Follow Windows Taskbar)"));
    SendMessageW(hTheme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Light Text (Dark Taskbar)"));
    SendMessageW(hTheme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Dark Text (Light Taskbar)"));
    SendMessageW(hTheme, CB_SETCURSEL, static_cast<int>(m_initialCfg.themeMode), 0);

    // Offset + checkboxes
    SetWindowTextW(GetDlgItem(m_hwnd, IDC_EDIT_OFFSET_X), std::to_wstring(m_initialCfg.horizontalOffset).c_str());
    SendMessageW(GetDlgItem(m_hwnd, IDC_CHECK_TRAY_ICON), BM_SETCHECK,
                 m_initialCfg.showTrayIcon ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(m_hwnd, IDC_CHECK_AUTOSTART), BM_SETCHECK,
                 m_initialAutoStart ? BST_CHECKED : BST_UNCHECKED, 0);
}

void SettingsDialog::UpdatePingControlsEnabled() {
    BOOL enabled = (SendMessageW(GetDlgItem(m_hwnd, IDC_CHECK_SHOW_PING), BM_GETCHECK, 0, 0) == BST_CHECKED);
    EnableWindow(GetDlgItem(m_hwnd, IDC_EDIT_PING_HOST), enabled);
    EnableWindow(GetDlgItem(m_hwnd, IDC_COMBO_PING_INTERVAL), enabled);
}

AppConfig SettingsDialog::ReadConfigFromControls(bool& outAutoStart) const {
    AppConfig cfg = m_initialCfg;

    int adapterIdx = static_cast<int>(SendMessageW(GetDlgItem(m_hwnd, IDC_COMBO_ADAPTER), CB_GETCURSEL, 0, 0));
    if (adapterIdx <= 0 || static_cast<size_t>(adapterIdx - 1) >= m_adapters.size()) {
        cfg.adapterName.clear();
    } else {
        cfg.adapterName = m_adapters[static_cast<size_t>(adapterIdx - 1)].alias;
    }

    int unitIdx = static_cast<int>(SendMessageW(GetDlgItem(m_hwnd, IDC_COMBO_UNIT), CB_GETCURSEL, 0, 0));
    cfg.speedUnit = (unitIdx == 1) ? SpeedUnit::BitsPerSec : SpeedUnit::BytesPerSec;

    int updIdx = static_cast<int>(SendMessageW(GetDlgItem(m_hwnd, IDC_COMBO_UPDATE_INTERVAL), CB_GETCURSEL, 0, 0));
    constexpr int kUpdateChoices[] = { 500, 1000, 2000 };
    cfg.updateIntervalMs = kUpdateChoices[std::clamp(updIdx, 0, 2)];

    cfg.showPing = (SendMessageW(GetDlgItem(m_hwnd, IDC_CHECK_SHOW_PING), BM_GETCHECK, 0, 0) == BST_CHECKED);
    std::wstring host = GetControlText(GetDlgItem(m_hwnd, IDC_EDIT_PING_HOST));
    // Trim whitespace
    while (!host.empty() && (host.front() == L' ' || host.front() == L'\t')) host.erase(host.begin());
    while (!host.empty() && (host.back() == L' ' || host.back() == L'\t')) host.pop_back();
    cfg.pingHost = host.empty() ? L"8.8.8.8" : host;

    int pingIdx = static_cast<int>(SendMessageW(GetDlgItem(m_hwnd, IDC_COMBO_PING_INTERVAL), CB_GETCURSEL, 0, 0));
    constexpr int kPingChoices[] = { 1000, 2000, 3000, 5000 };
    cfg.pingIntervalMs = kPingChoices[std::clamp(pingIdx, 0, 3)];

    int fontIdx = static_cast<int>(SendMessageW(GetDlgItem(m_hwnd, IDC_COMBO_FONT_SIZE), CB_GETCURSEL, 0, 0));
    cfg.fontSizePt = 8 + std::clamp(fontIdx, 0, 4);

    int themeIdx = static_cast<int>(SendMessageW(GetDlgItem(m_hwnd, IDC_COMBO_THEME), CB_GETCURSEL, 0, 0));
    cfg.themeMode = static_cast<ThemeMode>(std::clamp(themeIdx, 0, 2));

    std::wstring offsetStr = GetControlText(GetDlgItem(m_hwnd, IDC_EDIT_OFFSET_X));
    try {
        cfg.horizontalOffset = std::stoi(offsetStr);
    } catch (...) {
        cfg.horizontalOffset = m_initialCfg.horizontalOffset;
    }

    cfg.showTrayIcon   = (SendMessageW(GetDlgItem(m_hwnd, IDC_CHECK_TRAY_ICON), BM_GETCHECK, 0, 0) == BST_CHECKED);
    outAutoStart       = (SendMessageW(GetDlgItem(m_hwnd, IDC_CHECK_AUTOSTART), BM_GETCHECK, 0, 0) == BST_CHECKED);

    cfg.Clamp();
    return cfg;
}
