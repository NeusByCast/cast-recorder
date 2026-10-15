#pragma once
// Pure (platform-free) rules for the window source picker, unit-tested on Linux (tests/test_main.cpp):
//  - which top-level windows are real app windows (vs. tooltips, hover cards, media popups, owned dialogs)
//  - the label: app name first ("Brave"), page title second with the " - Brave" suffix removed.
#include <cstdint>
#include <cwctype>
#include <string>

namespace cr {

// Window facts gathered by the Win32 side (styles are raw GWL_STYLE / GWL_EXSTYLE bits).
struct WinFacts {
    bool visible = true, ownProcess = false, cloaked = false, iconic = false, child = false;
    bool owned = false;            // GA_ROOTOWNER != self (owned popup / dialog / bubble)
    uint32_t style = 0, exStyle = 0;
    std::wstring cls;              // window class
    int titleLen = 0;
    int w = 0, h = 0;              // DWM extended frame bounds (or restored placement when iconic)
};
namespace ws {
constexpr uint32_t POPUP = 0x80000000u, CHILD = 0x40000000u, CAPTION = 0x00C00000u, SYSMENU = 0x00080000u,
                   THICKFRAME = 0x00040000u, MINIMIZEBOX = 0x00020000u;
constexpr uint32_t EX_TOOLWINDOW = 0x00000080u, EX_APPWINDOW = 0x00040000u, EX_NOACTIVATE = 0x08000000u,
                   EX_TOPMOST = 0x00000008u, EX_LAYERED = 0x00080000u, EX_TRANSPARENT = 0x00000020u;
}
constexpr int kMinPickW = 120, kMinPickH = 80;

// Returns nullptr when the window is pickable, else a short reason (for logs/tests).
inline const char* pickReject(const WinFacts& f) {
    if (!f.visible) return "hidden";
    if (f.ownProcess) return "own process";
    if (f.child || (f.style & ws::CHILD)) return "child";
    if (f.cloaked) return "cloaked";
    if (f.titleLen <= 0) return "untitled";
    const bool app = (f.exStyle & ws::EX_APPWINDOW) != 0;
    if (f.exStyle & ws::EX_TOOLWINDOW) return "tool window";
    if ((f.exStyle & ws::EX_NOACTIVATE) && !app) return "no-activate popup";
    if (f.owned && !app) return "owned popup";
    if ((f.exStyle & ws::EX_TRANSPARENT) && (f.exStyle & ws::EX_LAYERED) && !app) return "click-through overlay";
    if ((f.style & ws::POPUP) && !app && !(f.style & (ws::CAPTION | ws::THICKFRAME | ws::MINIMIZEBOX | ws::SYSMENU)))
        return "bare popup";
    static const wchar_t* deny[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"tooltips_class32",
                                    L"#32768", L"TaskListThumbnailWnd", L"NotifyIconOverflowWindow", L"XamlExplorerHostIslandWindow",
                                    L"Windows.UI.Core.CoreWindow", L"ForegroundStaging", L"MultitaskingViewFrame", L"TopLevelWindowForOverflowXamlIsland"};
    for (auto d : deny) if (f.cls == d) return "shell window";
    if (f.w < kMinPickW || f.h < kMinPickH) return "too small";
    return nullptr;
}

inline std::wstring lowerW(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
inline std::wstring exeStem(const std::wstring& exe) {
    std::wstring s = exe;
    size_t k = s.find_last_of(L"\\/"); if (k != std::wstring::npos) s = s.substr(k + 1);
    if (s.size() > 4 && lowerW(s.substr(s.size() - 4)) == L".exe") s.resize(s.size() - 4);
    return s;
}
// Friendly app name: well-known apps, else the exe's FileDescription (if short and sane), else the exe stem capitalized.
inline std::wstring appDisplayName(const std::wstring& exe, const std::wstring& fileDescription) {
    static const struct { const wchar_t* exe; const wchar_t* name; } known[] = {
        {L"brave", L"Brave"}, {L"chrome", L"Chrome"}, {L"msedge", L"Edge"}, {L"firefox", L"Firefox"}, {L"opera", L"Opera"},
        {L"opera_gx", L"Opera GX"}, {L"vivaldi", L"Vivaldi"}, {L"arc", L"Arc"}, {L"zen", L"Zen"}, {L"librewolf", L"LibreWolf"},
        {L"discord", L"Discord"}, {L"spotify", L"Spotify"}, {L"code", L"VS Code"}, {L"explorer", L"File Explorer"},
        {L"notepad", L"Notepad"}, {L"obs64", L"OBS Studio"}, {L"steam", L"Steam"}, {L"steamwebhelper", L"Steam"},
        {L"slack", L"Slack"}, {L"teams", L"Teams"}, {L"ms-teams", L"Teams"}, {L"vlc", L"VLC"}, {L"windowsterminal", L"Terminal"}};
    std::wstring stem = exeStem(exe), ls = lowerW(stem);
    for (auto& k : known) if (ls == k.exe) return k.name;
    std::wstring d = fileDescription;
    while (!d.empty() && iswspace(d.back())) d.pop_back();
    while (!d.empty() && iswspace(d.front())) d.erase(d.begin());
    if (!d.empty() && d.size() <= 32 && lowerW(d) != ls + L".exe") return d;
    if (stem.empty()) return L"Window";
    stem[0] = (wchar_t)towupper(stem[0]);
    return stem;
}
// "Cast Recorder v1.0 release - Brave" -> "Cast Recorder v1.0 release". Handles -, – and — separators, the
// zero-width space Edge puts in "Microsoft\u200BEdge", and browser profile segments ("... - Work - Microsoft Edge").
inline std::wstring stripAppSuffix(const std::wstring& title, const std::wstring& exe, const std::wstring& appName) {
    std::wstring t;
    for (wchar_t c : title) if (c != 0x200B && c != 0x200E && c != 0x200F) t += c;
    std::wstring stem = lowerW(exeStem(exe));
    const bool edge = stem == L"msedge";
    // longest first: "Mozilla Firefox Private Browsing" before "Mozilla Firefox"
    std::wstring cands[] = {L"mozilla firefox private browsing", L"firefox private browsing", lowerW(appName), stem, L"google chrome",
                            L"microsoft edge", L"mozilla firefox", L"brave", L"opera", L"vivaldi", L"visual studio code"};
    std::wstring lt = lowerW(t);
    const wchar_t* seps[] = {L" - ", L" \u2013 ", L" \u2014 "};
    for (auto& c : cands) {
        if (c.empty()) continue;
        bool hit = false;
        for (const wchar_t* sep : seps) {
            std::wstring suf = sep + c;
            if (lt.size() > suf.size() && lt.compare(lt.size() - suf.size(), suf.size(), suf) == 0) { t.resize(t.size() - suf.size()); hit = true; break; }
            // Chrome / Brave with a custom profile: "Inbox - Google Chrome - John" (app name, then the profile last)
            std::wstring mid = suf + sep;
            size_t m = lt.rfind(mid);
            if (m != std::wstring::npos && m > 0) {
                std::wstring tail = lt.substr(m + mid.size());
                bool plain = !tail.empty() && tail.size() <= 40;
                for (const wchar_t* s2 : seps) if (tail.find(s2) != std::wstring::npos) plain = false;
                if (plain) { t.resize(m); hit = true; break; }
            }
        }
        if (!hit) continue;
        // Profile segment right before the app name ("... - Personal - Microsoft Edge"). Edge always puts the
        // profile there (custom names too: "Inbox - John (Acme) - Microsoft Edge"); others only the known ones.
        size_t k = t.rfind(L" - ");
        if (k != std::wstring::npos) {
            std::wstring seg = lowerW(t.substr(k + 3));
            bool prof = edge && seg.size() <= 40 && k > 0;
            for (const wchar_t* pf : {L"personal", L"work", L"school", L"guest", L"inprivate", L"incognito", L"private"})
                if (seg == pf) prof = true;
            if (prof) t.resize(k);
        }
        // Edge: "Page and 3 more pages"
        if (edge) {
            std::wstring l2 = lowerW(t);
            size_t a2 = l2.rfind(L" and ");
            if (a2 != std::wstring::npos) {
                size_t i = a2 + 5, d0 = i;
                while (i < l2.size() && iswdigit(l2[i])) ++i;
                std::wstring rest = l2.substr(i);
                if (i > d0 && (rest == L" more page" || rest == L" more pages")) t.resize(a2);
            }
        }
        break;
    }
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    if (t.empty() || lowerW(t) == lowerW(appName)) return L"";
    return t;
}
// Picker / source label: "Brave · Cast Recorder v1.0 release" (title part may be empty).
inline std::wstring windowLabel(const std::wstring& appName, const std::wstring& pageTitle) {
    return pageTitle.empty() ? appName : appName + L" \u00B7 " + pageTitle;
}
}  // namespace cr
