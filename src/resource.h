#pragma once

// Custom window messages
constexpr unsigned int WM_APP_TRAY_ICON         = 0x8000 + 1;
constexpr unsigned int WM_APP_PING_UPDATE       = 0x8000 + 2;
constexpr unsigned int WM_APP_ACTIVATE_EXISTING = 0x8000 + 3;

// Timers
constexpr uintptr_t TIMER_ID_NET_SAMPLE         = 1;
constexpr uintptr_t TIMER_ID_TASKBAR_SYNC       = 2;

// Context menu command IDs
constexpr unsigned int IDM_SHOW_PING            = 1001;
constexpr unsigned int IDM_UNIT_BYTES           = 1002;
constexpr unsigned int IDM_UNIT_BITS            = 1003;
constexpr unsigned int IDM_START_WINDOWS        = 1004;
constexpr unsigned int IDM_OPEN_SETTINGS        = 1005;
constexpr unsigned int IDM_RESET_POSITION       = 1006;
constexpr unsigned int IDM_EXIT                 = 1007;

// Adapter submenu dynamic IDs (2000 = Auto, 2001..2199 = specific adapters)
constexpr unsigned int IDM_ADAPTER_AUTO         = 2000;
constexpr unsigned int IDM_ADAPTER_BASE         = 2001;
constexpr unsigned int IDM_ADAPTER_MAX          = 2199;

// Settings window control IDs
constexpr int IDC_COMBO_ADAPTER                 = 3001;
constexpr int IDC_COMBO_UNIT                    = 3002;
constexpr int IDC_COMBO_UPDATE_INTERVAL         = 3003;
constexpr int IDC_CHECK_SHOW_PING               = 3004;
constexpr int IDC_EDIT_PING_HOST                = 3005;
constexpr int IDC_COMBO_PING_INTERVAL           = 3006;
constexpr int IDC_COMBO_FONT_SIZE               = 3007;
constexpr int IDC_COMBO_THEME                   = 3008;
constexpr int IDC_EDIT_OFFSET_X                 = 3009;
constexpr int IDC_CHECK_TRAY_ICON               = 3010;
constexpr int IDC_CHECK_AUTOSTART               = 3011;
constexpr int IDC_BTN_OK                        = 3020;
constexpr int IDC_BTN_APPLY                     = 3021;
constexpr int IDC_BTN_CANCEL                    = 3022;
