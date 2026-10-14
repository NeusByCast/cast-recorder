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
}
