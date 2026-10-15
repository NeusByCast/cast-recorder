#include "windows_enum.h"
#include <dwmapi.h>
#include <cwctype>
#include <algorithm>
#include <map>
#include <vector>
#include "winlabel.h"

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
bool frameRect(HWND h, RECT& r) {
    if (IsIconic(h)) {
        WINDOWPLACEMENT wp{sizeof wp};
        if (GetWindowPlacement(h, &wp)) { r = wp.rcNormalPosition; return true; }
    }
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) return true;
    return GetWindowRect(h, &r) != 0;
}
bool g_includeOwn = false;
WinFacts factsOf(HWND h) {
    WinFacts f;
    f.visible = IsWindowVisible(h) != 0;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    f.ownProcess = pid == GetCurrentProcessId() && !g_includeOwn;
    f.style = (uint32_t)GetWindowLongPtrW(h, GWL_STYLE);
    f.exStyle = (uint32_t)GetWindowLongPtrW(h, GWL_EXSTYLE);
    f.child = (f.style & WS_CHILD) != 0;
    f.owned = GetAncestor(h, GA_ROOTOWNER) != h || GetWindow(h, GW_OWNER) != nullptr;
    DWORD cloaked = 0;  // hidden UWP shells, windows on other virtual desktops
    f.cloaked = SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked;
    f.titleLen = GetWindowTextLengthW(h);
    wchar_t cls[128] = {};
    GetClassNameW(h, cls, 128);
    f.cls = cls;
    f.iconic = IsIconic(h) != 0;
    RECT r{};
    if (frameRect(h, r)) { f.w = r.right - r.left; f.h = r.bottom - r.top; }
    return f;
}
bool capturable(HWND h) { return pickReject(factsOf(h)) == nullptr; }
std::wstring fileDescription(HWND h) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    std::wstring path;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t b[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, b, &n)) path = b;
        CloseHandle(p);
    }
    if (path.empty()) return L"";
    DWORD dummy = 0, sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!sz || sz > (1u << 20)) return L"";
    std::vector<BYTE> buf(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, buf.data())) return L"";
    struct LangCp { WORD lang, cp; }* tr = nullptr; UINT trLen = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &trLen) || trLen < sizeof(LangCp)) return L"";
    wchar_t q[80]; swprintf(q, 80, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].cp);
    wchar_t* v = nullptr; UINT vl = 0;
    if (VerQueryValueW(buf.data(), q, (void**)&v, &vl) && v && vl) return std::wstring(v);
    return L"";
}
HICON iconOf(HWND h) {
    DWORD_PTR ic = 0;
    if (!SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 50, &ic) || !ic)
        SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 50, &ic);
    if (!ic) ic = (DWORD_PTR)GetClassLongPtrW(h, GCLP_HICON);
    if (!ic) ic = (DWORD_PTR)GetClassLongPtrW(h, GCLP_HICONSM);
    return (HICON)ic;
}
struct EnumCtx { std::vector<CaptureWindow>* v; std::map<DWORD, std::pair<std::wstring, std::wstring>>* names; };
BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<EnumCtx*>(lp);
    if (!capturable(h)) return TRUE;
    CaptureWindow w;
    w.hwnd = h;
    int n = GetWindowTextLengthW(h);
    w.title.resize((size_t)n + 1);
    w.title.resize((size_t)GetWindowTextW(h, &w.title[0], n + 1));
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    auto it = c->names->find(pid);
    if (it == c->names->end()) {
        std::wstring exe = exeOf(h);
        it = c->names->emplace(pid, std::make_pair(exe, appDisplayName(exe, fileDescription(h)))).first;
    }
    w.exe = it->second.first;
    w.app = it->second.second;
    w.page = stripAppSuffix(w.title, w.exe, w.app);
    w.icon = iconOf(h);
    w.minimized = IsIconic(h) != 0;
    frameRect(h, w.rect);
    c->v->push_back(std::move(w));
    return TRUE;
}
}  // namespace

void includeOwnWindowsForTest(bool on) { g_includeOwn = on; }
std::wstring CaptureWindow::label() const { return windowLabel(app, page); }
const char* captureRejectReason(HWND h) { return IsWindow(h) ? pickReject(factsOf(h)) : "not a window"; }

std::vector<CaptureWindow> listCaptureWindows() {
    std::vector<CaptureWindow> raw;
    std::map<DWORD, std::pair<std::wstring, std::wstring>> names;
    EnumCtx c{&raw, &names};
    EnumWindows(enumProc, (LPARAM)&c);
    // Same app + same title (two windows on the same page): keep both and number the later ones "(2)", "(3)", ...
    // so each stays selectable. Then group by app in first-seen z-order.
    std::vector<CaptureWindow> u;
    for (auto& w : raw) {
        int same = 0;
        for (auto& x : u) if (lower(x.exe) == lower(w.exe) && x.title == w.title) ++same;
        u.push_back(w);
        if (same) u.back().page = (w.page.empty() ? std::wstring(L"window") : w.page) + L" (" + std::to_wstring(same + 1) + L")";
    }
    std::vector<CaptureWindow> v;
    std::vector<bool> used(u.size(), false);
    for (size_t i = 0; i < u.size(); ++i) {
        if (used[i]) continue;
        for (size_t j = i; j < u.size(); ++j)
            if (!used[j] && lower(u[j].exe) == lower(u[i].exe)) { used[j] = true; v.push_back(u[j]); }
    }
    return v;
}

HWND findCaptureWindow(const std::wstring& q) {
    std::wstring l = lower(q);
    for (auto& w : listCaptureWindows())
        if (lower(w.title).find(l) != std::wstring::npos || lower(w.exe).find(l) != std::wstring::npos || lower(w.app) == l) return w.hwnd;
    return nullptr;
}
std::wstring windowExe(HWND h) { return exeOf(h); }

HWND resolveSavedWindow(const std::wstring& exe, const std::wstring& title) {
    if (exe.empty()) return nullptr;
    auto ws = listCaptureWindows();
    std::wstring le = lower(exe);
    for (auto& w : ws) if (lower(w.exe) == le && w.title == title) return w.hwnd;
    HWND best = nullptr; long long ba = -1;  // same app: its biggest real window (not the first popup in z-order)
    for (auto& w : ws)
        if (lower(w.exe) == le) {
            long long a = (long long)(w.rect.right - w.rect.left) * (w.rect.bottom - w.rect.top);
            if (!w.minimized) a += 1LL << 40;  // prefer a window that is on screen
            if (a > ba) { ba = a; best = w.hwnd; }
        }
    return best;
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
