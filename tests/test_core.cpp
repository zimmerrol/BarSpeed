#include "config.h"
#include "net_monitor.h"
#include "ping_monitor.h"
#include "tray_icon.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

namespace {

int g_failures = 0;

void Check(bool cond, const char* expr, int line) {
    if (!cond) {
        std::cerr << "FAIL (line " << line << "): " << expr << "\n";
        g_failures++;
    }
}

#define EXPECT(expr) Check((expr), #expr, __LINE__)

void TestFormatSpeedBytes() {
    EXPECT(NetworkMonitor::FormatSpeed(0.0, SpeedUnit::BytesPerSec) == L"0.00 KB/s");
    EXPECT(NetworkMonitor::FormatSpeed(573.44, SpeedUnit::BytesPerSec) == L"0.56 KB/s");
    EXPECT(NetworkMonitor::FormatSpeed(1751.04, SpeedUnit::BytesPerSec) == L"1.71 KB/s");
    EXPECT(NetworkMonitor::FormatSpeed(15.5 * 1024.0, SpeedUnit::BytesPerSec) == L"15.5 KB/s");
    EXPECT(NetworkMonitor::FormatSpeed(420.0 * 1024.0, SpeedUnit::BytesPerSec) == L"420 KB/s");
    EXPECT(NetworkMonitor::FormatSpeed(2.5 * 1024.0 * 1024.0, SpeedUnit::BytesPerSec) == L"2.50 MB/s");
    EXPECT(NetworkMonitor::FormatSpeed(12.4 * 1024.0 * 1024.0, SpeedUnit::BytesPerSec) == L"12.4 MB/s");
    EXPECT(NetworkMonitor::FormatSpeed(1.25 * 1024.0 * 1024.0 * 1024.0, SpeedUnit::BytesPerSec) == L"1.25 GB/s");
    EXPECT(NetworkMonitor::FormatSpeed(-5.0, SpeedUnit::BytesPerSec) == L"0.00 KB/s");
    EXPECT(NetworkMonitor::FormatSpeed(std::numeric_limits<double>::quiet_NaN(), SpeedUnit::BytesPerSec) == L"0.00 KB/s");
}

void TestFormatSpeedBits() {
    EXPECT(NetworkMonitor::FormatSpeed(0.0, SpeedUnit::BitsPerSec) == L"0.00 Kbps");
    EXPECT(NetworkMonitor::FormatSpeed(1250.0, SpeedUnit::BitsPerSec) == L"10.0 Kbps");
    EXPECT(NetworkMonitor::FormatSpeed(125000.0, SpeedUnit::BitsPerSec) == L"1.00 Mbps");
    EXPECT(NetworkMonitor::FormatSpeed(12500000.0, SpeedUnit::BitsPerSec) == L"100 Mbps");
    EXPECT(NetworkMonitor::FormatSpeed(125000000.0, SpeedUnit::BitsPerSec) == L"1.00 Gbps");
}

void TestFormatTotalBytes() {
    EXPECT(NetworkMonitor::FormatTotalBytes(512) == L"512 B");
    EXPECT(NetworkMonitor::FormatTotalBytes(2048) == L"2.00 KB");
    EXPECT(NetworkMonitor::FormatTotalBytes(5ULL * 1024 * 1024) == L"5.00 MB");
    EXPECT(NetworkMonitor::FormatTotalBytes(3ULL * 1024 * 1024 * 1024) == L"3.00 GB");
}

void TestConfigRoundTrip() {
    AppConfig orig;
    orig.adapterName      = L"Wi-Fi (5GHz)";
    orig.speedUnit        = SpeedUnit::BitsPerSec;
    orig.updateIntervalMs = 500;
    orig.showPing         = false;
    orig.pingHost         = L"1.1.1.1";
    orig.pingIntervalMs   = 3000;
    orig.fontSizePt       = 11;
    orig.themeMode        = ThemeMode::LightTaskbar;
    orig.horizontalOffset = 42;
    orig.showTrayIcon     = false;

    std::string serialized = ConfigStore::Serialize(orig);
    AppConfig parsed = ConfigStore::Deserialize(serialized);

    EXPECT(parsed.adapterName == orig.adapterName);
    EXPECT(parsed.speedUnit == orig.speedUnit);
    EXPECT(parsed.updateIntervalMs == orig.updateIntervalMs);
    EXPECT(parsed.showPing == orig.showPing);
    EXPECT(parsed.pingHost == orig.pingHost);
    EXPECT(parsed.pingIntervalMs == orig.pingIntervalMs);
    EXPECT(parsed.fontSizePt == orig.fontSizePt);
    EXPECT(parsed.themeMode == orig.themeMode);
    EXPECT(parsed.horizontalOffset == orig.horizontalOffset);
    EXPECT(parsed.showTrayIcon == orig.showTrayIcon);

    // Clamping test for out-of-range values
    AppConfig clamped = ConfigStore::Deserialize(
        "update_interval_ms=10\n"
        "ping_interval_ms=5\n"
        "font_size_pt=99\n"
        "horizontal_offset=99999\n"
        "ping_host=\n"
    );
    EXPECT(clamped.updateIntervalMs == 250);
    EXPECT(clamped.pingIntervalMs == 500);
    EXPECT(clamped.fontSizePt == 14);
    EXPECT(clamped.horizontalOffset == 3000);
    EXPECT(clamped.pingHost == L"8.8.8.8");
}

void TestNetworkMonitorSmoke() {
    NetworkMonitor mon;
    auto adapters = NetworkMonitor::EnumerateAdapters();
    std::cout << "Detected usable adapters: " << adapters.size() << "\n";

    NetworkSample s1 = mon.Sample(L"");
    EXPECT(s1.downloadBytesPerSec == 0.0);
    EXPECT(s1.uploadBytesPerSec == 0.0);
    EXPECT(!s1.activeAdapterLabel.empty());

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    NetworkSample s2 = mon.Sample(L"");
    EXPECT(s2.downloadBytesPerSec >= 0.0);
    EXPECT(s2.uploadBytesPerSec >= 0.0);
}

void TestPingMonitorLifecycle() {
    PingMonitor ping;
    // Start disabled -> should send 0 packets and stop cleanly
    ping.Start(nullptr, 0, false, L"127.0.0.1", 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    PingStatus st = ping.GetStatus();
    EXPECT(!st.hasResult);
    EXPECT(st.sent == 0);

    // Enable pinging loopback 127.0.0.1 (works offline / inside sandbox too if ICMP loopback allowed)
    ping.Configure(true, L"127.0.0.1", 500);
    for (int i = 0; i < 20; ++i) {
        st = ping.GetStatus();
        if (st.hasResult) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::cout << "Loopback ping test: hasResult=" << st.hasResult
              << " reachable=" << st.reachable
              << " rtt=" << st.rttMs << "ms\n";

    // Stop must return promptly without blocking
    auto t0 = std::chrono::steady_clock::now();
    ping.Stop();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT(elapsedMs < 2000);
}

void TestTrayIconRendering() {
    HICON hIcon = TrayIcon::CreateAppIcon(32);
    EXPECT(hIcon != nullptr);
    if (!hIcon) return;

    ICONINFO ii{};
    EXPECT(GetIconInfo(hIcon, &ii) == TRUE);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);

    // Draw icon into a 32x32 32-bit DIB and verify visible non-zero pixels
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = 32;
    bmi.bmiHeader.biHeight      = -32;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hBmp = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screenDC);

    EXPECT(memDC != nullptr && hBmp != nullptr && bits != nullptr);
    if (memDC && hBmp && bits) {
        HGDIOBJ old = SelectObject(memDC, hBmp);
        std::fill_n(static_cast<uint32_t*>(bits), 32 * 32, 0u);
        EXPECT(DrawIconEx(memDC, 0, 0, hIcon, 32, 32, 0, nullptr, DI_NORMAL) == TRUE);
        int nonZero = 0;
        const auto* px = static_cast<const uint32_t*>(bits);
        for (int i = 0; i < 32 * 32; ++i) {
            if ((px[i] & 0x00FFFFFFu) != 0) nonZero++;
        }
        EXPECT(nonZero > 400);
        SelectObject(memDC, old);
    }
    if (hBmp) DeleteObject(hBmp);
    if (memDC) DeleteDC(memDC);
    DestroyIcon(hIcon);
}

} // namespace

int main() {
    TestFormatSpeedBytes();
    TestFormatSpeedBits();
    TestFormatTotalBytes();
    TestConfigRoundTrip();
    TestTrayIconRendering();
    TestNetworkMonitorSmoke();
    TestPingMonitorLifecycle();

    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED!\n";
        return 0;
    }
    std::cerr << g_failures << " TEST(S) FAILED!\n";
    return 1;
}
