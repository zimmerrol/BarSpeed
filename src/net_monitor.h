#pragma once

#include "config.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct AdapterInfo {
    uint64_t     luid = 0;
    std::wstring alias;       // Friendly name, e.g. "Ethernet", "Wi-Fi"
    std::wstring description; // Driver description, e.g. "Intel(R) Wi-Fi 6E AX211"
    bool         isHardware = false;
};

struct NetworkSample {
    double       downloadBytesPerSec = 0.0;
    double       uploadBytesPerSec   = 0.0;
    uint64_t     sessionDownBytes    = 0;
    uint64_t     sessionUpBytes      = 0;
    std::wstring activeAdapterLabel;
};

class NetworkMonitor {
public:
    NetworkMonitor();

    // Enumerates currently connected (Up), non-loopback, non-filter network adapters.
    static std::vector<AdapterInfo> EnumerateAdapters();

    // Takes a new sample and updates download/upload speeds for the selected adapter
    // (empty targetAdapterAlias = Auto: sum all active hardware NICs).
    NetworkSample Sample(const std::wstring& targetAdapterAlias);

    // Resets baseline counters (e.g. when switching selected adapter).
    void Reset();

    // Formatting utilities
    static std::wstring FormatSpeed(double bytesPerSec, SpeedUnit unit);
    static std::wstring FormatTotalBytes(uint64_t totalBytes);

private:
    struct CounterPair {
        uint64_t inOctets  = 0;
        uint64_t outOctets = 0;
    };

    std::unordered_map<uint64_t, CounterPair> m_prevCounters;
    int64_t  m_lastTick       = 0;
    int64_t  m_perfFreq       = 1;
    uint64_t m_sessionDown    = 0;
    uint64_t m_sessionUp      = 0;
    std::wstring m_lastTarget;
};
