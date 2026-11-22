#pragma once

#include "config.h"
#include "net_monitor.h"
#include "ping_monitor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <functional>
#include <string>

class TaskbarWidget {
public:
    struct Callbacks {
        std::function<void(POINT screenPt)> onContextMenu;
        std::function<void()>               onDoubleClick;
        std::function<void(int newOffset)>  onOffsetChanged;
    };

    TaskbarWidget();
    ~TaskbarWidget();

    TaskbarWidget(const TaskbarWidget&) = delete;
    TaskbarWidget& operator=(const TaskbarWidget&) = delete;

    bool Initialize(HINSTANCE hInst, const AppConfig& cfg, Callbacks callbacks);
    void Destroy();

    void ApplyConfig(const AppConfig& cfg);
    void UpdateData(const NetworkSample& net, const PingStatus& ping);
    void SyncWithTaskbar();
    void RefreshTheme();

    bool IsAlive() const { return m_hwnd != nullptr && IsWindow(m_hwnd); }
    HWND GetHwnd() const { return m_hwnd; }

    // Checks Windows 11 Registry SystemUsesLightTheme (returns true if taskbar is dark)
    static bool IsSystemTaskbarDark();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool CreateWidgetWindow(HINSTANCE hInst);
    bool EnsureD2DResources();
    void DiscardD2DResources();
    void EnsureBackBuffer(int width, int height);
    void ReleaseBackBuffer();
    void RebuildTextFormat();
    void ComputeDesiredSize();
    void Render();
    void UpdateTooltipText();
    bool ResolveUseDarkTaskbar() const;

    HINSTANCE m_hInst          = nullptr;
    HWND      m_hwnd           = nullptr;
    HWND      m_tooltipHwnd    = nullptr;
    HWND      m_taskbarHwnd    = nullptr;
    bool      m_attachedChild  = false;
    bool      m_mouseHovering  = false;
    bool      m_dragging       = false;
    POINT     m_dragStartPt{};
    int       m_dragStartOffset = 0;

    UINT      m_dpi            = 96;
    int       m_width          = 110;
    int       m_height         = 40;
    int       m_lastX          = -99999;
    int       m_lastY          = -99999;
    bool      m_systemDark     = true;

    AppConfig     m_cfg;
    NetworkSample m_net;
    PingStatus    m_ping;
    Callbacks     m_callbacks;

    // Direct2D / DirectWrite resources
    ID2D1Factory*          m_d2dFactory    = nullptr;
    IDWriteFactory*        m_dwriteFactory = nullptr;
    ID2D1DCRenderTarget*   m_renderTarget  = nullptr;
    IDWriteTextFormat*     m_textFormat    = nullptr;
    IDWriteTextFormat*     m_arrowFormat   = nullptr;

    // 32-bit premultiplied BGRA DIB backing buffer for UpdateLayeredWindow
    HDC     m_memDC    = nullptr;
    HBITMAP m_dibBmp   = nullptr;
    HBITMAP m_oldBmp   = nullptr;
    int     m_bufW     = 0;
    int     m_bufH     = 0;
};
