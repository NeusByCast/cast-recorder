// Cast Recorder: borderless dark settings window, tray icon, global hotkeys, headless CLI for tests.
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include "../core/hotkey.h"
#include "../core/settings.h"
#include "recorder.h"
#include "resource.h"

using namespace cr;

namespace {

// ---------- utilities ----------
std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string U(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}
std::wstring settingsPath() {
    std::wstring d = knownFolder(FOLDERID_RoamingAppData) + L"\\Cast Recorder";
    SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
    return d + L"\\settings.json";
}
std::wstring defaultOutputDir() { return knownFolder(FOLDERID_Videos) + L"\\Cast Recorder"; }

// ---------- palette ----------
const COLORREF C_BG = RGB(17, 18, 23), C_TITLE = RGB(13, 14, 18), C_CARD = RGB(26, 28, 35), C_BORDER = RGB(40, 43, 53),
               C_TEXT = RGB(233, 235, 241), C_MUTED = RGB(140, 146, 162), C_ACCENT = RGB(99, 102, 241),
               C_ACCENT_H = RGB(120, 123, 255), C_SEG = RGB(34, 37, 46), C_SEG_H = RGB(44, 47, 58),
               C_REC = RGB(239, 68, 68), C_REC_H = RGB(248, 96, 96), C_WARN = RGB(245, 180, 60), C_ERR = RGB(248, 113, 113),
               C_OK = RGB(74, 222, 128), C_CLOSE_H = RGB(196, 43, 28);

enum Hit {
    H_NONE = 0, H_MIN, H_CLOSE, H_RECORD, H_FPS0, H_QUAL0 = H_FPS0 + 3, H_ENC0 = H_QUAL0 + 4, H_CONT0 = H_ENC0 + 5,
    H_HK0 = H_CONT0 + 3, H_HK1, H_BROWSE, H_OPEN, H_CURSOR, H_RESETDIR
};
struct Region { RECT rc; int id; };

const int kFps[] = {15, 30, 60};
const wchar_t* kFpsL[] = {L"15", L"30", L"60"};
const Quality kQual[] = {Quality::Low, Quality::Medium, Quality::High, Quality::Ultra};
const wchar_t* kQualL[] = {L"Low", L"Medium", L"High", L"Ultra"};
const Encoder kEnc[] = {Encoder::Auto, Encoder::NVENC, Encoder::AMF, Encoder::QSV, Encoder::Software};
const wchar_t* kEncL[] = {L"Auto", L"NVENC", L"AMF", L"QuickSync", L"Software"};
const Container kCont[] = {Container::MP4, Container::MKV, Container::MOV};
const wchar_t* kContL[] = {L"MP4", L"MKV", L"MOV"};

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr int HK_TOGGLE = 1, HK_FOLDER = 2;
constexpr UINT TIMER_TICK = 1;
constexpr int BASE_W = 560, BASE_H = 700;

struct App {
    HINSTANCE inst{};
    HWND hwnd{};
    UINT dpi = 96;
    HFONT fTitle{}, fBody{}, fSmall{}, fHead{}, fBig{}, fIcon{};
    HICON icoBig{}, icoSmall{}, icoTray{}, icoTrayRec{};
    Settings s;
    std::wstring settingsFile;
    Recorder rec;
    std::vector<Region> regions;
    int hover = H_NONE, pressed = H_NONE;
    int capturing = -1;         // hotkey field being edited (0/1), -1 none
    UINT liveMods = 0;
    std::wstring hkMsg[2];
    COLORREF hkMsgColor[2]{};
    std::wstring status, hwInfo;
    COLORREF statusColor = C_MUTED;
    bool trayAdded = false, quitting = false;
    UINT taskbarCreated = 0;
} g;

int S(int v) { return MulDiv(v, (int)g.dpi, 96); }

HFONT mkFont(int px, int weight, const wchar_t* face = L"Segoe UI Variable Text") {
    HFONT f = CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    return f ? f : CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}
void makeFonts() {
    for (HFONT f : {g.fTitle, g.fBody, g.fSmall, g.fHead, g.fBig, g.fIcon}) if (f) DeleteObject(f);
    g.fTitle = mkFont(13, FW_SEMIBOLD);
    g.fBody = mkFont(14, FW_NORMAL);
    g.fSmall = mkFont(12, FW_NORMAL);
    g.fHead = mkFont(12, FW_BOLD);
    g.fBig = mkFont(16, FW_SEMIBOLD);
    g.fIcon = CreateFontW(-S(10), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe MDL2 Assets");
}

// Tray icon with a red recording dot composited on the app icon.
HICON makeRecIcon(HICON base, int size) {
    HDC screen = GetDC(nullptr);
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi; bi.bV5Width = size; bi.bV5Height = -size; bi.bV5Planes = 1; bi.bV5BitCount = 32; bi.bV5Compression = BI_RGB;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(dc, color);
    memset(bits, 0, (size_t)size * size * 4);
    DrawIconEx(dc, 0, 0, base, size, size, 0, nullptr, DI_NORMAL);
    // Red dot, bottom-right, with a dark ring; written straight into premultiplied ARGB.
    uint32_t* px = (uint32_t*)bits;
    double r = size * 0.24, cx = size - r - 0.5, cy = size - r - 0.5;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            double d = sqrt((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy));
            if (d <= r) px[y * size + x] = 0xFFEF4444;
            else if (d <= r + size / 16.0 + 0.5) px[y * size + x] = 0xFF111217;
        }
    SelectObject(dc, old);
    DeleteDC(dc);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    ReleaseDC(nullptr, screen);
    return ic;
}

void loadIcons() {
    int big = GetSystemMetrics(SM_CXICON), smallSz = GetSystemMetrics(SM_CXSMICON);
    g.icoBig = (HICON)LoadImageW(g.inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, big, big, 0);
    g.icoSmall = (HICON)LoadImageW(g.inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, smallSz, smallSz, 0);
    g.icoTray = g.icoSmall;
    g.icoTrayRec = makeRecIcon(g.icoSmall, smallSz);
}

// ---------- tray ----------
void trayUpdate(DWORD msg) {
    NOTIFYICONDATAW n{sizeof n};
    n.hWnd = g.hwnd; n.uID = 1; n.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE; n.uCallbackMessage = WM_TRAY;
    n.hIcon = g.rec.running() ? g.icoTrayRec : g.icoTray;
    std::wstring tip = g.rec.running() ? L"Cast Recorder \u2013 Recording" : L"Cast Recorder \u2013 Ready";
    if (!g.s.toggleRecord.empty()) tip += L" (" + W(format(g.s.toggleRecord)) + L")";
    wcsncpy(n.szTip, tip.c_str(), ARRAYSIZE(n.szTip) - 1);
    if (Shell_NotifyIconW(msg, &n) && msg == NIM_ADD) g.trayAdded = true;
}

// ---------- hotkeys ----------
void unregisterAll() { UnregisterHotKey(g.hwnd, HK_TOGGLE); UnregisterHotKey(g.hwnd, HK_FOLDER); }
void registerAll() {
    unregisterAll();
    Hotkey* keys[2] = {&g.s.toggleRecord, &g.s.openFolder};
    auto conflicts = findConflicts({{"toggle", g.s.toggleRecord}, {"folder", g.s.openFolder}});
    for (int i = 0; i < 2; ++i) {
        const Hotkey& k = *keys[i];
        g.hkMsg[i].clear();
        if (k.empty()) { g.hkMsg[i] = L"Not set"; g.hkMsgColor[i] = C_MUTED; continue; }
        Issue is = validate(k);
        if (is.severity == Severity::Error) { g.hkMsg[i] = W(is.message); g.hkMsgColor[i] = C_ERR; continue; }
        if (!conflicts.empty() && i == 1) { g.hkMsg[i] = L"Same combo as Start / stop recording"; g.hkMsgColor[i] = C_ERR; continue; }
        // MOD_NOREPEAT: holding the combo doesn't retrigger.
        if (!RegisterHotKey(g.hwnd, i == 0 ? HK_TOGGLE : HK_FOLDER, k.mods | MOD_NOREPEAT, k.vk)) {
            g.hkMsg[i] = L"In use by another app \u2013 pick a different combo";
            g.hkMsgColor[i] = C_ERR;
            continue;
        }
        if (is.severity == Severity::Warning) { g.hkMsg[i] = W(is.message); g.hkMsgColor[i] = C_WARN; }
        else { g.hkMsg[i] = L"Active, works while games have focus"; g.hkMsgColor[i] = C_OK; }
    }
}

void save() { saveFile(U(g.settingsFile), g.s); }

std::wstring outputDir() { return g.s.outputDir.empty() ? defaultOutputDir() : W(g.s.outputDir); }

void setStatus(const std::wstring& s, COLORREF c) { g.status = s; g.statusColor = c; InvalidateRect(g.hwnd, nullptr, FALSE); }

// ---------- recording ----------
void startRecording() {
    if (g.rec.running()) return;
    std::wstring dir = outputDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) { setStatus(L"Can't create the save folder", C_ERR); return; }
    wchar_t name[64];
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    wcsftime(name, 64, L"Cast %Y-%m-%d %H-%M-%S", &lt);
    RecordOptions o;
    o.fps = g.s.fps; o.quality = g.s.quality; o.encoder = g.s.encoder; o.container = g.s.container;
    o.captureCursor = g.s.captureCursor; o.notify = g.hwnd;
    o.outputPath = dir + L"\\" + name + W(extension(g.s.container));
    g.rec.start(o);
    SetTimer(g.hwnd, TIMER_TICK, 250, nullptr);
    setStatus(L"Recording\u2026", C_REC);
    trayUpdate(NIM_MODIFY);
}
void stopRecording() { if (g.rec.running()) { g.rec.stop(); setStatus(L"Finishing file\u2026", C_MUTED); } }
void toggleRecording() { g.rec.running() ? stopRecording() : startRecording(); }
void openFolder() {
    std::wstring d = outputDir();
    SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void pickFolder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose where recordings are saved");
    IShellItem* cur = nullptr;
    std::wstring d = outputDir();
    SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
    if (SUCCEEDED(SHCreateItemFromParsingName(d.c_str(), nullptr, IID_PPV_ARGS(&cur)))) { dlg->SetFolder(cur); cur->Release(); }
    if (SUCCEEDED(dlg->Show(g.hwnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                g.s.outputDir = U(p) == U(defaultOutputDir()) ? std::string() : U(p);
                CoTaskMemFree(p);
                save();
            }
            it->Release();
        }
    }
    dlg->Release();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// ---------- painting ----------
void fill(HDC dc, const RECT& r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
void round(HDC dc, const RECT& r, COLORREF fillC, COLORREF border, int rad) {
    HBRUSH b = CreateSolidBrush(fillC);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(rad), S(rad));
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}
void text(HDC dc, const std::wstring& t, RECT r, HFONT f, COLORREF c, UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, t.c_str(), (int)t.size(), &r, fmt | DT_NOPREFIX);
}
void addRegion(RECT r, int id) { g.regions.push_back({r, id}); }

void segmented(HDC dc, int x, int y, int w, int idBase, const wchar_t* const* labels, int n, int sel) {
    int h = S(32), gap = S(4);
    RECT outer{x, y, x + w, y + h};
    round(dc, outer, C_SEG, C_BORDER, 10);
    int segW = (w - S(6) - gap * (n - 1)) / n;
    for (int i = 0; i < n; ++i) {
        RECT r{x + S(3) + i * (segW + gap), y + S(3), 0, y + h - S(3)};
        r.right = r.left + segW;
        bool on = i == sel, hov = g.hover == idBase + i;
        if (on) round(dc, r, hov ? C_ACCENT_H : C_ACCENT, on ? C_ACCENT : C_SEG, 8);
        else if (hov) round(dc, r, C_SEG_H, C_SEG_H, 8);
        text(dc, labels[i], r, g.fBody, on ? RGB(255, 255, 255) : C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        addRegion(r, idBase + i);
    }
}

void button(HDC dc, RECT r, int id, const wchar_t* label, bool primary) {
    bool hov = g.hover == id;
    COLORREF bg = primary ? (hov ? C_ACCENT_H : C_ACCENT) : (hov ? C_SEG_H : C_SEG);
    round(dc, r, bg, primary ? bg : C_BORDER, 10);
    text(dc, label, r, g.fBody, primary ? RGB(255, 255, 255) : C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    addRegion(r, id);
}

std::wstring modsText(UINT m) {
    std::wstring s;
    if (m & MOD_CTRL_) s += L"Ctrl+";
    if (m & MOD_ALT_) s += L"Alt+";
    if (m & MOD_SHIFT_) s += L"Shift+";
    if (m & MOD_WIN_) s += L"Win+";
    return s;
}

void paint(HDC dc, RECT client) {
    g.regions.clear();
    fill(dc, client, C_BG);
    SetBkMode(dc, TRANSPARENT);
    const int W_ = client.right, pad = S(24);

    // Title bar
    RECT tb{0, 0, W_, S(44)};
    fill(dc, tb, C_TITLE);
    DrawIconEx(dc, S(16), S(12), g.icoSmall, S(20), S(20), 0, nullptr, DI_NORMAL);
    text(dc, L"Cast Recorder", RECT{S(46), 0, W_ - S(100), S(44)}, g.fTitle, C_TEXT);
    RECT rmin{W_ - S(92), 0, W_ - S(46), S(44)}, rclose{W_ - S(46), 0, W_, S(44)};
    if (g.hover == H_MIN) fill(dc, rmin, C_SEG_H);
    if (g.hover == H_CLOSE) fill(dc, rclose, C_CLOSE_H);
    text(dc, L"\xE921", rmin, g.fIcon, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(dc, L"\xE8BB", rclose, g.fIcon, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    addRegion(rmin, H_MIN);
    addRegion(rclose, H_CLOSE);

    int y = S(44) + S(20);
    // Record card
    RECT card{pad, y, W_ - pad, y + S(84)};
    round(dc, card, C_CARD, C_BORDER, 14);
    bool rec = g.rec.running();
    std::wstring head = rec ? L"Recording" : L"Ready to record";
    wchar_t sub[160];
    if (rec) {
        int sec = (int)g.rec.elapsedSeconds();
        swprintf(sub, 160, L"%02d:%02d:%02d  \u00b7  %d FPS  \u00b7  %ls", sec / 3600, sec / 60 % 60, sec % 60, g.s.fps, kEncL[(int)g.s.encoder]);
    } else {
        swprintf(sub, 160, L"%ls to start or stop", g.s.toggleRecord.empty() ? L"Set a hotkey below" : W(format(g.s.toggleRecord)).c_str());
    }
    if (rec) {
        HBRUSH b = CreateSolidBrush(C_REC); HGDIOBJ ob = SelectObject(dc, b); HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, card.left + S(20), card.top + S(26), card.left + S(30), card.top + S(36));
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b);
    }
    int tx = card.left + (rec ? S(38) : S(20));
    text(dc, head, RECT{tx, card.top + S(16), card.right - S(170), card.top + S(46)}, g.fBig, C_TEXT);
    text(dc, sub, RECT{card.left + S(20), card.top + S(44), card.right - S(170), card.top + S(70)}, g.fSmall, C_MUTED);
    RECT rb{card.right - S(156), card.top + S(22), card.right - S(20), card.top + S(62)};
    bool hov = g.hover == H_RECORD;
    round(dc, rb, rec ? (hov ? C_SEG_H : C_SEG) : (hov ? C_REC_H : C_REC), rec ? C_BORDER : (hov ? C_REC_H : C_REC), 12);
    text(dc, rec ? L"Stop" : L"Start recording", rb, g.fBody, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    addRegion(rb, H_RECORD);
    y = card.bottom + S(24);

    const int labelW = S(120), cx = pad + labelW, cw = W_ - pad - cx;
    auto section = [&](const wchar_t* t) {
        text(dc, t, RECT{pad, y, W_ - pad, y + S(18)}, g.fHead, C_MUTED);
        y += S(26);
    };
    auto label = [&](const wchar_t* t) { text(dc, t, RECT{pad, y, cx, y + S(32)}, g.fBody, C_TEXT); };

    section(L"VIDEO");
    int sel = 2;
    for (int i = 0; i < 3; ++i) if (kFps[i] == g.s.fps) sel = i;
    label(L"Frame rate"); segmented(dc, cx, y, cw, H_FPS0, kFpsL, 3, sel); y += S(42);
    label(L"Quality"); segmented(dc, cx, y, cw, H_QUAL0, kQualL, 4, (int)g.s.quality); y += S(42);
    label(L"Encoder"); segmented(dc, cx, y, cw, H_ENC0, kEncL, 5, (int)g.s.encoder); y += S(36);
    text(dc, g.hwInfo, RECT{cx, y, W_ - pad, y + S(18)}, g.fSmall, C_MUTED); y += S(26);
    label(L"Format"); segmented(dc, cx, y, cw, H_CONT0, kContL, 3, (int)g.s.container); y += S(36);
    {
        std::wstring note = g.s.container == Container::MP4 ? L"Native MP4, no extra tools needed"
                          : (findFfmpeg().empty() ? L"Needs ffmpeg.exe next to the app or on PATH, otherwise saved as MP4"
                                                  : L"Recorded as MP4, then remuxed losslessly with ffmpeg");
        text(dc, note, RECT{cx, y, W_ - pad, y + S(18)}, g.fSmall, C_MUTED);
        y += S(30);
    }

    section(L"HOTKEYS");
    const wchar_t* hkLabels[2] = {L"Start / stop", L"Open folder"};
    const Hotkey* hks[2] = {&g.s.toggleRecord, &g.s.openFolder};
    for (int i = 0; i < 2; ++i) {
        label(hkLabels[i]);
        RECT f{cx, y, W_ - pad, y + S(34)};
        bool focus = g.capturing == i, h2 = g.hover == H_HK0 + i;
        round(dc, f, focus ? RGB(30, 32, 44) : C_SEG, focus ? C_ACCENT : (h2 ? RGB(60, 64, 78) : C_BORDER), 10);
        RECT ft{f.left + S(12), f.top, f.right - S(12), f.bottom};
        if (focus) text(dc, g.liveMods ? modsText(g.liveMods) + L"\u2026" : L"Press a key combination\u2026   Esc cancels, Backspace clears", ft, g.fBody, focus && g.liveMods ? C_TEXT : C_MUTED);
        else text(dc, hks[i]->empty() ? L"Click and press a combo" : W(format(*hks[i])), ft, g.fBody, hks[i]->empty() ? C_MUTED : C_TEXT);
        addRegion(f, H_HK0 + i);
        y += S(38);
        text(dc, g.hkMsg[i], RECT{cx, y, W_ - pad, y + S(18)}, g.fSmall, g.hkMsgColor[i]);
        y += S(26);
    }
    y += S(4);

    section(L"OUTPUT");
    label(L"Save to");
    RECT pf{cx, y, W_ - pad - S(176), y + S(34)};
    round(dc, pf, C_SEG, C_BORDER, 10);
    text(dc, outputDir(), RECT{pf.left + S(12), pf.top, pf.right - S(10), pf.bottom}, g.fBody, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    button(dc, RECT{W_ - pad - S(168), y, W_ - pad - S(86), y + S(34)}, H_BROWSE, L"Change\u2026", false);
    button(dc, RECT{W_ - pad - S(78), y, W_ - pad, y + S(34)}, H_OPEN, L"Open", false);
    y += S(40);
    if (!g.s.outputDir.empty()) {
        RECT rr{cx, y, cx + S(150), y + S(18)};
        text(dc, L"Reset to default", rr, g.fSmall, g.hover == H_RESETDIR ? C_ACCENT_H : C_ACCENT);
        addRegion(rr, H_RESETDIR);
    }
    y += S(26);
    // Cursor toggle
    label(L"Show cursor");
    RECT sw{cx, y + S(6), cx + S(40), y + S(26)};
    round(dc, sw, g.s.captureCursor ? C_ACCENT : C_SEG_H, g.s.captureCursor ? C_ACCENT : C_BORDER, 20);
    int kx = g.s.captureCursor ? sw.right - S(18) : sw.left + S(2);
    HBRUSH kb = CreateSolidBrush(RGB(255, 255, 255)); HGDIOBJ ob = SelectObject(dc, kb); HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, kx, sw.top + S(2), kx + S(16), sw.top + S(18));
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(kb);
    addRegion(RECT{sw.left, sw.top, sw.right + S(10), sw.bottom}, H_CURSOR);

    // Status footer
    RECT foot{0, client.bottom - S(36), W_, client.bottom};
    fill(dc, foot, C_TITLE);
    text(dc, g.status, RECT{pad, foot.top, W_ - pad, foot.bottom}, g.fSmall, g.statusColor, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

int hitTest(POINT p) {
    for (auto it = g.regions.rbegin(); it != g.regions.rend(); ++it) if (PtInRect(&it->rc, p)) return it->id;
    return H_NONE;
}

void endCapture() {
    g.capturing = -1;
    g.liveMods = 0;
    registerAll();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

UINT currentMods() {
    UINT m = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= MOD_CTRL_;
    if (GetKeyState(VK_MENU) & 0x8000) m |= MOD_ALT_;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= MOD_SHIFT_;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= MOD_WIN_;
    return m;
}

// Returns true if the key was consumed by the hotkey field.
bool hotkeyKey(UINT vk, bool down) {
    if (g.capturing < 0) return false;
    Hotkey* dst = g.capturing == 0 ? &g.s.toggleRecord : &g.s.openFolder;
    UINT mods = currentMods();
    if (isModifierKey(vk)) { g.liveMods = mods; InvalidateRect(g.hwnd, nullptr, FALSE); return true; }
    if (!down && vk != VK_SNAPSHOT) return true;  // PrintScreen only reports key-up
    if (mods == 0 && vk == VK_ESCAPE) { endCapture(); return true; }
    if (mods == 0 && (vk == VK_BACK || vk == VK_DELETE)) { *dst = Hotkey{}; save(); endCapture(); return true; }
    Hotkey hk{mods, vk};
    if (keyName(vk).empty()) { g.hkMsg[g.capturing] = L"That key isn't supported"; g.hkMsgColor[g.capturing] = C_ERR; return true; }
    int idx = g.capturing;
    Issue is = validate(hk);
    if (is.severity == Severity::Error) {
        g.capturing = -1; g.liveMods = 0;
        registerAll();
        g.hkMsg[idx] = W(format(hk)) + L": " + W(is.message);
        g.hkMsgColor[idx] = C_ERR;
        InvalidateRect(g.hwnd, nullptr, FALSE);
        return true;
    }
    const Hotkey& other = idx == 0 ? g.s.openFolder : g.s.toggleRecord;
    if (hk == other) {
        g.capturing = -1; g.liveMods = 0;
        registerAll();
        g.hkMsg[idx] = W(format(hk)) + (idx == 0 ? L" is already used for Open folder" : L" is already used for Start / stop");
        g.hkMsgColor[idx] = C_ERR;
        InvalidateRect(g.hwnd, nullptr, FALSE);
        return true;
    }
    *dst = hk;
    save();
    endCapture();
    trayUpdate(NIM_MODIFY);
    return true;
}

void click(int id) {
    if (id != H_HK0 && id != H_HK1 && g.capturing >= 0) endCapture();
    if (id == H_MIN) ShowWindow(g.hwnd, SW_MINIMIZE);
    else if (id == H_CLOSE) ShowWindow(g.hwnd, SW_HIDE);  // keeps running in the tray
    else if (id == H_RECORD) toggleRecording();
    else if (id >= H_FPS0 && id < H_FPS0 + 3) { g.s.fps = kFps[id - H_FPS0]; save(); }
    else if (id >= H_QUAL0 && id < H_QUAL0 + 4) { g.s.quality = kQual[id - H_QUAL0]; save(); }
    else if (id >= H_ENC0 && id < H_ENC0 + 5) { g.s.encoder = kEnc[id - H_ENC0]; save(); }
    else if (id >= H_CONT0 && id < H_CONT0 + 3) { g.s.container = kCont[id - H_CONT0]; save(); }
    else if (id == H_HK0 || id == H_HK1) {
        g.capturing = id - H_HK0;
        g.liveMods = 0;
        unregisterAll();  // so pressing the current combo is captured, not triggered
        SetFocus(g.hwnd);
    } else if (id == H_BROWSE) pickFolder();
    else if (id == H_OPEN) openFolder();
    else if (id == H_RESETDIR) { g.s.outputDir.clear(); save(); }
    else if (id == H_CURSOR) { g.s.captureCursor = !g.s.captureCursor; save(); }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void showWindow() {
    ShowWindow(g.hwnd, IsIconic(g.hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g.hwnd);
}

void trayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, g.rec.running() ? L"Stop recording" : L"Start recording");
    AppendMenuW(m, MF_STRING, 2, L"Open recordings folder");
    AppendMenuW(m, MF_STRING, 3, L"Settings");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 4, L"Exit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g.hwnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    if (c == 1) toggleRecording();
    else if (c == 2) openFolder();
    else if (c == 3) showWindow();
    else if (c == 4) { g.quitting = true; if (g.rec.running()) g.rec.stop(); else DestroyWindow(g.hwnd); }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g.taskbarCreated && g.taskbarCreated) { trayUpdate(NIM_ADD); return 0; }
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0;  // whole window is client area (borderless)
        break;
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &p);
        if (p.y < S(44) && hitTest(p) == H_NONE) return HTCAPTION;  // drag
        return HTCLIENT;
    }
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        makeFonts();
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int id = hitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (id != g.hover) {
            g.hover = id;
            InvalidateRect(h, nullptr, FALSE);
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
        }
        SetCursor(LoadCursor(nullptr, id != H_NONE ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) return TRUE;  // handled in WM_MOUSEMOVE
        break;
    case WM_MOUSELEAVE: g.hover = H_NONE; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: g.pressed = hitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}); SetCapture(h); return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        int id = hitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (id != H_NONE && id == g.pressed) click(id);
        else if (id == H_NONE && g.capturing >= 0) endCapture();
        g.pressed = H_NONE;
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (hotkeyKey((UINT)wp, true)) return 0;
        if (wp == VK_ESCAPE) { ShowWindow(h, SW_HIDE); return 0; }
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (hotkeyKey((UINT)wp, false)) return 0;
        break;
    case WM_SYSCHAR: case WM_CHAR:
        if (g.capturing >= 0) return 0;  // no menu beep while capturing Alt combos
        break;
    case WM_KILLFOCUS: case WM_ACTIVATE:
        if ((msg == WM_KILLFOCUS || LOWORD(wp) == WA_INACTIVE) && g.capturing >= 0) endCapture();
        break;
    case WM_HOTKEY:
        if (wp == HK_TOGGLE) toggleRecording();
        else if (wp == HK_FOLDER) openFolder();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_TICK) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP_RECORDER_DONE: {
        g.rec.join();
        KillTimer(h, TIMER_TICK);
        const RecordResult& r = g.rec.result();
        if (r.ok) {
            size_t k = r.file.find_last_of(L'\\');
            std::wstring msg2 = L"Saved " + (k == std::wstring::npos ? r.file : r.file.substr(k + 1)) + L"  \u00b7  " + r.encoderUsed;
            if (!r.message.empty()) msg2 += L"  \u00b7  " + r.message;
            if (r.late) msg2 += L"  \u00b7  " + std::to_wstring(r.late) + L" late frames";
            setStatus(msg2, C_OK);
        } else setStatus(L"Recording failed: " + r.message, C_ERR);
        trayUpdate(NIM_MODIFY);
        if (g.quitting) DestroyWindow(h);
        return 0;
    }
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP) showWindow();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) trayMenu();
        return 0;
    case WM_APP + 2:  // second instance asked us to show
        showWindow();
        return 0;
    case WM_DESTROY:
        unregisterAll();
        if (g.trayAdded) { NOTIFYICONDATAW n{sizeof n}; n.hWnd = h; n.uID = 1; Shell_NotifyIconW(NIM_DELETE, &n); }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void enableDpiAwareness() {
    using Fn = BOOL(WINAPI*)(HANDLE);
    if (auto f = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))
        if (f((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) return;
    SetProcessDPIAware();
}
UINT dpiFor(HWND h) {
    using Fn = UINT(WINAPI*)(HWND);
    if (auto f = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")) return f(h);
    HDC dc = GetDC(nullptr); UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc); return d;
}

// --record <seconds> [--fps N] [--encoder auto|nvenc|amf|qsv|software] [--container mp4|mkv|mov] [--out file] [--allow-synthetic]
int runCli(int argc, wchar_t** argv) {
    // Keep redirected stdout (CI) as is; otherwise print to the parent console if there is one.
    HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((so == nullptr || so == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    std::wstring logPath;
    double secs = 5;
    RecordOptions o;
    Settings s;
    loadFile(U(settingsPath()), s);
    o.fps = s.fps; o.quality = s.quality; o.encoder = s.encoder; o.container = s.container; o.captureCursor = s.captureCursor;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? U(argv[++i]) : std::string(); };
        if (a == L"--record") secs = atof(next().c_str());
        else if (a == L"--fps") o.fps = atoi(next().c_str());
        else if (a == L"--encoder") fromString(next(), o.encoder);
        else if (a == L"--container") fromString(next(), o.container);
        else if (a == L"--quality") fromString(next(), o.quality);
        else if (a == L"--out") o.outputPath = W(next());
        else if (a == L"--allow-synthetic") o.allowSynthetic = true;
        else if (a == L"--log") logPath = W(next());
    }
    if (o.outputPath.empty()) {
        std::wstring d = s.outputDir.empty() ? defaultOutputDir() : W(s.outputDir);
        SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
        o.outputPath = d + L"\\cli-test" + W(extension(o.container));
    }
    Recorder r;
    r.start(o);
    DWORD end = GetTickCount() + (DWORD)(secs * 1000);
    while (r.running() && GetTickCount() < end) Sleep(20);
    r.stop();
    r.join();
    const RecordResult& res = r.result();
    char summary[4096];
    snprintf(summary, sizeof summary, "ok=%d frames=%llu duplicated=%llu late=%llu\ncapture=%s\nencoder=%s\nfile=%s\nmessage=%s\n",
             res.ok ? 1 : 0, res.frames, res.duplicated, res.late, U(res.captureUsed).c_str(), U(res.encoderUsed).c_str(),
             U(res.file).c_str(), U(res.message).c_str());
    fputs(summary, stdout);
    fflush(stdout);
    if (!logPath.empty()) {
        if (FILE* f = _wfopen(logPath.c_str(), L"wb")) { fputs(summary, f); fclose(f); }
    }
    return res.ok ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    enableDpiAwareness();
    for (int i = 1; i < argc; ++i)
        if (wcscmp(argv[i], L"--record") == 0) { int rc = runCli(argc, argv); CoUninitialize(); return rc; }
    bool minimized = false;
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--minimized") == 0) minimized = true;

    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\CastRecorder.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"CastRecorderWnd", nullptr)) PostMessageW(other, WM_APP + 2, 0, 0);
        return 0;
    }

    g.inst = inst;
    g.settingsFile = settingsPath();
    loadFile(U(g.settingsFile), g.s);
    g.s.fps = normalizeFps(g.s.fps);
    g.hwInfo = describeHardwareEncoders();
    g.hwInfo = g.hwInfo.empty() ? L"No hardware encoder found, Auto uses software" : L"Hardware encoders: " + g.hwInfo;
    loadIcons();

    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = g.icoBig;
    wc.hIconSm = g.icoSmall;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"CastRecorderWnd";
    RegisterClassExW(&wc);

    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi}; GetMonitorInfoW(mon, &mi);
    g.hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Cast Recorder",
                             WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU | WS_CAPTION, 0, 0, BASE_W, BASE_H,
                             nullptr, nullptr, inst, nullptr);
    if (!g.hwnd) return 1;
    g.dpi = dpiFor(g.hwnd);
    makeFonts();
    int w = S(BASE_W), h = S(BASE_H);
    RECT wa = mi.rcWork;
    SetWindowPos(g.hwnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, SWP_NOZORDER | SWP_FRAMECHANGED);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    int corner = 2; /* DWMWCP_ROUND */
    DwmSetWindowAttribute(g.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner);
    MARGINS m{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(g.hwnd, &m);  // keeps the DWM drop shadow on a borderless window

    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    trayUpdate(NIM_ADD);
    registerAll();
    g.status = L"Settings save automatically \u00b7 closing keeps Cast Recorder in the tray";
    if (!minimized && !g.s.startMinimized) ShowWindow(g.hwnd, SW_SHOW);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    g.rec.stop();
    g.rec.join();
    if (mtx) CloseHandle(mtx);
    CoUninitialize();
    LocalFree(argv);
    return 0;
}
