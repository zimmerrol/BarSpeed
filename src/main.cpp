#include "config.h"
#include "net_monitor.h"
#include "ping_monitor.h"
#include "resource.h"
#include "settings_dialog.h"
#include "taskbar_widget.h"
#include "tray_icon.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>

#include <string>
#include <vector>

namespace {

constexpr wchar_t kHostClassName[] = L"BarSpeed_HostWindow";
constexpr wchar_t kMutexName[]     = L"Local\\BarSpeed_SingleInstance_Mutex";

class AppController {
public:
    bool Init(HINSTANCE hInst) {
        m_hInst = hInst;
        m_cfg   = ConfigStore::Load();
        m_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = AppController::HostWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = kHostClassName;
        RegisterClassExW(&wc);

        // Hidden top-level window (NOT HWND_MESSAGE, so it receives TaskbarCreated & WM_SETTINGCHANGE!)
        m_hostHwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            kHostClassName,
            L"BarSpeedHost",
            WS_POPUP,
            0, 0, 0, 0,
            nullptr, nullptr, hInst, this
        );
        if (!m_hostHwnd) return false;

        InitWidget();
        SyncTrayIcon();

        // Take initial baseline sample immediately
        m_lastNet = m_netMonitor.Sample(m_cfg.adapterName);
        m_pingMonitor.Start(m_hostHwnd, WM_APP_PING_UPDATE, m_cfg.showPing, m_cfg.pingHost, m_cfg.pingIntervalMs);
        m_lastPing = m_pingMonitor.GetStatus();
        m_widget.UpdateData(m_lastNet, m_lastPing);

        SetTimer(m_hostHwnd, TIMER_ID_NET_SAMPLE, static_cast<UINT>(m_cfg.updateIntervalMs), nullptr);
        SetTimer(m_hostHwnd, TIMER_ID_TASKBAR_SYNC, 500, nullptr);
        return true;
    }

    int RunMessageLoop() {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (m_settings.IsOpen() && IsDialogMessageW(m_settings.GetHwnd(), &msg)) {
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

    void Shutdown() {
        m_pingMonitor.Stop();
        m_settings.Close();
        m_tray.Hide();
        m_widget.Destroy();
        if (m_hostHwnd && IsWindow(m_hostHwnd)) {
            KillTimer(m_hostHwnd, TIMER_ID_NET_SAMPLE);
            KillTimer(m_hostHwnd, TIMER_ID_TASKBAR_SYNC);
            DestroyWindow(m_hostHwnd);
        }
        m_hostHwnd = nullptr;
    }

private:
    static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        AppController* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<AppController*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<AppController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (self) {
            return self->HandleHostMessage(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void InitWidget() {
        TaskbarWidget::Callbacks cb;
        cb.onContextMenu = [this](POINT pt) { ShowContextMenu(pt); };
        cb.onDoubleClick = [this]() { OpenSettings(); };
        cb.onOffsetChanged = [this](int newOffset) {
            m_cfg.horizontalOffset = newOffset;
            ConfigStore::Save(m_cfg);
        };
        m_widget.Initialize(m_hInst, m_cfg, std::move(cb));
    }

    void SyncTrayIcon() {
        if (m_cfg.showTrayIcon) {
            if (!m_tray.IsVisible()) {
                m_tray.Show(m_hostHwnd, WM_APP_TRAY_ICON, BuildTrayTooltip());
            } else {
                m_tray.UpdateTooltip(BuildTrayTooltip());
            }
        } else {
            m_tray.Hide();
        }
    }

    std::wstring BuildTrayTooltip() const {
        std::wstring tip = L"BarSpeed\n";
        tip += L"\u2191 " + NetworkMonitor::FormatSpeed(m_lastNet.uploadBytesPerSec, m_cfg.speedUnit)
             + L"  \u2193 " + NetworkMonitor::FormatSpeed(m_lastNet.downloadBytesPerSec, m_cfg.speedUnit);
        if (m_cfg.showPing) {
            tip += L"\nPing (" + m_cfg.pingHost + L"): ";
            if (!m_lastPing.hasResult) tip += L"--";
            else if (!m_lastPing.reachable) tip += L"Timeout";
            else tip += std::to_wstring(m_lastPing.rttMs) + L" ms";
        }
        return tip;
    }

    void ApplyNewConfig(const AppConfig& newCfg) {
        bool intervalChanged = (newCfg.updateIntervalMs != m_cfg.updateIntervalMs);
        bool adapterChanged  = (newCfg.adapterName != m_cfg.adapterName);
        m_cfg = newCfg;
        m_cfg.Clamp();
        ConfigStore::Save(m_cfg);

        if (adapterChanged) {
            m_netMonitor.Reset();
            m_lastNet = m_netMonitor.Sample(m_cfg.adapterName);
        }
        if (intervalChanged && m_hostHwnd) {
            SetTimer(m_hostHwnd, TIMER_ID_NET_SAMPLE, static_cast<UINT>(m_cfg.updateIntervalMs), nullptr);
        }

        m_pingMonitor.Configure(m_cfg.showPing, m_cfg.pingHost, m_cfg.pingIntervalMs);
        m_lastPing = m_pingMonitor.GetStatus();

        m_widget.ApplyConfig(m_cfg);
        m_widget.UpdateData(m_lastNet, m_lastPing);
        SyncTrayIcon();
    }

    void OpenSettings() {
        m_settings.Show(m_hInst, m_cfg, ConfigStore::IsAutoStartEnabled(),
            [this](const AppConfig& newCfg, bool autoStart) {
                ConfigStore::SetAutoStartEnabled(autoStart);
                ApplyNewConfig(newCfg);
            });
    }

    void ShowContextMenu(POINT screenPt) {
        HMENU hMenu = CreatePopupMenu();
        if (!hMenu) return;

        // Ping toggle
        std::wstring pingLabel = L"Show Ping (" + m_cfg.pingHost + L")";
        AppendMenuW(hMenu, MF_STRING | (m_cfg.showPing ? MF_CHECKED : MF_UNCHECKED),
                    IDM_SHOW_PING, pingLabel.c_str());

        // Unit submenu
        HMENU hUnitMenu = CreatePopupMenu();
        AppendMenuW(hUnitMenu, MF_STRING | (m_cfg.speedUnit == SpeedUnit::BytesPerSec ? MF_CHECKED : MF_UNCHECKED),
                    IDM_UNIT_BYTES, L"Bytes/s (KB/s, MB/s)");
        AppendMenuW(hUnitMenu, MF_STRING | (m_cfg.speedUnit == SpeedUnit::BitsPerSec ? MF_CHECKED : MF_UNCHECKED),
                    IDM_UNIT_BITS, L"Bits/s (Kbps, Mbps)");
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hUnitMenu), L"Speed Unit");

        // Adapter submenu
        m_menuAdapters = NetworkMonitor::EnumerateAdapters();
        HMENU hAdapterMenu = CreatePopupMenu();
        AppendMenuW(hAdapterMenu, MF_STRING | (m_cfg.adapterName.empty() ? MF_CHECKED : MF_UNCHECKED),
                    IDM_ADAPTER_AUTO, L"Auto (All Active Physical)");
        if (!m_menuAdapters.empty()) {
            AppendMenuW(hAdapterMenu, MF_SEPARATOR, 0, nullptr);
            size_t count = std::min<size_t>(m_menuAdapters.size(), IDM_ADAPTER_MAX - IDM_ADAPTER_BASE);
            for (size_t i = 0; i < count; ++i) {
                bool isSel = (!m_cfg.adapterName.empty() && m_menuAdapters[i].alias == m_cfg.adapterName);
                AppendMenuW(hAdapterMenu, MF_STRING | (isSel ? MF_CHECKED : MF_UNCHECKED),
                            IDM_ADAPTER_BASE + static_cast<UINT>(i), m_menuAdapters[i].alias.c_str());
            }
        }
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hAdapterMenu), L"Network Adapter");

        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_RESET_POSITION, L"Reset Taskbar Position");
        AppendMenuW(hMenu, MF_STRING | (ConfigStore::IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED),
                    IDM_START_WINDOWS, L"Start with Windows");
        AppendMenuW(hMenu, MF_STRING, IDM_OPEN_SETTINGS, L"Settings...");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

        SetForegroundWindow(m_hostHwnd);
        TrackPopupMenuEx(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, screenPt.x, screenPt.y, m_hostHwnd, nullptr);
        PostMessageW(m_hostHwnd, WM_NULL, 0, 0);

        DestroyMenu(hMenu);
    }

    LRESULT HandleHostMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (m_taskbarCreatedMsg != 0 && msg == m_taskbarCreatedMsg) {
            // Explorer restarted: recreate widget and re-register tray icon
            InitWidget();
            m_widget.UpdateData(m_lastNet, m_lastPing);
            m_tray.Hide();
            SyncTrayIcon();
            return 0;
        }

        switch (msg) {
        case WM_TIMER:
            if (wParam == TIMER_ID_NET_SAMPLE) {
                m_lastNet = m_netMonitor.Sample(m_cfg.adapterName);
                m_widget.UpdateData(m_lastNet, m_lastPing);
                if (m_tray.IsVisible()) {
                    m_tray.UpdateTooltip(BuildTrayTooltip());
                }
            } else if (wParam == TIMER_ID_TASKBAR_SYNC) {
                if (!m_widget.IsAlive()) {
                    InitWidget();
                    m_widget.UpdateData(m_lastNet, m_lastPing);
                } else {
                    m_widget.SyncWithTaskbar();
                }
            }
            return 0;

        case WM_APP_PING_UPDATE:
            m_lastPing = m_pingMonitor.GetStatus();
            m_widget.UpdateData(m_lastNet, m_lastPing);
            if (m_tray.IsVisible()) {
                m_tray.UpdateTooltip(BuildTrayTooltip());
            }
            return 0;

        case WM_APP_TRAY_ICON: {
            UINT event = LOWORD(lParam);
            if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
                POINT pt{};
                GetCursorPos(&pt);
                ShowContextMenu(pt);
            } else if (event == WM_LBUTTONDBLCLK) {
                OpenSettings();
            }
            return 0;
        }

        case WM_APP_ACTIVATE_EXISTING:
            if (!m_widget.IsAlive()) {
                InitWidget();
            } else {
                m_widget.SyncWithTaskbar();
            }
            m_widget.UpdateData(m_lastNet, m_lastPing);
            OpenSettings();
            return 0;

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            m_widget.RefreshTheme();
            m_widget.SyncWithTaskbar();
            return 0;

        case WM_DISPLAYCHANGE:
            m_widget.SyncWithTaskbar();
            return 0;

        case WM_COMMAND: {
            UINT id = LOWORD(wParam);
            if (id == IDM_SHOW_PING) {
                AppConfig cfg = m_cfg;
                cfg.showPing = !cfg.showPing;
                ApplyNewConfig(cfg);
                return 0;
            }
            if (id == IDM_UNIT_BYTES || id == IDM_UNIT_BITS) {
                AppConfig cfg = m_cfg;
                cfg.speedUnit = (id == IDM_UNIT_BITS) ? SpeedUnit::BitsPerSec : SpeedUnit::BytesPerSec;
                ApplyNewConfig(cfg);
                return 0;
            }
            if (id == IDM_ADAPTER_AUTO) {
                AppConfig cfg = m_cfg;
                cfg.adapterName.clear();
                ApplyNewConfig(cfg);
                return 0;
            }
            if (id >= IDM_ADAPTER_BASE && id <= IDM_ADAPTER_MAX) {
                size_t idx = id - IDM_ADAPTER_BASE;
                if (idx < m_menuAdapters.size()) {
                    AppConfig cfg = m_cfg;
                    cfg.adapterName = m_menuAdapters[idx].alias;
                    ApplyNewConfig(cfg);
                }
                return 0;
            }
            if (id == IDM_RESET_POSITION) {
                AppConfig cfg = m_cfg;
                cfg.horizontalOffset = 8;
                ApplyNewConfig(cfg);
                InitWidget();
                m_widget.UpdateData(m_lastNet, m_lastPing);
                return 0;
            }
            if (id == IDM_START_WINDOWS) {
                ConfigStore::SetAutoStartEnabled(!ConfigStore::IsAutoStartEnabled());
                return 0;
            }
            if (id == IDM_OPEN_SETTINGS) {
                OpenSettings();
                return 0;
            }
            if (id == IDM_EXIT) {
                PostQuitMessage(0);
                return 0;
            }
            break;
        }

        case WM_ENDSESSION:
            if (wParam) {
                Shutdown();
            }
            return 0;

        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    HINSTANCE      m_hInst = nullptr;
    HWND           m_hostHwnd = nullptr;
    UINT           m_taskbarCreatedMsg = 0;

    AppConfig      m_cfg;
    NetworkMonitor m_netMonitor;
    PingMonitor    m_pingMonitor;
    TaskbarWidget  m_widget;
    TrayIcon       m_tray;
    SettingsDialog m_settings;

    NetworkSample  m_lastNet;
    PingStatus     m_lastPing;
    std::vector<AdapterInfo> m_menuAdapters;
};

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // Ensure only one instance runs per user session; if already running, bring up Settings & re-sync widget
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kHostClassName, nullptr);
        if (existing) {
            PostMessageW(existing, WM_APP_ACTIVATE_EXISTING, 0, 0);
        }
        CloseHandle(hMutex);
        return 0;
    }

    // Fallback DPI awareness call (manifest also declares PerMonitorV2)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    AppController app;
    int exitCode = 1;
    if (app.Init(hInstance)) {
        exitCode = app.RunMessageLoop();
        app.Shutdown();
    }

    CoUninitialize();
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return exitCode;
}
