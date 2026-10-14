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
    RECT rect{};
};
std::vector<CaptureWindow> listCaptureWindows();  // z-order (topmost first); excludes our own process
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
// Re-find a saved window: exact exe+title, then same exe (title changed, e.g. document name), else nullptr.
HWND resolveSavedWindow(const std::wstring& exe, const std::wstring& title);
std::wstring windowExe(HWND h);
}
