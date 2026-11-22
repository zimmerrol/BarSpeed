#include "config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace {

constexpr wchar_t kRunRegPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"BarSpeed";

std::string Trim(std::string s) {
    auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

bool ParseBool(const std::string& val, bool fallback) {
    std::string lower = Trim(val);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") return true;
    if (lower == "0" || lower == "false" || lower == "no" || lower == "off") return false;
    return fallback;
}

int ParseInt(const std::string& val, int fallback) {
    try {
        return std::stoi(Trim(val));
    } catch (...) {
        return fallback;
    }
}

std::wstring GetExecutablePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) return {};
        if (len < buf.size() - 1) {
            buf.resize(len);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

bool IsDirectoryWritable(const std::wstring& dirPath) {
    std::wstring probe = dirPath + L"\\.barspeed_write_test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    return true;
}

} // namespace

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                     nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), needed);
    return out;
}

void AppConfig::Clamp() {
    if (speedUnit != SpeedUnit::BytesPerSec && speedUnit != SpeedUnit::BitsPerSec) {
        speedUnit = SpeedUnit::BytesPerSec;
    }
    updateIntervalMs = std::clamp(updateIntervalMs, 250, 10000);
    pingIntervalMs   = std::clamp(pingIntervalMs, 500, 60000);
    fontSizePt       = std::clamp(fontSizePt, 7, 14);
    if (themeMode != ThemeMode::Auto &&
        themeMode != ThemeMode::DarkTaskbar &&
        themeMode != ThemeMode::LightTaskbar) {
        themeMode = ThemeMode::Auto;
    }
    horizontalOffset = std::clamp(horizontalOffset, -1000, 3000);
    if (pingHost.empty()) {
        pingHost = L"8.8.8.8";
    }
}

std::wstring ConfigStore::GetDefaultConfigPath() {
    std::wstring exe = GetExecutablePath();
    if (!exe.empty()) {
        size_t pos = exe.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            std::wstring exeDir = exe.substr(0, pos);
            std::wstring portablePath = exeDir + L"\\BarSpeed.ini";
            if (GetFileAttributesW(portablePath.c_str()) != INVALID_FILE_ATTRIBUTES ||
                IsDirectoryWritable(exeDir)) {
                return portablePath;
            }
        }
    }

    PWSTR appData = nullptr;
    std::wstring result = L"BarSpeed.ini";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData) {
        std::wstring dir = std::wstring(appData) + L"\\BarSpeed";
        CreateDirectoryW(dir.c_str(), nullptr);
        result = dir + L"\\BarSpeed.ini";
        CoTaskMemFree(appData);
    }
    return result;
}

std::string ConfigStore::Serialize(const AppConfig& cfg) {
    AppConfig c = cfg;
    c.Clamp();

    std::ostringstream oss;
    oss << "# BarSpeed Configuration\n";
    oss << "adapter_name=" << WideToUtf8(c.adapterName) << "\n";
    oss << "speed_unit=" << (c.speedUnit == SpeedUnit::BitsPerSec ? "bits" : "bytes") << "\n";
    oss << "update_interval_ms=" << c.updateIntervalMs << "\n";
    oss << "show_ping=" << (c.showPing ? "true" : "false") << "\n";
    oss << "ping_host=" << WideToUtf8(c.pingHost) << "\n";
    oss << "ping_interval_ms=" << c.pingIntervalMs << "\n";
    oss << "font_size_pt=" << c.fontSizePt << "\n";
    oss << "theme_mode=" << static_cast<int>(c.themeMode) << "\n";
    oss << "horizontal_offset=" << c.horizontalOffset << "\n";
    oss << "show_tray_icon=" << (c.showTrayIcon ? "true" : "false") << "\n";
    return oss.str();
}

AppConfig ConfigStore::Deserialize(const std::string& content) {
    AppConfig cfg;
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';' || trimmed[0] == '[') {
            continue;
        }
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;

        std::string key = Trim(trimmed.substr(0, eq));
        std::string val = Trim(trimmed.substr(eq + 1));

        if (key == "adapter_name") {
            cfg.adapterName = Utf8ToWide(val);
        } else if (key == "speed_unit") {
            cfg.speedUnit = (val == "bits" || val == "1") ? SpeedUnit::BitsPerSec : SpeedUnit::BytesPerSec;
        } else if (key == "update_interval_ms") {
            cfg.updateIntervalMs = ParseInt(val, cfg.updateIntervalMs);
        } else if (key == "show_ping") {
            cfg.showPing = ParseBool(val, cfg.showPing);
        } else if (key == "ping_host") {
            if (!val.empty()) cfg.pingHost = Utf8ToWide(val);
        } else if (key == "ping_interval_ms") {
            cfg.pingIntervalMs = ParseInt(val, cfg.pingIntervalMs);
        } else if (key == "font_size_pt") {
            cfg.fontSizePt = ParseInt(val, cfg.fontSizePt);
        } else if (key == "theme_mode") {
            cfg.themeMode = static_cast<ThemeMode>(ParseInt(val, static_cast<int>(cfg.themeMode)));
        } else if (key == "horizontal_offset") {
            cfg.horizontalOffset = ParseInt(val, cfg.horizontalOffset);
        } else if (key == "show_tray_icon") {
            cfg.showTrayIcon = ParseBool(val, cfg.showTrayIcon);
        }
    }
    cfg.Clamp();
    return cfg;
}

AppConfig ConfigStore::Load(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return AppConfig{};
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    return Deserialize(oss.str());
}

bool ConfigStore::Save(const AppConfig& cfg, const std::wstring& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    std::string data = Serialize(cfg);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

bool ConfigStore::IsAutoStartEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    LONG res = RegQueryValueExW(hKey, kRunValueName, nullptr, &type, nullptr, &size);
    RegCloseKey(hKey);
    return (res == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && size > sizeof(wchar_t));
}

bool ConfigStore::SetAutoStartEnabled(bool enable) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    LONG res = ERROR_SUCCESS;
    if (enable) {
        std::wstring exe = GetExecutablePath();
        if (exe.empty()) {
            RegCloseKey(hKey);
            return false;
        }
        std::wstring cmd = L"\"" + exe + L"\"";
        res = RegSetValueExW(hKey, kRunValueName, 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(cmd.c_str()),
                             static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        res = RegDeleteValueW(hKey, kRunValueName);
        if (res == ERROR_FILE_NOT_FOUND) {
            res = ERROR_SUCCESS;
        }
    }
    RegCloseKey(hKey);
    return res == ERROR_SUCCESS;
}
