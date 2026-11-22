#pragma once

#include "config.h"
#include "net_monitor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <functional>
#include <vector>

class SettingsDialog {
public:
    using ApplyCallback = std::function<void(const AppConfig& newCfg, bool autoStart)>;

    SettingsDialog() = default;
    ~SettingsDialog();

    SettingsDialog(const SettingsDialog&) = delete;
    SettingsDialog& operator=(const SettingsDialog&) = delete;

    void Show(HINSTANCE hInst, const AppConfig& currentCfg, bool currentAutoStart, ApplyCallback onApply);
    void Close();
    bool IsOpen() const { return m_hwnd != nullptr && IsWindow(m_hwnd); }
    HWND GetHwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void CreateControls(HWND hwnd);
    void PopulateValues();
    void UpdatePingControlsEnabled();
    AppConfig ReadConfigFromControls(bool& outAutoStart) const;
    int Scale(int logicalPx) const;

    HWND                     m_hwnd = nullptr;
    HFONT                    m_font = nullptr;
    HFONT                    m_boldFont = nullptr;
    HICON                    m_icon = nullptr;
    UINT                     m_dpi = 96;
    AppConfig                m_initialCfg;
    bool                     m_initialAutoStart = false;
    std::vector<AdapterInfo> m_adapters;
    ApplyCallback            m_onApply;
};
