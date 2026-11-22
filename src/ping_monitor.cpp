#include "ping_monitor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#include <algorithm>
#include <chrono>
#include <vector>

PingMonitor::PingMonitor() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

PingMonitor::~PingMonitor() {
    Stop();
    WSACleanup();
}

void PingMonitor::Start(HWND notifyHwnd, UINT notifyMsg, bool enabled, const std::wstring& host, int intervalMs) {
    Stop();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_notifyHwnd = notifyHwnd;
        m_notifyMsg  = notifyMsg;
        m_enabled    = enabled;
        m_host       = host.empty() ? L"8.8.8.8" : host;
        m_intervalMs = std::clamp(intervalMs, 500, 60000);
        m_running    = true;
        m_configGen++;
        m_status      = PingStatus{};
        m_status.host = m_host;
        m_rttSum      = 0;
        m_rttCount    = 0;
    }
    m_thread = std::thread(&PingMonitor::WorkerLoop, this);
}

void PingMonitor::Configure(bool enabled, const std::wstring& host, int intervalMs) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::wstring cleanHost = host.empty() ? L"8.8.8.8" : host;
        bool hostChanged = (cleanHost != m_host);
        bool enabledChanged = (enabled != m_enabled);

        m_enabled    = enabled;
        m_host       = cleanHost;
        m_intervalMs = std::clamp(intervalMs, 500, 60000);
        m_configGen++;

        if (hostChanged || enabledChanged) {
            m_status      = PingStatus{};
            m_status.host = m_host;
            m_rttSum      = 0;
            m_rttCount    = 0;
        }
    }
    m_cv.notify_all();
}

void PingMonitor::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) return;
        m_running = false;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

PingStatus PingMonitor::GetStatus() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

bool PingMonitor::ResolveIPv4(const std::wstring& host, uint32_t& outAddr) {
    IN_ADDR addr{};
    if (InetPtonW(AF_INET, host.c_str(), &addr) == 1) {
        outAddr = addr.S_un.S_addr;
        return true;
    }

    ADDRINFOW hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOW* res = nullptr;
    if (GetAddrInfoW(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        return false;
    }
    auto* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    outAddr = sin->sin_addr.S_un.S_addr;
    FreeAddrInfoW(res);
    return true;
}

void PingMonitor::WorkerLoop() {
    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) {
        return;
    }

    constexpr char kPayload[] = "BarSpeedPing";
    constexpr WORD kPayloadSize = static_cast<WORD>(sizeof(kPayload) - 1);
    std::vector<BYTE> replyBuf(sizeof(ICMP_ECHO_REPLY) + kPayloadSize + 64, 0);

    std::wstring cachedHost;
    uint32_t cachedAddr = 0;
    bool hasCachedAddr = false;

    for (;;) {
        bool enabled = false;
        std::wstring host;
        int intervalMs = 2000;
        uint64_t gen = 0;
        HWND notifyHwnd = nullptr;
        UINT notifyMsg = 0;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&]() { return !m_running || m_enabled; });
            if (!m_running) break;

            enabled    = m_enabled;
            host       = m_host;
            intervalMs = m_intervalMs;
            gen        = m_configGen;
            notifyHwnd = m_notifyHwnd;
            notifyMsg  = m_notifyMsg;
        }

        if (!enabled) continue;

        auto cycleStart = std::chrono::steady_clock::now();

        if (!hasCachedAddr || host != cachedHost) {
            hasCachedAddr = ResolveIPv4(host, cachedAddr);
            cachedHost = host;
        }

        bool reachable = false;
        uint32_t rttMs = 0;

        if (hasCachedAddr) {
            constexpr DWORD kTimeoutMs = 1500;
            DWORD replies = IcmpSendEcho(
                hIcmp,
                static_cast<IPAddr>(cachedAddr),
                const_cast<char*>(kPayload),
                kPayloadSize,
                nullptr,
                replyBuf.data(),
                static_cast<DWORD>(replyBuf.size()),
                kTimeoutMs
            );

            if (replies > 0) {
                auto* echo = reinterpret_cast<const ICMP_ECHO_REPLY*>(replyBuf.data());
                if (echo->Status == IP_SUCCESS) {
                    reachable = true;
                    rttMs = echo->RoundTripTime;
                }
            } else {
                // Re-resolve next time in case DNS IP changed
                hasCachedAddr = false;
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running) break;

            // Only publish if host/config didn't change mid-flight
            if (gen == m_configGen && m_enabled) {
                m_status.hasResult = true;
                m_status.reachable = reachable;
                m_status.host      = host;
                m_status.sent++;
                if (reachable) {
                    m_status.rttMs = rttMs;
                    if (m_rttCount == 0 || rttMs < m_status.minRttMs) {
                        m_status.minRttMs = rttMs;
                    }
                    m_rttSum += rttMs;
                    m_rttCount++;
                    m_status.avgRttMs = static_cast<uint32_t>(m_rttSum / m_rttCount);
                } else {
                    m_status.lost++;
                }

                if (notifyHwnd && IsWindow(notifyHwnd)) {
                    PostMessageW(notifyHwnd, notifyMsg, 0, 0);
                }
            }
        }

        // Wait for the remainder of the interval (waking immediately on config change or Stop)
        auto targetWake = cycleStart + std::chrono::milliseconds(intervalMs);
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_until(lock, targetWake, [&]() {
            return !m_running || m_configGen != gen || !m_enabled;
        });
        if (!m_running) break;
    }

    IcmpCloseHandle(hIcmp);
}
