# BarSpeed

A lightweight, clean, from-scratch Windows 11 taskbar network speed and ping monitor written in native C++20 and Win32 APIs — with **zero third-party dependencies**.

> Fully vibe-coded with Antigravity and Gemini 4 Neon.

## Features

- **Native Windows 11 Taskbar Integration**: Renders directly on the Windows 11 taskbar next to the system tray chevron using per-pixel alpha Direct2D + DirectWrite (`UpdateLayeredWindow`), seamlessly matching both Dark Mode and Light Mode Acrylic/Mica taskbars.
- **Accurate 64-bit Speed Counters**: Uses `GetIfTable2` (`iphlpapi.dll`) with hardware/filter interface deduplication so physical NICs aren't double-counted with QoS/WFP/VPN virtual filters. Supports **Auto (All Active Physical)** or choosing any specific adapter.
- **Configurable Ping Indicator**: Native ICMP echo (`IcmpSendEcho`, no `ping.exe` console windows, no admin privileges required) on a dedicated background thread. Defaults to `8.8.8.8`, customizable host/IP and interval, with a color-coded latency dot (green `< 50 ms`, amber `50–149 ms`, red `≥ 150 ms` or timeout) and instant toggle on/off.
- **Units**: Switch between **Bytes/s** (`KB/s`, `MB/s`, `GB/s`) and **Bits/s** (`Kbps`, `Mbps`, `Gbps`).
- **DPI & Explorer-Restart Resilient**: Per-Monitor DPI Aware v2, automatically scales across 100%–200%+ displays, adapts to theme changes, and automatically re-attaches if `explorer.exe` restarts.
- **Interactive Controls**:
  - **Right-click** the taskbar widget (or tray icon) for quick toggles (Ping on/off, Unit, Network Adapter, Reset Position, Start with Windows, Settings, Exit).
  - **Hover** over the widget for a live tooltip with active adapter, session upload/download totals, and ping min/avg/loss stats.
  - **Shift + Left-Drag** on the widget to nudge its horizontal position pixel-by-pixel along the taskbar.
  - **Double-click** the widget (or tray icon) to open the Settings window.

## Security & Auditability

Built from scratch so every line of code can be audited quickly:
- **Zero third-party libraries or prebuilt binaries** — compiles strictly against standard Microsoft Windows SDK headers and static MSVC CRT (`/MT`).
- **Network traffic**: Only sends ICMP echo requests to your configured ping host (default `8.8.8.8`) when the Ping Indicator is enabled. Disabling the Ping Indicator results in **zero** outbound packets.
- **No DLL injection** into `explorer.exe` and **no administrator privileges** required.
- **Plain-text config**: Saved to `BarSpeed.ini` next to the `.exe` (or `%APPDATA%\BarSpeed\BarSpeed.ini` if the folder is read-only).

## Building from Source

Requires **Visual Studio 2022** (or **Visual Studio 2022 Build Tools**) with the C++ desktop toolchain and Windows 10/11 SDK.

```bat
build.bat
```

Output: `build\BarSpeed.exe` (single standalone x64 executable, ~380 KB, no external runtime DLLs required).

To build and run the unit test suite:

```bat
build.bat test
```

## Project Layout

- `src/main.cpp` — Application entry point, single-instance guard, timers, context menu, and Explorer-restart recovery
- `src/taskbar_widget.h/.cpp` — Windows 11 taskbar docking and Direct2D/DirectWrite rendering
- `src/net_monitor.h/.cpp` — 64-bit `GetIfTable2` network traffic sampling and speed/byte formatting
- `src/ping_monitor.h/.cpp` — Background-thread native ICMP ping (`IcmpSendEcho`)
- `src/settings_dialog.h/.cpp` — DPI-aware native Win32 Settings window
- `src/tray_icon.h/.cpp` — Programmatic 32-bit ARGB tray icon and `Shell_NotifyIconW` wrapper
- `src/config.h/.cpp` — Plain INI configuration persistence and optional `HKCU\...\Run` startup registration
- `tests/test_core.cpp` — Unit tests for speed formatting, config round-trip, tray icon rendering, adapter enumeration, and ping lifecycle

## License

[MIT](LICENSE)
