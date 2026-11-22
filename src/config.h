#pragma once

#include <string>

enum class SpeedUnit : int {
    BytesPerSec = 0, // B/s, KB/s, MB/s, GB/s
    BitsPerSec  = 1, // bps, Kbps, Mbps, Gbps
};

enum class ThemeMode : int {
    Auto         = 0, // Follow Windows 11 SystemUsesLightTheme
    DarkTaskbar  = 1, // Light text for dark taskbar
    LightTaskbar = 2, // Dark text for light taskbar
};

struct AppConfig {
    std::wstring adapterName;              // Empty = Auto (all active hardware NICs)
    SpeedUnit    speedUnit        = SpeedUnit::BytesPerSec;
    int          updateIntervalMs = 1000;  // 500, 1000, 2000
    bool         showPing         = true;
    std::wstring pingHost         = L"8.8.8.8";
    int          pingIntervalMs   = 2000;  // 1000, 2000, 3000, 5000
    int          fontSizePt       = 9;     // 8..12
    ThemeMode    themeMode        = ThemeMode::Auto;
    int          horizontalOffset = 8;     // Logical px (at 96 DPI) left of TrayNotifyWnd
    bool         showTrayIcon     = true;

    void Clamp();
};

class ConfigStore {
public:
    static std::wstring GetDefaultConfigPath();
    static AppConfig Load(const std::wstring& path = GetDefaultConfigPath());
    static bool Save(const AppConfig& cfg, const std::wstring& path = GetDefaultConfigPath());

    static std::string Serialize(const AppConfig& cfg);
    static AppConfig Deserialize(const std::string& content);

    static bool IsAutoStartEnabled();
    static bool SetAutoStartEnabled(bool enable);
};

// Utility string conversion helpers
std::string WideToUtf8(const std::wstring& wide);
std::wstring Utf8ToWide(const std::string& utf8);
