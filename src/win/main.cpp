// Cast Recorder: borderless dark settings window, tray icon, global hotkeys, headless CLI for tests.
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <dxgi.h>
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
#include "preview.h"
#include "resource.h"
#include "version.h"
#include "click_wav.h"
#include <mmsystem.h>
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

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

// ---------- design tokens (Vey, tokens.json; logical px @96 DPI) ----------
constexpr COLORREF hex(unsigned v) { return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255); }
const COLORREF C_BG = hex(0x0E1014), C_SURFACE = hex(0x161A20), C_ELEV = hex(0x1C2128), C_PREVIEW = hex(0x0A0C0F),
               C_INPUT = hex(0x1A1F27), C_INPUT_H = hex(0x222832), C_BORDER_S = hex(0x24282E) /* #FFFFFF14 on bg */,
               C_BORDER = hex(0x2C3036) /* #FFFFFF1F */, C_BORDER_STRONG = hex(0x41444A) /* #FFFFFF33 */,
               C_TEXT = hex(0xF2F3F5), C_TEXT2 = hex(0xA8ADB7), C_MUTED = hex(0x6B7280), C_DISABLED = hex(0x4B5563),
               C_ACCENT = hex(0xE5243B), C_ACCENT_B = hex(0xFF6B6B), C_ACCENT_H = hex(0xF0354A), C_ACCENT_P = hex(0xC41E33),
               C_OK = hex(0x22C55E), C_WARN = hex(0xF59E0B), C_ERR = hex(0xEF4444), C_TB_HOVER = hex(0x25282C),
               C_CLOSE_H = hex(0xE81123), C_GRID = hex(0x1D2229);

enum Hit {
    H_NONE = 0, H_MIN, H_CLOSE, H_RECORD, H_PAUSE, H_DD_FPS, H_DD_QUAL, H_DD_ENC, H_DD_GPU, H_DD_RES, H_DD_CONT,
    H_BROWSE, H_PATH, H_HK0, H_HK1, H_ITEM0 = 100
};
struct Region { RECT rc; int id; };

const int kFps[] = {15, 30, 60};
const wchar_t* kFpsL[] = {L"15 FPS", L"30 FPS", L"60 FPS"};
const Quality kQual[] = {Quality::Low, Quality::Medium, Quality::High, Quality::Ultra};
const wchar_t* kQualL[] = {L"Low", L"Medium", L"High", L"Ultra"};
const Encoder kEnc[] = {Encoder::Auto, Encoder::NVENC, Encoder::AMF, Encoder::QSV, Encoder::Software};
const wchar_t* kEncL[] = {L"Auto", L"NVENC", L"AMF", L"QuickSync", L"Software"};
const OutputScale kRes[] = {OutputScale::Native, OutputScale::P1080, OutputScale::P720};
const wchar_t* kResL[] = {L"Native", L"1080p", L"720p"};
const Container kCont[] = {Container::MP4, Container::MKV, Container::MOV};
const wchar_t* kContL[] = {L"MP4", L"MKV", L"MOV"};

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr int HK_TOGGLE = 1, HK_FOLDER = 2, HK_PAUSE = 3;
constexpr UINT TIMER_TICK = 1, TIMER_PREVIEW = 2;
constexpr int BASE_W = 760, BASE_H = 560, MIN_W = 640, MIN_H = 480;
// UI watchdog > worst-case teardown: capture Close 2 s + Finalize (8000/2) + remux (8000/2) + 3 s margin.
constexpr ULONGLONG kWatchdogMs = 2000 + 8000 / 2 + 8000 / 2 + 3000;
constexpr int TB_H = 40, PAD = 16, GAP = 16, PANEL_W = 280, HK_H = 72, BAR_H = 56;

struct App {
    HINSTANCE inst{};
    HWND hwnd{};
    UINT dpi = 96;
    HFONT fTitle{}, fBody{}, fLabel{}, fSmall{}, fSection{}, fBtn{}, fMono{}, fTimer{};
    HICON icoBig{}, icoSmall{}, icoTray{}, icoTrayRec{};
    Settings s;
    std::wstring settingsFile;
    Recorder rec;
    std::vector<Region> regions;
    int hover = H_NONE, pressed = H_NONE;
    int capturing = -1;         // hotkey field being edited (0 start/stop, 1 pause), -1 none
    UINT liveMods = 0;
    std::wstring hkMsg[2];
    COLORREF hkMsgColor[2]{};
    std::wstring status;
    COLORREF statusColor = C_TEXT2;
    int openDrop = H_NONE;      // dropdown whose list is open
    RECT dropRc{};
    std::vector<std::wstring> gpus;  // adapter descriptions, index = DXGI ordinal
    // preview (GDI desktop thumbnail; ~2 fps when idle, frozen while recording)
    HBITMAP prevBmp{};
    int prevW = 0, prevH = 0;
    bool prevLive = false;
    GpuPreview gprev;           // GPU swapchain preview (child window); GDI thumbnail is the fallback
    bool trayAdded = false, quitting = false;
    UINT taskbarCreated = 0;
} g;

int S(int v) { return MulDiv(v, (int)g.dpi, 96); }

HFONT mkFont(int px, int weight, const wchar_t* face = L"Segoe UI") {
    HFONT f = CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    return f ? f : CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Arial");
}
void makeFonts() {
    for (HFONT f : {g.fTitle, g.fBody, g.fLabel, g.fSmall, g.fSection, g.fBtn, g.fMono, g.fTimer}) if (f) DeleteObject(f);
    g.fTitle = mkFont(13, FW_SEMIBOLD);
    g.fBody = mkFont(13, FW_NORMAL);
    g.fLabel = mkFont(12, FW_NORMAL);
    g.fSmall = mkFont(11, FW_NORMAL);
    g.fSection = mkFont(11, FW_SEMIBOLD);
    g.fBtn = mkFont(14, FW_SEMIBOLD);
    g.fMono = mkFont(12, 500);
    g.fTimer = mkFont(14, FW_SEMIBOLD, L"Consolas");
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
    uint32_t* px = (uint32_t*)bits;
    double r = size * 0.24, cx = size - r - 0.5, cy = size - r - 0.5;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            double d = sqrt((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy));
            if (d <= r) px[y * size + x] = 0xFFE5243B;
            else if (d <= r + size / 16.0 + 0.5) px[y * size + x] = 0xFF0E1014;
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

void enumGpus() {
    g.gpus.clear();
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        g.gpus.push_back(d.Description);
        a->Release();
    }
    f->Release();
}

// ---------- tray ----------
void trayUpdate(DWORD msg) {
    NOTIFYICONDATAW n{sizeof n};
    n.hWnd = g.hwnd; n.uID = 1; n.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE; n.uCallbackMessage = WM_TRAY;
    n.hIcon = g.rec.running() ? g.icoTrayRec : g.icoTray;
    std::wstring tip = g.rec.isPaused() ? L"Cast Recorder \u2013 Paused" : g.rec.running() ? L"Cast Recorder \u2013 Recording" : L"Cast Recorder \u2013 Ready";
    if (!g.s.toggleRecord.empty()) tip += L" (" + W(format(g.s.toggleRecord)) + L")";
    wcsncpy(n.szTip, tip.c_str(), ARRAYSIZE(n.szTip) - 1);
    if (Shell_NotifyIconW(msg, &n) && msg == NIM_ADD) g.trayAdded = true;
}

// ---------- hotkeys ----------
void unregisterAll() { UnregisterHotKey(g.hwnd, HK_TOGGLE); UnregisterHotKey(g.hwnd, HK_FOLDER); UnregisterHotKey(g.hwnd, HK_PAUSE); }
void registerAll() {
    if (g.rec.running()) return;  // the live session keeps its hotkeys; never unregister mid-recording
    unregisterAll();
    Hotkey* keys[2] = {&g.s.toggleRecord, &g.s.pauseToggle};
    const int ids[2] = {HK_TOGGLE, HK_PAUSE};
    for (int i = 0; i < 2; ++i) {
        const Hotkey& k = *keys[i];
        g.hkMsg[i].clear();
        if (k.empty()) { g.hkMsg[i] = L"Not set"; g.hkMsgColor[i] = C_MUTED; continue; }
        Issue is = validate(k);
        if (is.severity == Severity::Error) { g.hkMsg[i] = W(is.message); g.hkMsgColor[i] = C_ERR; continue; }
        if (i == 1 && k == g.s.toggleRecord) { g.hkMsg[i] = L"Same combo as Start / Stop"; g.hkMsgColor[i] = C_ERR; continue; }
        if (!RegisterHotKey(g.hwnd, ids[i], k.mods | MOD_NOREPEAT, k.vk)) {
            g.hkMsg[i] = L"In use by another app"; g.hkMsgColor[i] = C_ERR; continue;
        }
        if (is.severity == Severity::Warning) { g.hkMsg[i] = W(is.message); g.hkMsgColor[i] = C_WARN; }
        else { g.hkMsg[i] = L"Works in games"; g.hkMsgColor[i] = C_OK; }
    }
    // Open-folder combo has no field in the new layout but stays active if set and not clashing.
    if (!g.s.openFolder.empty() && !(g.s.openFolder == g.s.toggleRecord) && !(g.s.openFolder == g.s.pauseToggle) &&
        validate(g.s.openFolder).severity != Severity::Error)
        RegisterHotKey(g.hwnd, HK_FOLDER, g.s.openFolder.mods | MOD_NOREPEAT, g.s.openFolder.vk);
}

void save() { saveFile(U(g.settingsFile), g.s); }

std::wstring outputDir() { return g.s.outputDir.empty() ? defaultOutputDir() : W(g.s.outputDir); }
void setStatus(const std::wstring& s, COLORREF c) { g.status = s; g.statusColor = c; if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE); }

void playClick() {
    if (!PlaySoundW(reinterpret_cast<LPCWSTR>(kClickWav), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT))
        PlaySoundW(L"SystemNotification", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
}

// ---------- preview ----------
// Cheap GDI thumbnail of the primary monitor into a small DIB. Runs at ~2 fps idle and is frozen while
// recording so it never competes with the capture/encode path or a game for GPU time.
void updatePreview() {
    if (g.rec.running() || g.gprev.live()) return;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw <= 0 || sh <= 0) return;
    int tw = S(400), th = tw * 9 / 16;
    HDC screen = GetDC(nullptr);
    if (!g.prevBmp || g.prevW != tw || g.prevH != th) {
        if (g.prevBmp) DeleteObject(g.prevBmp);
        g.prevBmp = CreateCompatibleBitmap(screen, tw, th);
        g.prevW = tw; g.prevH = th;
    }
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, g.prevBmp);
    SetStretchBltMode(mem, HALFTONE);
    SetBrushOrgEx(mem, 0, 0, nullptr);
    // letterbox the monitor into 16:9
    double sa = (double)sw / sh, ta = 16.0 / 9.0;
    int dw = tw, dh = th, dx = 0, dy = 0;
    if (sa > ta) { dh = (int)(tw / sa); dy = (th - dh) / 2; } else { dw = (int)(th * sa); dx = (tw - dw) / 2; }
    RECT all{0, 0, tw, th};
    HBRUSH b = CreateSolidBrush(C_INPUT); FillRect(mem, &all, b); DeleteObject(b);
    g.prevLive = StretchBlt(mem, dx, dy, dw, dh, screen, 0, 0, sw, sh, SRCCOPY | CAPTUREBLT) != 0;
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// ---------- recording ----------
// Per-recording diagnostics: <output folder>\CastRecorder.log (appended): path, encoder probe/setup HRESULTs.
void writeSessionLog(const RecordResult& r) {
    std::wstring path = outputDir() + L"\\CastRecorder.log";
    FILE* f = _wfopen(path.c_str(), L"ab");
    if (!f) return;
    wchar_t ts[64]; time_t t = time(nullptr); tm lt{}; localtime_s(&lt, &t); wcsftime(ts, 64, L"%Y-%m-%d %H:%M:%S", &lt);
    std::wstring txt = std::wstring(L"=== ") + ts + L" ===\r\nok=" + (r.ok ? L"1" : L"0") + L" frames=" + std::to_wstring(r.frames) +
                       L" dropped=" + std::to_wstring(r.dropped) + L"\r\ncapture=" + r.captureUsed + L"\r\nencoder=" + r.encoderUsed +
                       L"\r\npath=" + r.pathLog + L"\r\nmessage=" + r.message + L"\r\n" + r.encoderLog + L"\r\n";
    std::string u = U(txt);
    fwrite(u.data(), 1, u.size(), f);
    fclose(f);
}

void forceQuit() {
    g.quitting = true;
    g.rec.stop();
    setStatus(L"Closing\u2026", C_TEXT2);
    ULONGLONG end = GetTickCount64() + 5000;
    while (g.rec.running() && GetTickCount64() < end) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        if (!g.rec.waitUntilIdle(50)) Sleep(10);
    }
    if (g.rec.running()) g.rec.abort();
    g.rec.waitUntilIdle(500);
    DestroyWindow(g.hwnd);
}

void startRecording() {
    if (g.rec.running()) return;
    if (g.rec.mfAbandoned()) { setStatus(L"Restart Cast Recorder to record again (encoder got stuck)", C_WARN); return; }
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
    o.captureCursor = g.s.captureCursor; o.scale = g.s.scale; o.gpuIndex = g.s.gpuIndex; o.bitrateCap = g.s.bitrateCap;
    o.notify = g.hwnd;
    o.outputPath = dir + L"\\" + name + W(extension(g.s.container));
    registerAll();
    if (!g.rec.start(o)) { setStatus(L"Already recording", C_WARN); return; }
    g.gprev.setRecording(true);
    playClick();
    SetTimer(g.hwnd, TIMER_TICK, 250, nullptr);
    setStatus(L"Recording\u2026", C_ACCENT_B);
    trayUpdate(NIM_MODIFY);
}
void stopRecording() {
    if (!g.rec.isActive()) return;
    playClick();
    g.rec.stop();
    setStatus(L"Finishing file\u2026", C_TEXT2);
    trayUpdate(NIM_MODIFY);
}
void toggleRecording() {
    if (g.rec.isActive()) stopRecording();
    else if (!g.rec.running()) startRecording();
}
void togglePause() {
    if (!g.rec.isActive()) return;
    if (g.rec.togglePause()) {
        playClick();
        if (g.rec.isPaused()) setStatus(L"Paused", C_WARN);
        else setStatus(L"Recording\u2026", C_ACCENT_B);
        trayUpdate(NIM_MODIFY);
    }
}
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

// ---------- painting primitives ----------
void fill(HDC dc, const RECT& r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
void round(HDC dc, const RECT& r, COLORREF fillC, COLORREF border, int rad, int penW = 1) {
    HBRUSH b = CreateSolidBrush(fillC);
    HPEN p = CreatePen(PS_SOLID, penW, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(rad) * 2, S(rad) * 2);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}
void ring(HDC dc, const RECT& r, COLORREF c, int rad, int w) {
    HPEN p = CreatePen(PS_INSIDEFRAME, w, c);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH)), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(rad) * 2, S(rad) * 2);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(p);
}
void dot(HDC dc, int cx, int cy, int r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b);
}
void line(HDC dc, int x0, int y0, int x1, int y1, COLORREF c, int w) {
    HPEN p = CreatePen(PS_SOLID, w, c); HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, x0, y0, nullptr); LineTo(dc, x1, y1);
    SelectObject(dc, op); DeleteObject(p);
}
void text(HDC dc, const std::wstring& t, RECT r, HFONT f, COLORREF c, UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, t.c_str(), (int)t.size(), &r, fmt | DT_NOPREFIX);
}
int textW(HDC dc, const std::wstring& t, HFONT f) { SelectObject(dc, f); SIZE z{}; GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &z); return z.cx; }
void addRegion(RECT r, int id) { g.regions.push_back({r, id}); }
void chevron(HDC dc, int cx, int cy, COLORREF c) {
    int s = S(4);
    line(dc, cx - s, cy - s / 2, cx, cy + s / 2, c, S(1) + 1 > 2 ? 2 : S(1) + 1);
    line(dc, cx, cy + s / 2, cx + s, cy - s / 2, c, S(1) + 1 > 2 ? 2 : S(1) + 1);
}

std::wstring modsText(UINT m) {
    std::wstring s;
    if (m & MOD_CTRL_) s += L"Ctrl+";
    if (m & MOD_ALT_) s += L"Alt+";
    if (m & MOD_SHIFT_) s += L"Shift+";
    if (m & MOD_WIN_) s += L"Win+";
    return s;
}

// ---------- dropdown model ----------
int ddCount(int id) {
    switch (id) {
        case H_DD_FPS: return 3; case H_DD_QUAL: return 4; case H_DD_ENC: return 5;
        case H_DD_GPU: return 1 + (int)g.gpus.size(); case H_DD_RES: return 3; case H_DD_CONT: return 3;
    }
    return 0;
}
std::wstring ddItem(int id, int i) {
    switch (id) {
        case H_DD_FPS: return kFpsL[i]; case H_DD_QUAL: return kQualL[i]; case H_DD_ENC: return kEncL[i];
        case H_DD_GPU: return i == 0 ? std::wstring(L"Auto (monitor's GPU)") : g.gpus[i - 1];
        case H_DD_RES: return kResL[i]; case H_DD_CONT: return kContL[i];
    }
    return L"";
}
int ddSel(int id) {
    switch (id) {
        case H_DD_FPS: return g.s.fps == 15 ? 0 : g.s.fps == 30 ? 1 : 2;
        case H_DD_QUAL: return (int)g.s.quality; case H_DD_ENC: return (int)g.s.encoder;
        case H_DD_GPU: return g.s.gpuIndex < 0 || g.s.gpuIndex >= (int)g.gpus.size() ? 0 : g.s.gpuIndex + 1;
        case H_DD_RES: return (int)g.s.scale; case H_DD_CONT: return (int)g.s.container;
    }
    return 0;
}
void ddSet(int id, int i) {
    switch (id) {
        case H_DD_FPS: g.s.fps = kFps[i]; break; case H_DD_QUAL: g.s.quality = kQual[i]; break;
        case H_DD_ENC: g.s.encoder = kEnc[i]; break; case H_DD_GPU: g.s.gpuIndex = i - 1; break;
        case H_DD_RES: g.s.scale = kRes[i]; break; case H_DD_CONT: g.s.container = kCont[i]; break;
    }
    save();
}
bool ddLocked(int id) { return g.rec.running() && id != H_NONE; }  // settings lock while a session is live

// ---------- keyboard focus (Tab / Shift+Tab, arrows, Enter/Space) ----------
const int kFocusOrder[] = {H_DD_FPS, H_DD_QUAL, H_DD_ENC, H_DD_GPU, H_DD_RES, H_DD_CONT, H_PATH, H_BROWSE, H_HK0, H_HK1, H_PAUSE, H_RECORD};
int g_focus = H_NONE;
int g_dropKb = -1;      // keyboard-highlighted item in the open dropdown  // keyboard focus; shown as a brighter border once Tab is used
bool kbFocused(int id) { return g_focus == id; }
bool focusable(int id) { return id != H_PAUSE || g.rec.isActive(); }
void moveFocus(int dir) {
    const int n = (int)(sizeof kFocusOrder / sizeof *kFocusOrder);
    int i = 0;
    for (; i < n && kFocusOrder[i] != g_focus; ++i) {}
    for (int k = 0; k < n; ++k) {
        i = g_focus == H_NONE && k == 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
        if (focusable(kFocusOrder[i])) { g_focus = kFocusOrder[i]; return; }
    }
}

// ---------- layout ----------
struct Layout { RECT title, preview, panel, hk, bar, record, pause; };
Layout layout(RECT c) {
    Layout L{};
    const int pad = S(PAD), gap = S(GAP);
    L.title = {0, 0, c.right, S(TB_H)};
    L.bar = {pad, c.bottom - pad - S(BAR_H) + S(8), c.right - pad, c.bottom - pad};
    L.hk = {pad, L.bar.top - gap / 2 - S(HK_H), c.right - pad, L.bar.top - gap / 2};
    int top = L.title.bottom + pad, bottom = L.hk.top - gap;
    int panelW = S(PANEL_W);
    L.panel = {c.right - pad - panelW, top, c.right - pad, bottom};
    L.preview = {pad, top, L.panel.left - gap, bottom};
    int bh = S(40), bw = S(120);
    L.record = {L.bar.right - bw, L.bar.top + (L.bar.bottom - L.bar.top - bh) / 2, L.bar.right, 0};
    L.record.bottom = L.record.top + bh;
    L.pause = {L.record.left - S(12) - S(104), L.record.top, L.record.left - S(12), L.record.bottom};
    return L;
}

void dropdown(HDC dc, RECT r, int id) {
    bool locked = ddLocked(id), open = g.openDrop == id, hov = g.hover == id && !locked;
    round(dc, r, hov || open ? C_INPUT_H : C_INPUT, open ? C_ACCENT : kbFocused(id) ? C_BORDER_STRONG : C_BORDER, 6);
    RECT t{r.left + S(10), r.top, r.right - S(34), r.bottom};  // clear of the arrow at any DPI
    text(dc, ddItem(id, ddSel(id)), t, g.fBody, locked ? C_DISABLED : C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    chevron(dc, r.right - S(14), (r.top + r.bottom) / 2, locked ? C_DISABLED : C_TEXT2);
    addRegion(r, id);
    if (open) g.dropRc = r;
}

void paintTitle(HDC dc, const Layout& L) {
    fill(dc, L.title, C_BG);
    DrawIconEx(dc, S(12), S(12), g.icoSmall, S(16), S(16), 0, nullptr, DI_NORMAL);
    text(dc, L"Cast Recorder", RECT{S(36), 0, L.title.right - S(100), L.title.bottom}, g.fTitle, C_TEXT);
    RECT rmin{L.title.right - S(92), 0, L.title.right - S(46), S(TB_H)}, rclose{L.title.right - S(46), 0, L.title.right, S(TB_H)};
    if (g.hover == H_MIN) fill(dc, rmin, C_TB_HOVER);
    if (g.hover == H_CLOSE) fill(dc, rclose, C_CLOSE_H);
    int mx = (rmin.left + rmin.right) / 2, my = (rmin.top + rmin.bottom) / 2;
    line(dc, mx - S(5), my, mx + S(5), my, C_TEXT, 1);
    int cx = (rclose.left + rclose.right) / 2, cy = (rclose.top + rclose.bottom) / 2, k = S(5);
    COLORREF xc = g.hover == H_CLOSE ? RGB(255, 255, 255) : C_TEXT;
    line(dc, cx - k, cy - k, cx + k + 1, cy + k + 1, xc, 1);
    line(dc, cx + k, cy - k, cx - k - 1, cy + k + 1, xc, 1);
    addRegion(rmin, H_MIN);
    addRegion(rclose, H_CLOSE);
}

RECT previewSurf(const Layout& L) {
    RECT in{L.preview.left + S(8), L.preview.top + S(8), L.preview.right - S(8), L.preview.bottom - S(8)};
    int iw = in.right - in.left, ih = in.bottom - in.top;
    int sw = iw, sh = iw * 9 / 16;
    if (sh > ih) { sh = ih; sw = ih * 16 / 9; }
    RECT surf{in.left + (iw - sw) / 2, in.top + (ih - sh) / 2, 0, 0};
    surf.right = surf.left + sw; surf.bottom = surf.top + sh;
    return surf;
}
RECT previewRectNow() { RECT c{}; GetClientRect(g.hwnd, &c); return previewSurf(layout(c)); }

void paintPreview(HDC dc, const Layout& L) {
    bool rec = g.rec.running(), paused = g.rec.isPaused();
    round(dc, L.preview, C_PREVIEW, C_BORDER, 8);
    // 16:9 surface letterboxed inside with an 8px inset
    RECT in{L.preview.left + S(8), L.preview.top + S(8), L.preview.right - S(8), L.preview.bottom - S(8)};
    int iw = in.right - in.left, ih = in.bottom - in.top;
    int sw = iw, sh = iw * 9 / 16;
    if (sh > ih) { sh = ih; sw = ih * 16 / 9; }
    RECT surf{in.left + (iw - sw) / 2, in.top + (ih - sh) / 2, 0, 0};
    surf.right = surf.left + sw; surf.bottom = surf.top + sh;
    fill(dc, surf, C_INPUT);
    if (g.prevBmp) {
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, g.prevBmp);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, surf.left, surf.top, sw, sh, mem, 0, 0, g.prevW, g.prevH, SRCCOPY);
        SelectObject(mem, old); DeleteDC(mem);
    } else {
        for (int x = surf.left + S(24); x < surf.right; x += S(24)) line(dc, x, surf.top, x, surf.bottom, C_GRID, 1);
        for (int y = surf.top + S(24); y < surf.bottom; y += S(24)) line(dc, surf.left, y, surf.right, y, C_GRID, 1);
        RECT win{surf.left + sw / 5, surf.top + sh / 5, surf.right - sw / 4, surf.bottom - sh / 4};
        round(dc, win, C_ELEV, C_BORDER, 6);
        text(dc, L"Waiting for capture\u2026", surf, g.fBody, C_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (rec) {  // dim + freeze while recording so the preview never competes with the game/encoder
        ring(dc, L.preview, C_ACCENT, 8, S(2));
    }
    // caption chip
    std::wstring cap = rec ? (paused ? L"Preview paused" : L"Preview paused while recording") : L"Live preview";
    int cw = textW(dc, cap, g.fSmall) + S(28);
    RECT chip{surf.left + S(8), surf.top + S(8), surf.left + S(8) + cw, surf.top + S(30)};
    round(dc, chip, C_PREVIEW, C_BORDER_S, 11);
    dot(dc, chip.left + S(11), (chip.top + chip.bottom) / 2, S(3), rec ? C_MUTED : (g.prevLive ? C_OK : C_MUTED));
    text(dc, cap, RECT{chip.left + S(20), chip.top, chip.right - S(6), chip.bottom}, g.fSmall, C_TEXT2);
    if (rec) {  // REC / PAUSED badge top-right
        std::wstring b = paused ? L"PAUSED" : L"REC";
        int bw = textW(dc, b, g.fSection) + S(26);
        RECT badge{surf.right - S(8) - bw, surf.top + S(8), surf.right - S(8), surf.top + S(30)};
        round(dc, badge, paused ? hex(0x3A2A0A) : C_ACCENT, paused ? C_WARN : C_ACCENT, 11);
        dot(dc, badge.left + S(11), (badge.top + badge.bottom) / 2, S(3), paused ? C_WARN : RGB(255, 255, 255));
        text(dc, b, RECT{badge.left + S(19), badge.top, badge.right - S(6), badge.bottom}, g.fSection, paused ? C_WARN : RGB(255, 255, 255));
    }
}

void paintPanel(HDC dc, const Layout& L) {
    round(dc, L.panel, C_SURFACE, C_BORDER_S, 8);
    const int x = L.panel.left + S(16), right = L.panel.right - S(16);
    const int labelW = S(88), ctrlX = x + labelW, rowH = S(32);
    int avail = L.panel.bottom - L.panel.top - S(32);
    // 7 control rows + 2 section headers; compress the row gap on short windows
    int gapY = (avail - 7 * rowH - 2 * S(18)) / 8;
    if (gapY > S(12)) gapY = S(12);
    if (gapY < S(4)) gapY = S(4);
    int y = L.panel.top + S(16);
    auto section = [&](const wchar_t* t) { text(dc, t, RECT{x, y, right, y + S(14)}, g.fSection, C_MUTED); y += S(18); };
    auto row = [&](const wchar_t* label, int id) {
        text(dc, label, RECT{x, y, ctrlX - S(6), y + rowH}, g.fLabel, C_TEXT2);
        dropdown(dc, RECT{ctrlX, y, right, y + rowH}, id);
        y += rowH + gapY;
    };
    section(L"CAPTURE");
    row(L"FPS", H_DD_FPS);
    row(L"Quality", H_DD_QUAL);
    row(L"Encoder", H_DD_ENC);
    row(L"GPU", H_DD_GPU);
    section(L"OUTPUT");
    row(L"Resolution", H_DD_RES);
    row(L"Container", H_DD_CONT);
    // Save folder: full-width path field (folder glyph + path) + Browse; the label column would clip it at 96 DPI.
    RECT br{right - S(80), y, right, y + rowH};
    RECT pf{x, y, br.left - S(8), y + rowH};
    round(dc, pf, g.hover == H_PATH ? C_INPUT_H : C_INPUT, kbFocused(H_PATH) ? C_BORDER_STRONG : C_BORDER, 6);
    {   // folder glyph
        int gx = pf.left + S(10), gy = (pf.top + pf.bottom) / 2;
        round(dc, RECT{gx, gy - S(4), gx + S(14), gy + S(6)}, C_TEXT2, C_TEXT2, 2);
        fill(dc, RECT{gx, gy - S(6), gx + S(6), gy - S(3)}, C_TEXT2);
    }
    text(dc, outputDir(), RECT{pf.left + S(32), pf.top, pf.right - S(8), pf.bottom}, g.fSmall, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    addRegion(pf, H_PATH);  // click opens the folder
    bool lock = g.rec.running();
    round(dc, br, g.hover == H_BROWSE && !lock ? C_INPUT_H : C_ELEV, kbFocused(H_BROWSE) ? C_BORDER_STRONG : C_BORDER, 6);
    text(dc, L"Browse…", br, g.fLabel, lock ? C_DISABLED : C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    addRegion(br, H_BROWSE);
}

void paintHotkeys(HDC dc, const Layout& L) {
    round(dc, L.hk, C_SURFACE, C_BORDER_S, 8);
    const wchar_t* labels[2] = {L"Start / Stop", L"Pause"};
    const Hotkey* hks[2] = {&g.s.toggleRecord, &g.s.pauseToggle};
    int colW = (L.hk.right - L.hk.left - S(32) - S(24)) / 2;
    for (int i = 0; i < 2; ++i) {
        int cx = L.hk.left + S(16) + i * (colW + S(24));
        int midY = (L.hk.top + L.hk.bottom) / 2;
        text(dc, labels[i], RECT{cx, L.hk.top + S(12), cx + colW, L.hk.top + S(28)}, g.fLabel, C_TEXT2);
        RECT f{cx, midY - S(4), cx + S(160), midY + S(26)};
        if (f.right > cx + colW) f.right = cx + colW;
        bool lockedHk = g.rec.running();
        bool focus = g.capturing == i, hv = g.hover == H_HK0 + i && !lockedHk;
        round(dc, f, focus ? C_INPUT_H : C_INPUT, focus ? C_ACCENT : (kbFocused(H_HK0 + i) ? C_BORDER_STRONG : hv ? C_BORDER_STRONG : C_BORDER), 6);
        std::wstring t; COLORREF tc = C_TEXT;
        if (focus) { t = g.liveMods ? modsText(g.liveMods) + L"\u2026" : L"Press a key\u2026"; tc = g.liveMods ? C_TEXT : C_MUTED; }
        else if (hks[i]->empty()) { t = L"Click to set"; tc = C_MUTED; }
        else t = W(format(*hks[i]));
        if (lockedHk && !focus) tc = C_DISABLED;
        text(dc, t, f, g.fMono, tc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        addRegion(f, H_HK0 + i);
        std::wstring m = focus ? L"Esc cancels \u00b7 Backspace clears" : g.hkMsg[i];
        text(dc, m, RECT{f.right + S(10), f.top, cx + colW, f.bottom}, g.fSmall, focus ? C_MUTED : g.hkMsgColor[i], DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

void paintBar(HDC dc, const Layout& L) {
    RecState st = g.rec.state();
    bool active = g.rec.isActive(), paused = g.rec.isPaused();
    bool finishing = st == RecState::Stopping || st == RecState::Finishing;
    int y0 = L.bar.top, y1 = L.bar.bottom;
    int x = L.bar.left;
    if (active) {
        dot(dc, x + S(5), (y0 + y1) / 2, S(5), paused ? C_WARN : C_ACCENT);
        x += S(18);
        std::wstring lbl = paused ? L"Paused" : L"Recording\u2026";
        text(dc, lbl, RECT{x, y0, x + S(120), y1}, g.fBtn, paused ? C_WARN : C_ACCENT_B);
        x += textW(dc, lbl, g.fBtn) + S(12);
        int sec = (int)g.rec.elapsedSeconds();
        wchar_t tm[32]; swprintf(tm, 32, L"%02d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
        text(dc, tm, RECT{x, y0, x + S(100), y1}, g.fTimer, C_TEXT);
    } else {
        std::wstring s = finishing ? L"Finishing file\u2026" : (g.status.empty() ? L"Ready" : g.status);
        text(dc, s, RECT{x, y0, L.pause.left - S(12), y1}, g.fLabel, finishing ? C_TEXT2 : g.statusColor,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    // Pause / Resume (only while a session is active)
    if (active) {
        bool hv = g.hover == H_PAUSE;
        round(dc, L.pause, hv ? C_INPUT_H : C_ELEV, C_BORDER, 10);
        int cx = L.pause.left + S(20), cy = (L.pause.top + L.pause.bottom) / 2;
        if (paused) {  // play triangle
            POINT tri[3] = {{cx - S(4), cy - S(6)}, {cx - S(4), cy + S(6)}, {cx + S(6), cy}};
            HBRUSH b = CreateSolidBrush(C_TEXT); HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
            Polygon(dc, tri, 3); SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b);
        } else {
            fill(dc, RECT{cx - S(5), cy - S(6), cx - S(1), cy + S(6)}, C_TEXT);
            fill(dc, RECT{cx + S(2), cy - S(6), cx + S(6), cy + S(6)}, C_TEXT);
        }
        text(dc, paused ? L"Resume" : L"Pause", RECT{cx + S(12), L.pause.top, L.pause.right - S(8), L.pause.bottom}, g.fBtn, C_TEXT);
        if (kbFocused(H_PAUSE)) { RECT fr{L.pause.left - S(3), L.pause.top - S(3), L.pause.right + S(3), L.pause.bottom + S(3)}; ring(dc, fr, C_TEXT2, 12, 1); }
        addRegion(L.pause, H_PAUSE);
    }
    // Record / Stop, drawn in code
    RECT r = L.record;
    bool hv = g.hover == H_RECORD, pr = g.pressed == H_RECORD;
    int cy = (r.top + r.bottom) / 2;
    if (active || finishing) {
        round(dc, r, hv ? C_INPUT_H : C_ELEV, finishing ? C_BORDER : C_ACCENT, 10, S(2));
        int sx = r.left + S(26);
        fill(dc, RECT{sx - S(6), cy - S(6), sx + S(6), cy + S(6)}, finishing ? C_DISABLED : C_ACCENT);
        text(dc, finishing ? L"Saving" : L"Stop", RECT{sx + S(12), r.top, r.right - S(10), r.bottom}, g.fBtn, finishing ? C_TEXT2 : C_TEXT);
    } else {
        COLORREF bg = pr ? C_ACCENT_P : hv ? C_ACCENT_H : C_ACCENT;
        round(dc, r, bg, bg, 10);
        int dx = r.left + S(26);
        dot(dc, dx, cy, S(6), RGB(255, 255, 255));
        dot(dc, dx, cy, S(3), bg);
        text(dc, L"Record", RECT{dx + S(14), r.top, r.right - S(10), r.bottom}, g.fBtn, RGB(255, 255, 255));
    }
    if (kbFocused(H_RECORD)) { RECT fr{r.left - S(3), r.top - S(3), r.right + S(3), r.bottom + S(3)}; ring(dc, fr, C_TEXT2, 12, 1); }
    addRegion(r, H_RECORD);
}

void paintDropList(HDC dc, RECT client) {
    if (g.openDrop == H_NONE) return;
    int n = ddCount(g.openDrop), ih = S(30);
    RECT r = g.dropRc;
    RECT list{r.left, r.bottom + S(4), r.right, r.bottom + S(4) + n * ih + S(8)};
    if (list.bottom > client.bottom - S(8)) { int h = list.bottom - list.top; list.bottom = r.top - S(4); list.top = list.bottom - h; }
    round(dc, list, C_ELEV, C_BORDER_STRONG, 6);
    addRegion(list, H_ITEM0 + 99);  // swallow clicks on padding
    int sel = ddSel(g.openDrop);
    for (int i = 0; i < n; ++i) {
        RECT it{list.left + S(4), list.top + S(4) + i * ih, list.right - S(4), list.top + S(4) + (i + 1) * ih};
        bool hv = g.hover == H_ITEM0 + i || g_dropKb == i;
        if (hv) round(dc, it, C_INPUT_H, C_INPUT_H, 4);
        if (i == sel) fill(dc, RECT{it.left + S(4), it.top + S(8), it.left + S(6), it.bottom - S(8)}, C_ACCENT);
        text(dc, ddItem(g.openDrop, i), RECT{it.left + S(12), it.top, it.right - S(6), it.bottom}, g.fBody, i == sel ? C_TEXT : C_TEXT2,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        addRegion(it, H_ITEM0 + i);
    }
}

void paint(HDC dc, RECT client) {
    g.regions.clear();
    fill(dc, client, C_BG);
    SetBkMode(dc, TRANSPARENT);
    Layout L = layout(client);
    paintTitle(dc, L);
    paintPreview(dc, L);
    paintPanel(dc, L);
    paintHotkeys(dc, L);
    paintBar(dc, L);
    paintDropList(dc, client);  // last: overlays everything and its regions win hit-testing
}

int hitTest(POINT p) {
    for (auto it = g.regions.rbegin(); it != g.regions.rend(); ++it) if (PtInRect(&it->rc, p)) return it->id;
    return H_NONE;
}

void endCapture() {
    g.capturing = -1;
    g.liveMods = 0;
    if (!g.rec.running()) registerAll();  // never touch global hotkeys mid-recording
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

bool hotkeyKey(UINT vk, bool down) {
    if (g.capturing < 0) return false;
    Hotkey* dst = g.capturing == 0 ? &g.s.toggleRecord : &g.s.pauseToggle;
    UINT mods = currentMods();
    if (isModifierKey(vk)) { g.liveMods = mods; InvalidateRect(g.hwnd, nullptr, FALSE); return true; }
    if (!down && vk != VK_SNAPSHOT) return true;
    if (mods == 0 && vk == VK_ESCAPE) { endCapture(); return true; }
    if (mods == 0 && (vk == VK_BACK || vk == VK_DELETE)) { *dst = Hotkey{}; save(); endCapture(); return true; }
    Hotkey hk{mods, vk};
    int idx = g.capturing;
    if (keyName(vk).empty()) { g.hkMsg[idx] = L"Key not supported"; g.hkMsgColor[idx] = C_ERR; return true; }
    Issue is = validate(hk);
    const Hotkey& other = idx == 0 ? g.s.pauseToggle : g.s.toggleRecord;
    if (is.severity == Severity::Error || hk == other) {
        g.capturing = -1; g.liveMods = 0;
        registerAll();
        g.hkMsg[idx] = W(format(hk)) + (hk == other ? std::wstring(idx == 0 ? L" is used by Pause" : L" is used by Start / Stop") : L": " + W(is.message));
        g.hkMsgColor[idx] = C_ERR;
        InvalidateRect(g.hwnd, nullptr, FALSE);
        return true;
    }
    if (hk == g.s.openFolder) g.s.openFolder = Hotkey{};  // the hidden folder combo yields to the visible ones
    *dst = hk;
    save();
    endCapture();
    trayUpdate(NIM_MODIFY);
    return true;
}

void click(int id) {
    if (id != H_HK0 && id != H_HK1 && g.capturing >= 0) endCapture();
    if (g.openDrop != H_NONE) {
        int dd = g.openDrop;
        g.openDrop = H_NONE;
        g_dropKb = -1;
        if (id >= H_ITEM0 && id < H_ITEM0 + ddCount(dd)) ddSet(dd, id - H_ITEM0);
        InvalidateRect(g.hwnd, nullptr, FALSE);
        if (id >= H_ITEM0 || id == dd) return;  // item picked, padding, or toggled closed
    }
    if (id == H_MIN) ShowWindow(g.hwnd, SW_MINIMIZE);
    else if (id == H_CLOSE) ShowWindow(g.hwnd, SW_HIDE);  // keeps running in the tray
    else if (id == H_RECORD) toggleRecording();
    else if (id == H_PAUSE) togglePause();
    else if (id >= H_DD_FPS && id <= H_DD_CONT) {
        if (ddLocked(id)) setStatus(L"Stop recording to change settings", C_TEXT2);
        else { if (id == H_DD_GPU) enumGpus(); g.openDrop = id; }
    } else if (id == H_HK0 || id == H_HK1) {
        if (g.rec.running()) { setStatus(L"Stop recording to change hotkeys", C_TEXT2); InvalidateRect(g.hwnd, nullptr, FALSE); return; }
        g.capturing = id - H_HK0;
        g.liveMods = 0;
        unregisterAll();
        SetFocus(g.hwnd);
    } else if (id == H_BROWSE) { if (!g.rec.running()) pickFolder(); }
    else if (id == H_PATH) openFolder();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void showWindow() {
    ShowWindow(g.hwnd, IsIconic(g.hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g.hwnd);
}

void trayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, g.rec.isActive() ? L"Stop recording" : L"Start recording");
    if (g.rec.isActive()) AppendMenuW(m, MF_STRING, 5, g.rec.isPaused() ? L"Resume" : L"Pause");
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
    else if (c == 5) togglePause();
    else if (c == 2) openFolder();
    else if (c == 3) showWindow();
    else if (c == 4) forceQuit();
}

void paintTo(HWND h, HDC dc) {
    RECT rc; GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    paint(mem, rc);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
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
        RECT rc; GetClientRect(h, &rc);
        if (p.x >= rc.right - S(16) && p.y >= rc.bottom - S(16)) return HTBOTTOMRIGHT;  // invisible resize grip
        if (p.y < S(TB_H) && hitTest(p) == H_NONE) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize = {S(MIN_W), S(MIN_H)};
        return 0;
    }
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        makeFonts();
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        updatePreview();
        g.gprev.setRect(previewRectNow());
        return 0;
    }
    case WM_SHOWWINDOW:
        if (wp) { updatePreview(); SetTimer(h, TIMER_PREVIEW, 500, nullptr); }
        else KillTimer(h, TIMER_PREVIEW);  // hidden to tray: no preview work at all
        g.gprev.setVisible(wp != 0);
        break;
    case WM_SIZE:
        g.gprev.setRect(previewRectNow());
        g.gprev.setVisible(wp != SIZE_MINIMIZED && IsWindowVisible(h));
        break;
    case WM_APP + 41:  // GPU preview went live (1) or failed (0)
        if (wp) g.gprev.setVisible(IsWindowVisible(h) && !IsIconic(h));
        else { if (g.gprev.hwnd()) ShowWindow(g.gprev.hwnd(), SW_HIDE); updatePreview(); }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paintTo(h, dc);
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
        if (LOWORD(lp) == HTCLIENT) return TRUE;
        break;
    case WM_MOUSELEAVE: g.hover = H_NONE; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: g.pressed = hitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}); SetCapture(h); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        int id = hitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (id != H_NONE && id == g.pressed) click(id);
        else if (id == H_NONE) {
            if (g.capturing >= 0) endCapture();
            if (g.openDrop != H_NONE) { g.openDrop = H_NONE; InvalidateRect(h, nullptr, FALSE); }
        }
        g.pressed = H_NONE;
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (hotkeyKey((UINT)wp, true)) return 0;
        if (wp == VK_ESCAPE) {
            if (g.openDrop != H_NONE) { g.openDrop = H_NONE; g_dropKb = -1; InvalidateRect(h, nullptr, FALSE); }
            else ShowWindow(h, SW_HIDE);
            return 0;
        }
        if (msg == WM_KEYDOWN) {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (g.openDrop != H_NONE) {  // list navigation
                int n = ddCount(g.openDrop);
                if (g_dropKb < 0) g_dropKb = ddSel(g.openDrop);
                if (wp == VK_DOWN) g_dropKb = (g_dropKb + 1) % n;
                else if (wp == VK_UP) g_dropKb = (g_dropKb + n - 1) % n;
                else if (wp == VK_RETURN || wp == VK_SPACE) { int dd = g.openDrop; g.openDrop = H_NONE; ddSet(dd, g_dropKb); g_dropKb = -1; }
                else if (wp == VK_TAB) { g.openDrop = H_NONE; g_dropKb = -1; moveFocus(shift ? -1 : 1); }
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (wp == VK_TAB) { moveFocus(shift ? -1 : 1); InvalidateRect(h, nullptr, FALSE); return 0; }
            if (g_focus >= H_DD_FPS && g_focus <= H_DD_CONT && (wp == VK_UP || wp == VK_DOWN) && !ddLocked(g_focus)) {
                int n = ddCount(g_focus), i = ddSel(g_focus) + (wp == VK_DOWN ? 1 : -1);
                if (i >= 0 && i < n) ddSet(g_focus, i);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if ((wp == VK_LEFT || wp == VK_RIGHT) && g_focus != H_NONE) { moveFocus(wp == VK_RIGHT ? 1 : -1); InvalidateRect(h, nullptr, FALSE); return 0; }
            if ((wp == VK_RETURN || wp == VK_SPACE) && g_focus != H_NONE) {
                if (g_focus >= H_DD_FPS && g_focus <= H_DD_CONT) g_dropKb = ddSel(g_focus);
                click(g_focus);
                return 0;
            }
        }
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (hotkeyKey((UINT)wp, false)) return 0;
        break;
    case WM_SYSCHAR: case WM_CHAR:
        if (g.capturing >= 0) return 0;
        break;
    case WM_KILLFOCUS: case WM_ACTIVATE:
        if (msg == WM_KILLFOCUS || LOWORD(wp) == WA_INACTIVE) {
            if (g.capturing >= 0) endCapture();
            if (g.openDrop != H_NONE) { g.openDrop = H_NONE; InvalidateRect(h, nullptr, FALSE); }
        }
        break;
    case WM_HOTKEY:
        if (wp == HK_TOGGLE) toggleRecording();
        else if (wp == HK_PAUSE) togglePause();
        else if (wp == HK_FOLDER) openFolder();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_PREVIEW) {
            if (!g.rec.running() && IsWindowVisible(h) && !IsIconic(h)) { updatePreview(); InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        if (wp == TIMER_TICK) {
            static ULONGLONG finishSince = 0;
            RecState st = g.rec.state();
            if (st == RecState::Stopping || st == RecState::Finishing) {
                if (!finishSince) finishSince = GetTickCount64();
                else if (GetTickCount64() - finishSince > kWatchdogMs) {
                    finishSince = 0;
                    g.rec.abort();
                    setStatus(L"Stopped (finalize took too long)", C_WARN);
                    KillTimer(h, TIMER_TICK);
                    trayUpdate(NIM_MODIFY);
                }
            } else finishSince = 0;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_APP_RECORDER_DONE: {
        g.rec.waitUntilIdle(100);
        KillTimer(h, TIMER_TICK);
        const RecordResult& r = g.rec.result();
        g.gprev.setRecording(false);
        writeSessionLog(r);
        if (r.ok) {
            size_t k = r.file.find_last_of(L'\\');
            std::wstring msg2 = L"Saved " + (k == std::wstring::npos ? r.file : r.file.substr(k + 1)) + L"  \u00b7  " + r.encoderUsed;
            if (!r.message.empty()) msg2 += L"  \u00b7  " + r.message;
            if (r.cpuFallback) msg2 += L"  \u00b7  GPU conversion failed, finished on CPU";
            if (r.dropped) msg2 += L"  \u00b7  " + std::to_wstring(r.dropped) + L" dropped";
            if (!r.encoderFallbacks.empty()) msg2 += L"  \u00b7  " + r.encoderFallbacks.front() + L" failed its test encode, fell back";
            setStatus(msg2, C_OK);
        } else if (r.abandoned) setStatus(L"Encoder got stuck, file discarded. Restart Cast Recorder to record again.", C_WARN);
        else if (r.timedOut) setStatus(L"Stopped (timed out): " + r.message, C_WARN);
        else setStatus(r.message.empty() ? L"Recording stopped" : (L"Recording failed: " + r.message), C_ERR);
        trayUpdate(NIM_MODIFY);
        updatePreview();
        InvalidateRect(h, nullptr, FALSE);
        if (g.quitting) DestroyWindow(h);
        return 0;
    }
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP) showWindow();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) trayMenu();
        return 0;
    case WM_APP + 2:
        showWindow();
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_DESTROY:
        g.gprev.stop();
        unregisterAll();
        if (g.trayAdded) { NOTIFYICONDATAW n{sizeof n}; n.hWnd = h; n.uID = 1; Shell_NotifyIconW(NIM_DELETE, &n); }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Headless UI render for docs/CI screenshots: paints the window at 96 DPI into a 24-bit BMP.
// state: idle | recording | paused | dropdown
int renderUi(const std::wstring& path, const std::wstring& state, int dpi) {
    g.dpi = dpi;
    makeFonts();
    enumGpus();
    if (g.icoSmall == nullptr) loadIcons();
    RECT rc{0, 0, S(BASE_W), S(BASE_H)};
    HDC screen = GetDC(nullptr);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = rc.right; bi.bmiHeader.biHeight = -rc.bottom;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, bmp);
    if (state == L"dropdown") g.openDrop = H_DD_ENC;
    g.hwnd = nullptr;
    g.status = state == L"recording" ? L"Recording…" : state == L"paused" ? L"Paused" : L"Ready";
    g.statusColor = state == L"recording" ? C_ACCENT_B : state == L"paused" ? C_WARN : C_TEXT2;
    g.hkMsg[0] = L"Works in games"; g.hkMsgColor[0] = C_OK;
    g.hkMsg[1] = L"Works in games"; g.hkMsgColor[1] = C_OK;
    paint(mem, rc);
    GdiFlush();
    // write BMP
    BITMAPFILEHEADER fh{}; fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
    DWORD sz = (DWORD)rc.right * rc.bottom * 4; fh.bfSize = fh.bfOffBits + sz;
    BITMAPINFOHEADER ih = bi.bmiHeader; ih.biHeight = -rc.bottom;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    bool ok = f != INVALID_HANDLE_VALUE;
    if (ok) {
        DWORD w = 0;
        ok = WriteFile(f, &fh, sizeof fh, &w, nullptr) && WriteFile(f, &ih, sizeof ih, &w, nullptr) && WriteFile(f, bits, sz, &w, nullptr);
        CloseHandle(f);
    }
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
    return ok ? 0 : 1;
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
    // Find --log first so validation errors can be written there too (CI reads the log file).
    for (int i = 1; i + 1 < argc; ++i) if (wcscmp(argv[i], L"--log") == 0) logPath = argv[i + 1];
    auto emit = [&](const std::string& text, FILE* stream) {
        fputs(text.c_str(), stream);
        fflush(stream);
        if (!logPath.empty()) if (FILE* f = _wfopen(logPath.c_str(), L"wb")) { fputs(text.c_str(), f); fclose(f); }
    };
    const char* usage =
        "Cast Recorder " CR_VERSION_STR "\n"
        "Usage: CastRecorder.exe [--minimized]\n"
        "       CastRecorder.exe --record SECONDS [options]\n\n"
        "  --record SECONDS     record headless for 0 < SECONDS <= 86400, then exit\n"
        "  --fps 15|30|60       frame rate (default: saved setting)\n"
        "  --quality low|medium|high|ultra\n"
        "  --encoder auto|nvenc|amf|qsv|software\n"
        "  --container mp4|mkv|mov\n"
        "  --scale native|1080p|720p\n"
        "  --gpu INDEX          adapter ordinal, or -1 for Auto (monitor's GPU)\n"
        "  --bitrate-cap BITS   optional bitrate ceiling (bits/s)\n"
        "  --out FILE           output file (its folder is created if needed)\n"
        "  --log FILE           also write the result summary to FILE\n"
        "  --allow-synthetic    use a test pattern if screen capture is unavailable (CI)\n"
        "  --pause-at S / --resume-at S  pause and resume at S seconds into the recording\n"
        "  --render-ui FILE.bmp  paint the settings window to a BMP (docs/CI)\n"
        "  --ui-state idle|dropdown\n"
        "  --help               show this help\n\n"
        "Exit codes: 0 success, 1 recording failed, 2 invalid arguments.\n";
    auto bad = [&](const std::string& m) { emit("error: " + m + "\n\n" + usage, stderr); return 2; };

    double secs = -1;
    std::wstring renderPath, uiState = L"idle"; int renderDpi = 96;
    double pauseAt = -1, resumeAt = -1;
    RecordOptions o;
    Settings s;
    loadFile(U(settingsPath()), s);
    o.fps = s.fps; o.quality = s.quality; o.encoder = s.encoder; o.container = s.container; o.captureCursor = s.captureCursor;
    o.scale = s.scale; o.gpuIndex = s.gpuIndex; o.bitrateCap = s.bitrateCap;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--help" || a == L"-h" || a == L"/?") { emit(usage, stdout); return 0; }
        auto value = [&](std::string& out) -> bool {
            if (i + 1 >= argc) return false;
            out = U(argv[++i]);
            return true;
        };
        std::string v;
        if (a == L"--allow-synthetic") { o.allowSynthetic = true; continue; }
        if (a == L"--minimized") continue;
        if (a != L"--record" && a != L"--fps" && a != L"--encoder" && a != L"--container" && a != L"--quality"
            && a != L"--out" && a != L"--log" && a != L"--scale" && a != L"--gpu" && a != L"--bitrate-cap"
            && a != L"--test-hang-finalize" && a != L"--pause-at" && a != L"--resume-at" && a != L"--render-ui" && a != L"--ui-state" && a != L"--dpi")
            return bad("unknown option " + U(a));
        if (!value(v)) return bad(U(a) + " needs a value");
        if (a == L"--record") {
            char* end = nullptr;
            double d = strtod(v.c_str(), &end);
            if (v.empty() || !end || *end || !(d > 0) || d > 86400) return bad("--record must be a number of seconds between 0 and 86400");
            secs = d;
        } else if (a == L"--fps") {
            if (v != "15" && v != "30" && v != "60") return bad("--fps must be 15, 30 or 60");
            o.fps = atoi(v.c_str());
        } else if (a == L"--encoder") {
            if (!fromString(v, o.encoder)) return bad("unknown --encoder '" + v + "'");
        } else if (a == L"--container") {
            if (!fromString(v, o.container)) return bad("unknown --container '" + v + "'");
        } else if (a == L"--quality") {
            if (!fromString(v, o.quality)) return bad("unknown --quality '" + v + "'");
        } else if (a == L"--scale") {
            if (!fromString(v, o.scale)) return bad("unknown --scale '" + v + "'");
        } else if (a == L"--gpu") {
            char* end = nullptr; long g = strtol(v.c_str(), &end, 10);
            if (v.empty() || !end || *end || g < -1 || g > 32) return bad("--gpu must be -1 (Auto) or 0..32");
            o.gpuIndex = (int)g;
        } else if (a == L"--bitrate-cap") {
            char* end = nullptr; unsigned long b = strtoul(v.c_str(), &end, 10);
            if (v.empty() || !end || *end || b > 500000000ul) return bad("--bitrate-cap must be 0..500000000");
            o.bitrateCap = (unsigned)b;
        } else if (a == L"--test-hang-finalize") {
            char* end = nullptr; unsigned long ms = strtoul(v.c_str(), &end, 10);
            if (v.empty() || !end || *end || ms > 60000) return bad("--test-hang-finalize must be 0..60000 ms");
            setTestHangFinalizeMs((DWORD)ms);
            // Finalize budget = finalizeTimeoutMs/2; keep it under the hang so abandon fires (~2 s).
            o.finalizeTimeoutMs = 2000;
        } else if (a == L"--pause-at" || a == L"--resume-at") {
            char* end = nullptr; double d = strtod(v.c_str(), &end);
            if (v.empty() || !end || *end || !(d >= 0) || d > 86400) return bad(U(a) + " must be seconds 0..86400");
            (a == L"--pause-at" ? pauseAt : resumeAt) = d;
        } else if (a == L"--render-ui") {
            if (v.empty()) return bad("--render-ui needs a .bmp path");
            renderPath = W(v);
        } else if (a == L"--ui-state") {
            if (v != "idle" && v != "dropdown") return bad("--ui-state must be idle or dropdown");
            uiState = W(v);
        } else if (a == L"--dpi") {
            char* end = nullptr; long d = strtol(v.c_str(), &end, 10);
            if (v.empty() || !end || *end || d < 72 || d > 384) return bad("--dpi must be 72..384");
            renderDpi = (int)d;
        } else if (a == L"--out") {
            if (v.empty()) return bad("--out needs a file name");
            o.outputPath = W(v);
        }
    }
    if (!renderPath.empty()) {
        g.inst = GetModuleHandleW(nullptr);
        loadIcons();
        int rc = renderUi(renderPath, uiState, renderDpi);
        emit(rc == 0 ? "ok=1 render-ui\n" : "ok=0 render-ui\n", stdout);
        return rc == 0 ? 0 : 1;
    }
    if (secs < 0) return bad("--record SECONDS is required");
    if (o.outputPath.empty()) {
        std::wstring d = s.outputDir.empty() ? defaultOutputDir() : W(s.outputDir);
        SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
        o.outputPath = d + L"\\cli-test" + W(extension(o.container));
    } else {
        wchar_t full[MAX_PATH * 4];
        DWORD n = GetFullPathNameW(o.outputPath.c_str(), MAX_PATH * 4, full, nullptr);
        if (!n || n >= MAX_PATH * 4) return bad("invalid --out path");
        o.outputPath = full;
        size_t slash = o.outputPath.find_last_of(L"\\/");
        if (slash == std::wstring::npos || slash + 1 == o.outputPath.size()) return bad("--out must name a file");
        std::wstring dir = o.outputPath.substr(0, slash);
        if (dir.size() > 2 || (dir.size() == 2 && dir[1] != L':')) SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        DWORD at = GetFileAttributesW((dir.size() == 2 ? dir + L"\\" : dir).c_str());
        if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) return bad("--out folder doesn't exist and can't be created");
    }
    // Share the GUI's single-instance mutex: never run a second capture/encode session beside the app.
    HANDLE cliMtx = CreateMutexW(nullptr, TRUE, L"Local\\CastRecorder.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (cliMtx) CloseHandle(cliMtx);
        emit("ok=0\nmessage=Cast Recorder is already running; close it (tray > Exit) before using --record\n", stdout);
        return 1;
    }
    Recorder r;
    r.start(o);
    // The duration counts from the first captured frame, so the file is as long as requested.
    while (r.running() && !r.started()) Sleep(5);
    // Wall-clock schedule from the first captured frame. --record is total wall time; pauses are inside it.
    const ULONGLONG t0 = GetTickCount64();
    const ULONGLONG ms = (ULONGLONG)(secs * 1000.0);
    bool pausedOnce = false, resumedOnce = false;
    ULONGLONG pausedWall = 0, pauseStart = 0;
    while (r.isActive() && GetTickCount64() - t0 < ms) {
        ULONGLONG el = GetTickCount64() - t0;
        if (pauseAt >= 0 && !pausedOnce && el >= (ULONGLONG)(pauseAt * 1000)) { if (r.pause()) { pauseStart = GetTickCount64(); } pausedOnce = true; }
        if (resumeAt >= 0 && pausedOnce && !resumedOnce && el >= (ULONGLONG)(resumeAt * 1000)) {
            if (r.resume() && pauseStart) pausedWall = GetTickCount64() - pauseStart;
            resumedOnce = true;
        }
        Sleep(5);
    }
    if (pausedOnce && !resumedOnce && pauseStart) pausedWall = GetTickCount64() - pauseStart;
    r.stop();
    // Hang-finalize tests use a short finalize budget; give them headroom then abort.
    DWORD waitMs = 15000;
    if (!r.waitUntilIdle(waitMs)) r.abort();
    r.waitUntilIdle(2000);
    for (int i = 0; i < 100 && abandonedFinalizeLive() > 0; ++i) Sleep(50);
    const RecordResult res = r.result();
    std::string extra = "state=" + std::string(toString(r.state())) + "\n";
    if (pausedOnce) { char b[96]; snprintf(b, sizeof b, "paused_ms=%llu pauses=%llu\n", pausedWall, res.pauses); extra += b; }
    if (res.abandoned) {  // same process: MF is poisoned, a second session must be refused
        bool again = r.start(o);
        extra += again ? "second_start=accepted\n" : "second_start=refused\n";
        if (again) { r.stop(); r.waitUntilIdle(5000); }
    }
    char summary[4096];
    snprintf(summary, sizeof summary,
             "ok=%d frames=%llu duplicated=%llu late=%llu dropped=%llu abandoned=%d\ncapture=%s\nencoder=%s\npath=%s\nfile=%s\nmessage=%s\n",
             res.ok ? 1 : 0, res.frames, res.duplicated, res.late, res.dropped, res.abandoned ? 1 : 0,
             U(res.captureUsed).c_str(), U(res.encoderUsed).c_str(), U(res.pathLog).c_str(),
             U(res.file).c_str(), U(res.message).c_str());
    emit(std::string(summary) + extra + "encoder_log:\n" + U(res.encoderLog), stdout);
    return res.ok ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // Only load DLLs from System32 (no current-directory/app-folder DLL planting).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    enableDpiAwareness();
    for (int i = 1; i < argc; ++i)
        // Any option other than --minimized goes to the console CLI, which validates it (never opens the window).
if (wcscmp(argv[i], L"--minimized") != 0 && (argv[i][0] == L'-' || wcscmp(argv[i], L"/?") == 0)) {
            int rc = runCli(argc, argv);
            CoUninitialize();
            // GUI-subsystem processes sometimes lose WinMain's return code under Start-Process; be explicit.
            ExitProcess((UINT)rc);
        }
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
    loadIcons();
    enumGpus();

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
    if (g.s.hotkeysMigrated) {  // one-time notice; saving writes v2 so it won't show again
        g.s.hotkeysMigrated = false;
        save();
        setStatus(L"Hotkeys changed to Ctrl+Shift+F9 / Ctrl+Shift+F10 (Alt+F9/F10 clash with NVIDIA). You can change them below.", C_WARN);
    }
    g.status = L"Settings save automatically \u00b7 closing keeps Cast Recorder in the tray";
    if (!minimized && !g.s.startMinimized) ShowWindow(g.hwnd, SW_SHOW);
    g.gprev.start(g.hwnd, previewRectNow(), g.s.captureCursor);
    if (!IsWindowVisible(g.hwnd)) g.gprev.setVisible(false);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    g.rec.stop();
    if (!g.rec.waitUntilIdle(3000)) g.rec.abort();
    g.rec.waitUntilIdle(1000);
    if (mtx) CloseHandle(mtx);
    CoUninitialize();
    LocalFree(argv);
    return 0;
}
