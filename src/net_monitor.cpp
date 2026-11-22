#include "net_monitor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace {

bool IsUsableInterface(const MIB_IF_ROW2& row) {
    if (row.OperStatus != IfOperStatusUp) return false;
    if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) return false;
    if (row.Type == IF_TYPE_TUNNEL) return false;
    if (row.InterfaceAndOperStatusFlags.FilterInterface) return false;
    // Skip interfaces with no media connected
    if (row.MediaConnectState == MediaConnectStateDisconnected) return false;
    return true;
}

std::wstring FormatScaled(double value, const wchar_t* unit) {
    wchar_t buf[64]{};
    if (value < 10.0) {
        std::swprintf(buf, 64, L"%.2f %s", value, unit);
    } else if (value < 100.0) {
        std::swprintf(buf, 64, L"%.1f %s", value, unit);
    } else {
        std::swprintf(buf, 64, L"%.0f %s", value, unit);
    }
    return buf;
}

} // namespace

NetworkMonitor::NetworkMonitor() {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    m_perfFreq = (freq.QuadPart > 0) ? freq.QuadPart : 1;
}

void NetworkMonitor::Reset() {
    m_prevCounters.clear();
    m_lastTick = 0;
    m_sessionDown = 0;
    m_sessionUp = 0;
}

std::vector<AdapterInfo> NetworkMonitor::EnumerateAdapters() {
    std::vector<AdapterInfo> adapters;
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) {
        return adapters;
    }

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (!IsUsableInterface(row)) continue;

        AdapterInfo info;
        info.luid = row.InterfaceLuid.Value;
        info.alias = row.Alias;
        info.description = row.Description;
        info.isHardware = (row.InterfaceAndOperStatusFlags.HardwareInterface != 0);
        if (!info.alias.empty()) {
            adapters.push_back(std::move(info));
        }
    }

    FreeMibTable(table);

    // Put hardware NICs first, then sort alphabetically by friendly name
    std::sort(adapters.begin(), adapters.end(), [](const AdapterInfo& a, const AdapterInfo& b) {
        if (a.isHardware != b.isHardware) return a.isHardware > b.isHardware;
        return a.alias < b.alias;
    });
    return adapters;
}

NetworkSample NetworkMonitor::Sample(const std::wstring& targetAdapterAlias) {
    if (targetAdapterAlias != m_lastTarget) {
        Reset();
        m_lastTarget = targetAdapterAlias;
    }

    NetworkSample result;
    result.sessionDownBytes = m_sessionDown;
    result.sessionUpBytes = m_sessionUp;

    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) {
        result.activeAdapterLabel = targetAdapterAlias.empty() ? L"Auto (Unavailable)" : targetAdapterAlias;
        return result;
    }

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);

    std::vector<const MIB_IF_ROW2*> selected;
    std::vector<const MIB_IF_ROW2*> fallbackNonHw;

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (!IsUsableInterface(row)) continue;

        if (!targetAdapterAlias.empty()) {
            if (targetAdapterAlias == row.Alias) {
                selected.push_back(&row);
            }
        } else {
            if (row.InterfaceAndOperStatusFlags.HardwareInterface) {
                selected.push_back(&row);
            } else {
                fallbackNonHw.push_back(&row);
            }
        }
    }

    // If Auto mode finds no hardware NIC up (e.g. virtual-only VM), fall back to non-filter active NICs
    if (targetAdapterAlias.empty() && selected.empty()) {
        selected = std::move(fallbackNonHw);
    }

    std::unordered_map<uint64_t, CounterPair> curCounters;
    uint64_t deltaIn = 0;
    uint64_t deltaOut = 0;
    std::wstring activeNames;

    for (const MIB_IF_ROW2* row : selected) {
        uint64_t luid = row->InterfaceLuid.Value;
        CounterPair cur{ row->InOctets, row->OutOctets };
        curCounters[luid] = cur;

        if (!activeNames.empty()) activeNames += L", ";
        activeNames += row->Alias;

        auto it = m_prevCounters.find(luid);
        if (it != m_prevCounters.end()) {
            if (cur.inOctets >= it->second.inOctets) {
                deltaIn += (cur.inOctets - it->second.inOctets);
            }
            if (cur.outOctets >= it->second.outOctets) {
                deltaOut += (cur.outOctets - it->second.outOctets);
            }
        }
    }

    FreeMibTable(table);

    if (m_lastTick != 0 && now.QuadPart > m_lastTick) {
        double dt = static_cast<double>(now.QuadPart - m_lastTick) / static_cast<double>(m_perfFreq);
        if (dt > 0.05) {
            result.downloadBytesPerSec = static_cast<double>(deltaIn) / dt;
            result.uploadBytesPerSec   = static_cast<double>(deltaOut) / dt;
            m_sessionDown += deltaIn;
            m_sessionUp   += deltaOut;
        }
    }

    m_prevCounters = std::move(curCounters);
    m_lastTick = now.QuadPart;

    result.sessionDownBytes = m_sessionDown;
    result.sessionUpBytes   = m_sessionUp;
    if (!activeNames.empty()) {
        result.activeAdapterLabel = activeNames;
    } else if (!targetAdapterAlias.empty()) {
        result.activeAdapterLabel = targetAdapterAlias + L" (Disconnected)";
    } else {
        result.activeAdapterLabel = L"No active adapter";
    }
    return result;
}

std::wstring NetworkMonitor::FormatSpeed(double bytesPerSec, SpeedUnit unit) {
    if (!std::isfinite(bytesPerSec) || bytesPerSec < 0.0) {
        bytesPerSec = 0.0;
    }

    if (unit == SpeedUnit::BitsPerSec) {
        double bitsPerSec = bytesPerSec * 8.0;
        double kbps = bitsPerSec / 1000.0;
        if (kbps < 1000.0) {
            return FormatScaled(kbps, L"Kbps");
        }
        double mbps = kbps / 1000.0;
        if (mbps < 1000.0) {
            return FormatScaled(mbps, L"Mbps");
        }
        double gbps = mbps / 1000.0;
        return FormatScaled(gbps, L"Gbps");
    }

    // BytesPerSec (1024-based KB/s, MB/s, GB/s)
    double kbs = bytesPerSec / 1024.0;
    if (kbs < 1000.0) {
        return FormatScaled(kbs, L"KB/s");
    }
    double mbs = kbs / 1024.0;
    if (mbs < 1000.0) {
        return FormatScaled(mbs, L"MB/s");
    }
    double gbs = mbs / 1024.0;
    return FormatScaled(gbs, L"GB/s");
}

std::wstring NetworkMonitor::FormatTotalBytes(uint64_t totalBytes) {
    constexpr double kKB = 1024.0;
    constexpr double kMB = kKB * 1024.0;
    constexpr double kGB = kMB * 1024.0;
    constexpr double kTB = kGB * 1024.0;

    double b = static_cast<double>(totalBytes);
    if (b < kKB) {
        return std::to_wstring(totalBytes) + L" B";
    }
    if (b < kMB) {
        return FormatScaled(b / kKB, L"KB");
    }
    if (b < kGB) {
        return FormatScaled(b / kMB, L"MB");
    }
    if (b < kTB) {
        return FormatScaled(b / kGB, L"GB");
    }
    return FormatScaled(b / kTB, L"TB");
}
