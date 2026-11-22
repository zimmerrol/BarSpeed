#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct PingStatus {
    bool         hasResult = false;
    bool         reachable = false;
    uint32_t     rttMs     = 0;
    uint32_t     minRttMs  = 0;
    uint32_t     avgRttMs  = 0;
    uint64_t     sent      = 0;
    uint64_t     lost      = 0;
    std::wstring host      = L"8.8.8.8";
};

class PingMonitor {
public:
    PingMonitor();
    ~PingMonitor();

    PingMonitor(const PingMonitor&) = delete;
    PingMonitor& operator=(const PingMonitor&) = delete;

    // Starts the background worker thread. Posts notifyMsg to notifyHwnd on each ping update.
    void Start(HWND notifyHwnd, UINT notifyMsg, bool enabled, const std::wstring& host, int intervalMs);

    // Dynamically updates configuration without restarting the thread.
    void Configure(bool enabled, const std::wstring& host, int intervalMs);

    // Cleanly stops the background worker thread.
    void Stop();

    PingStatus GetStatus() const;

private:
    void WorkerLoop();
    static bool ResolveIPv4(const std::wstring& host, uint32_t& outAddr);

    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::thread             m_thread;

    HWND         m_notifyHwnd = nullptr;
    UINT         m_notifyMsg  = 0;
    bool         m_running    = false;
    bool         m_enabled    = false;
    std::wstring m_host       = L"8.8.8.8";
    int          m_intervalMs = 2000;
    uint64_t     m_configGen  = 0;

    PingStatus   m_status;
    uint64_t     m_rttSum     = 0;
    uint64_t     m_rttCount   = 0;
};
