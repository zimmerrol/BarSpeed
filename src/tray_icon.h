#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <string>

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // Creates a clean programmatic 32-bit ARGB icon (up/down network arrows).
    static HICON CreateAppIcon(int sizePx = 32);

    bool Show(HWND ownerHwnd, UINT callbackMsg, const std::wstring& tooltip);
    void UpdateTooltip(const std::wstring& tooltip);
    void Hide();
    bool IsVisible() const { return m_visible; }

private:
    HWND  m_ownerHwnd   = nullptr;
    UINT  m_callbackMsg = 0;
    HICON m_icon        = nullptr;
    bool  m_visible     = false;
};
