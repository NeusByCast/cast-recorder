#include "windows_enum.h"
#include <dwmapi.h>
#include <cwctype>
#include <algorithm>

namespace cr {
namespace {
std::wstring lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
std::wstring exeOf(HWND h) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    std::wstring r;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t b[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, b, &n)) { r = b; size_t k = r.find_last_of(L'\\'); if (k != std::wstring::npos) r = r.substr(k + 1); }
        CloseHandle(p);
    }
    return r;
}
bool capturable(HWND h) {
    if (!IsWindowVisible(h)) return false;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;
    if (GetAncestor(h, GA_ROOTOWNER) != h && !(ex & WS_EX_APPWINDOW)) return false;  // owned popups/dialogs
    DWORD cloaked = 0;  // hidden UWP shells, windows on other virtual desktops
    if (SUCCEEDED(DwmGetWindowAttribute(h, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof cloaked)) && cloaked) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    RECT r; GetWindowRect(h, &r);
    if (!IsIconic(h) && (r.right - r.left < 32 || r.bottom - r.top < 32)) return false;
    return true;
}
HICON iconOf(HWND h) {
    DWORD_PTR ic = 0;
    if (!SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 50, &ic) || !ic)
        SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 50, &ic);
    if (!ic) ic = (DWORD_PTR)GetClassLongPtrW(h, GCLP_HICON);
    if (!ic) ic = (DWORD_PTR)GetClassLongPtrW(h, GCLP_HICONSM);
    return (HICON)ic;
}
BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
    if (!capturable(h)) return TRUE;
    CaptureWindow w;
    w.hwnd = h;
    int n = GetWindowTextLengthW(h);
    w.title.resize((size_t)n + 1);
    w.title.resize((size_t)GetWindowTextW(h, &w.title[0], n + 1));
    w.exe = exeOf(h);
    w.icon = iconOf(h);
    w.minimized = IsIconic(h) != 0;
    GetWindowRect(h, &w.rect);
    reinterpret_cast<std::vector<CaptureWindow>*>(lp)->push_back(std::move(w));
    return TRUE;
}
}  // namespace

std::vector<CaptureWindow> listCaptureWindows() {
    std::vector<CaptureWindow> v;
    EnumWindows(enumProc, (LPARAM)&v);
    return v;
}

HWND findCaptureWindow(const std::wstring& q) {
    std::wstring l = lower(q);
    for (auto& w : listCaptureWindows())
        if (lower(w.title).find(l) != std::wstring::npos || lower(w.exe).find(l) != std::wstring::npos) return w.hwnd;
    return nullptr;
}
std::wstring windowExe(HWND h) { return exeOf(h); }

HWND resolveSavedWindow(const std::wstring& exe, const std::wstring& title) {
    if (exe.empty()) return nullptr;
    auto ws = listCaptureWindows();
    std::wstring le = lower(exe);
    for (auto& w : ws) if (lower(w.exe) == le && w.title == title) return w.hwnd;
    for (auto& w : ws) if (lower(w.exe) == le) return w.hwnd;
    return nullptr;
}

std::vector<DisplayInfo> listDisplays() {
    std::vector<DisplayInfo> v;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        MONITORINFOEXW mi{}; mi.cbSize = sizeof mi;
        if (!GetMonitorInfoW(m, &mi)) return TRUE;
        DisplayInfo d; d.mon = m; d.device = mi.szDevice; d.rect = mi.rcMonitor; d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        ((std::vector<DisplayInfo>*)p)->push_back(d);
        return TRUE;
    }, (LPARAM)&v);
    std::sort(v.begin(), v.end(), [](const DisplayInfo& a, const DisplayInfo& b) {
        return a.rect.left != b.rect.left ? a.rect.left < b.rect.left : a.rect.top < b.rect.top; });
    for (size_t i = 0; i < v.size(); ++i) v[i].index = (int)i + 1;
    return v;
}
}  // namespace cr
