#pragma once
// Visible top-level windows that make sense to capture (for the source picker; see WgcCapture::startWindow).
#include <windows.h>
#include <string>
#include <vector>

namespace cr {
struct CaptureWindow {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring exe;      // process image name, e.g. "notepad.exe"
    HICON icon = nullptr;  // borrowed from the window/class: don't DestroyIcon
    bool minimized = false;
    RECT rect{};           // DWM extended frame bounds (visible frame, no shadow); restored placement when minimized
    std::wstring app;      // "Brave"
    std::wstring page;     // title without the app suffix ("Cast Recorder v1.0 release"); may be empty
    std::wstring label() const;  // "Brave · Cast Recorder v1.0 release"
};
// Real top-level app windows only (alt-tab rules: no tool windows, owned popups, hover cards, tooltips, media
// popups, cloaked or tiny windows), one entry per app window (same-title windows numbered "(2)", "(3)"), grouped by app in z-order.
std::vector<CaptureWindow> listCaptureWindows();
void includeOwnWindowsForTest(bool on);  // CI picker test: its test windows live in this process
// Why a window is not offered (nullptr = offered). For logs and the CI picker test.
const char* captureRejectReason(HWND h);
HWND findCaptureWindow(const std::wstring& titleOrExe);  // case-insensitive substring match (CLI/tests)

struct DisplayInfo {
    HMONITOR mon = nullptr;
    std::wstring device;   // \\.\DISPLAY1 (persisted)
    RECT rect{};
    bool primary = false;
    int index = 0;         // 1-based, ordered left-to-right
};
std::vector<DisplayInfo> listDisplays();

// What to capture. Persisted as kind + monitor device name / window exe + title.
struct CaptureTarget {
    enum Kind { AllDisplays, Monitor, Window } kind = Monitor;
    HMONITOR mon = nullptr;   // Monitor (nullptr = primary)
    HWND hwnd = nullptr;      // Window
};
// Re-find a saved window among the offered ones: exact exe+title, then the largest window of the same exe (title
// changed, e.g. another tab), else nullptr. Never a popup of that app.
HWND resolveSavedWindow(const std::wstring& exe, const std::wstring& title);
std::wstring windowExe(HWND h);
}
