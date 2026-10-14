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
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <algorithm>
#include <cwctype>
#include <objidl.h>
#include <shlwapi.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include "../core/hotkey.h"
#include "../core/settings.h"
#include "recorder.h"
#include "preview.h"
#include "audio.h"
#include "applog.h"
#include "windows_enum.h"
#include "resource.h"
#include "version.h"
#include "click_wav.h"
#include <mmsystem.h>
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdiplus.lib")
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
std::wstring g_settingsOverride;  // --settings-file (tests)
bool g_selftest = false;
bool g_selftestRecord = false;    // --selftest-record: press Record right after launch, stop after 2 s, exit          // --selftest-settings: drive the real UI, change settings, exit via X
std::wstring settingsPath() {
    if (!g_settingsOverride.empty()) return g_settingsOverride;
    return cr::appDataDir() + L"\\settings.json";
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
    H_NONE = 0, H_MIN, H_MAX, H_CLOSE, H_TAB_REC, H_TAB_SET, H_SRC, H_DD_FPS, H_DD_QUAL, H_DD_ENC, H_DD_GPU, H_DD_RES, H_DD_CONT,
    H_DD_SYSDEV, H_DD_MICDEV, H_MICCHIP, H_MICICON, H_SYSCHIP, H_PAUSE, H_RECORD, H_STATUSLOG, H_HK0, H_HK1, H_HK2, H_HKPTT,
    H_PATH, H_FOLDERICON, H_BROWSE, H_LOG, H_TG_SYS, H_TG_MIC, H_TG_NS, H_TG_MON, H_TG_PTT, H_SL_VOL, H_SL_GATE, H_CH_MONO, H_CH_STEREO,
    P_BASE = 1000, P_SEARCH = 2000, P_MUTE, P_SL_VOL, P_PAD
};
bool g_renderOnly = false;  // --render-ui: no windows, no animations

const int kFps[] = {15, 30, 60};
const wchar_t* kFpsL[] = {L"15 FPS", L"30 FPS", L"60 FPS"};
const Quality kQual[] = {Quality::Low, Quality::Medium, Quality::High, Quality::Ultra};
const wchar_t* kQualL[] = {L"Low", L"Medium", L"High", L"Ultra"};
const Encoder kEnc[] = {Encoder::Auto, Encoder::NVENC, Encoder::AMF, Encoder::QSV, Encoder::Software};
const OutputScale kRes[] = {OutputScale::Native, OutputScale::P1080, OutputScale::P720};
const wchar_t* kResL[] = {L"Native", L"1080p", L"720p"};
const Container kCont[] = {Container::MP4, Container::MKV, Container::MOV};
const wchar_t* kContL[] = {L"MP4", L"MKV", L"MOV"};

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr int HK_TOGGLE = 1, HK_FOLDER = 2, HK_PAUSE = 3, HK_MIC = 4;
constexpr UINT TIMER_TICK = 1, TIMER_PREVIEW = 2, TIMER_SELFTEST = 3, TIMER_FRAME = 6;
constexpr int BASE_W = 1100, BASE_H = 700, MIN_W = 960, MIN_H = 620;  // v2 SPEC: Window
// UI watchdog > worst-case teardown: capture Close 2 s + Finalize (8000/2) + remux (8000/2) + 3 s margin.
constexpr ULONGLONG kWatchdogMs = 2000 + 8000 / 2 + 8000 / 2 + 3000;

struct App {
    HINSTANCE inst{};
    HWND hwnd{};
    UINT dpi = 96;
    HFONT fTitle{}, fBody{}, fLabel{}, fSmall{}, fSection{}, fBtn{}, fMono{}, fTimer{};
    HICON icoBig{}, icoSmall{}, icoTray{}, icoTrayRec{};
    Settings s;
    std::wstring settingsFile;
    Recorder rec;
    int hover = H_NONE, pressed = H_NONE;
    int capturing = -1;         // hotkey field being edited (0 start/stop, 1 pause), -1 none
    UINT liveMods = 0;
    std::wstring hkMsg[3];
    std::vector<AudioDevice> outs;  // output devices for system audio (index+1 in the dropdown; 0 = default)
    CaptureTarget target;
    COLORREF hkMsgColor[3]{};
    std::vector<AudioDevice> mics;  // index+2 in the Microphone dropdown (0 Off, 1 Default)
    bool micMuted = false;
    double lastBeepMs = 0;
    bool warmLogged = false;
    bool recPending = false;  // a recording ran whose result isn't logged yet
    Recorder warm;  // startup warm-up: fills the encoder probe cache and loads MF/AAC so Record starts fast
    std::wstring status;
    COLORREF statusColor = C_TEXT2;
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
    // The saved GPU is matched by its description; the list position is only a tie-break for identical names.
    if (!g.s.gpuName.empty()) {
        int found = -1;
        for (int k = 0; k < (int)g.gpus.size(); ++k)
            if (U(g.gpus[k]) == g.s.gpuName && (found < 0 || k == g.s.gpuIndex)) found = k;
        g.s.gpuIndex = found;  // GPU gone -> Auto (name kept, so it comes back when the GPU does)
    } else if (g.s.gpuIndex >= (int)g.gpus.size()) g.s.gpuIndex = -1;
}

// ---------- tray ----------
void trayUpdate(DWORD) {}  // no tray icon: the window (taskbar) is the only surface
[[maybe_unused]] static void trayUpdateUnused(DWORD msg) {
    NOTIFYICONDATAW n{sizeof n};
    n.hWnd = g.hwnd; n.uID = 1; n.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE; n.uCallbackMessage = WM_TRAY;
    n.hIcon = g.rec.running() ? g.icoTrayRec : g.icoTray;
    std::wstring tip = g.rec.isPaused() ? L"Cast Recorder \u2013 Paused" : g.rec.running() ? L"Cast Recorder \u2013 Recording" : L"Cast Recorder \u2013 Ready";
    if (!g.s.toggleRecord.empty()) tip += L" (" + W(format(g.s.toggleRecord)) + L")";
    wcsncpy(n.szTip, tip.c_str(), ARRAYSIZE(n.szTip) - 1);
    if (Shell_NotifyIconW(msg, &n) && msg == NIM_ADD) g.trayAdded = true;
}

// ---------- hotkeys ----------
void unregisterAll() { UnregisterHotKey(g.hwnd, HK_TOGGLE); UnregisterHotKey(g.hwnd, HK_FOLDER); UnregisterHotKey(g.hwnd, HK_PAUSE); UnregisterHotKey(g.hwnd, HK_MIC); }
void registerAll() {
    if (g.rec.running()) return;  // the live session keeps its hotkeys; never unregister mid-recording
    unregisterAll();
    Hotkey* keys[3] = {&g.s.toggleRecord, &g.s.pauseToggle, &g.s.micMute};
    const int ids[3] = {HK_TOGGLE, HK_PAUSE, HK_MIC};
    for (int i = 0; i < 3; ++i) {
        const Hotkey& k = *keys[i];
        g.hkMsg[i].clear();
        if (k.empty()) { g.hkMsgColor[i] = C_MUTED; continue; }
        Issue is = validate(k);
        if (is.severity == Severity::Error) { g.hkMsg[i] = W(is.message); g.hkMsgColor[i] = C_ERR; continue; }
        if ((i == 1 && k == g.s.toggleRecord) || (i == 2 && (k == g.s.toggleRecord || k == g.s.pauseToggle))) { g.hkMsg[i] = L"Same combo as Start / Stop"; g.hkMsgColor[i] = C_ERR; continue; }
        if (!RegisterHotKey(g.hwnd, ids[i], k.mods | MOD_NOREPEAT, k.vk)) {
            g.hkMsg[i] = L"In use by another app"; g.hkMsgColor[i] = C_ERR; continue;
        }
        if (is.severity == Severity::Warning) { g.hkMsg[i] = W(is.message); g.hkMsgColor[i] = C_WARN; }
        else g.hkMsgColor[i] = C_OK;
    }
    // Open-folder combo has no field in the new layout but stays active if set and not clashing.
    if (!g.s.openFolder.empty() && !(g.s.openFolder == g.s.toggleRecord) && !(g.s.openFolder == g.s.pauseToggle) && !(g.s.openFolder == g.s.micMute) &&
        validate(g.s.openFolder).severity != Severity::Error)
        RegisterHotKey(g.hwnd, HK_FOLDER, g.s.openFolder.mods | MOD_NOREPEAT, g.s.openFolder.vk);
}

void setStatus(const std::wstring& s, COLORREF c);
std::wstring settingsSummary(const Settings& s) {
    std::string j = toJson(s);
    for (auto& c : j) if (c == '\n') c = ' ';
    return W(j);
}
Settings g_lastSaved;  // what's on disk: saves are logged as a diff against it
std::wstring settingsDiff(const Settings& a, const Settings& b) {
    auto lines = [](const Settings& x) { std::vector<std::string> v; std::string j = toJson(x), cur;
        for (char c : j) { if (c == '\n') { v.push_back(cur); cur.clear(); } else cur += c; } return v; };
    auto la = lines(a), lb = lines(b);
    std::wstring d;
    for (size_t i = 0; i < lb.size(); ++i) if (i >= la.size() || la[i] != lb[i]) {
        std::string t = lb[i]; while (!t.empty() && (t[0] == ' ')) t.erase(0, 1); if (!t.empty() && t.back() == ',') t.pop_back();
        d += W(t) + L"; ";
    }
    return d.empty() ? L"(no change)" : d;
}
void save() {
    if (g.hwnd) g.s.maximized = IsZoomed(g.hwnd) != 0;
    if (saveFile(U(g.settingsFile), g.s)) {
        std::wstring d = settingsDiff(g_lastSaved, g.s);
        if (d != L"(no change)") cr::appLog(L"settings saved: " + d);
        g_lastSaved = g.s;
        return;
    }
    DWORD e = GetLastError();
    cr::appLog(L"settings SAVE FAILED (" + std::to_wstring(e) + L") to " + g.settingsFile);
    setStatus(L"Couldn't save settings (error " + std::to_wstring(e) + L")", C_ERR);
}

std::wstring outputDir() { return g.s.outputDir.empty() ? defaultOutputDir() : W(g.s.outputDir); }
void setStatus(const std::wstring& s, COLORREF c) { g.status = s; g.statusColor = c; if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE); }

// Our own tones (res/sounds, scripts/make-sounds.py): soft bell chimes at -24 dBFS, a -28 dBFS tick.
void playTone(int id) {
    LARGE_INTEGER a, b, f; QueryPerformanceCounter(&a);
    if (!PlaySoundW(MAKEINTRESOURCEW(id), g.inst, SND_RESOURCE | SND_ASYNC | SND_NODEFAULT))
        PlaySoundW(reinterpret_cast<LPCWSTR>(kClickWav), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
    g.lastBeepMs = (double)(b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;  // SND_ASYNC: returns immediately
}

// ---------- preview ----------
// Cheap GDI thumbnail of the primary monitor into a small DIB. Runs at ~2 fps idle and is frozen while
// recording so it never competes with the capture/encode path or a game for GPU time.
RECT previewSourceRect();
void updatePreview() {
    if (g.rec.running() || g.gprev.live()) return;
    RECT src = previewSourceRect();
    int sw = src.right - src.left, sh = src.bottom - src.top;
    if (sw <= 0 || sh <= 0) return;
    int tw = S(640), th = tw * 9 / 16;
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
    HBRUSH b = CreateSolidBrush(RGB(0, 0, 0)); FillRect(mem, &all, b); DeleteObject(b);
    g.prevLive = StretchBlt(mem, dx, dy, dw, dh, screen, src.left, src.top, sw, sh, SRCCOPY | CAPTUREBLT) != 0;
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

RECT previewSourceRect() {
    if (g.target.kind == CaptureTarget::Window && g.target.hwnd && IsWindow(g.target.hwnd)) { RECT r{}; GetWindowRect(g.target.hwnd, &r); return r; }
    if (g.target.kind == CaptureTarget::Monitor) { MONITORINFO mi{sizeof mi}; if (g.target.mon && GetMonitorInfoW(g.target.mon, &mi)) return mi.rcMonitor; }
    if (g.target.kind == CaptureTarget::AllDisplays)
        return RECT{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                    GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

// ---------- recording ----------
// Per-recording diagnostics: <output folder>\CastRecorder.log (appended): path, encoder probe/setup HRESULTs.
void writeSessionLog(const RecordResult& r, const wchar_t* what = L"recording") {
    wchar_t bb[64]; swprintf(bb, 64, L"%.1f", g.lastBeepMs);
    std::wstring txt = std::wstring(L"=== ") + what + L" === ok=" + (r.ok ? L"1" : L"0") + L" frames=" + std::to_wstring(r.frames) +
                       L" dropped=" + std::to_wstring(r.dropped) + L" file=" + r.file + L"\ncapture=" + r.captureUsed + L"\nencoder=" + r.encoderUsed +
                       L"\npath=" + r.pathLog + L"\nstart_beep_call_ms=" + bb + L" (async, after first frame)\nmessage=" + r.message + L"\n" + r.encoderLog;
    cr::appLog(txt);
}

RecordOptions optionsFromSettings() {
    RecordOptions o;
    o.fps = g.s.fps; o.quality = g.s.quality; o.encoder = g.s.encoder; o.container = g.s.container;
    o.captureCursor = g.s.captureCursor; o.scale = g.s.scale; o.gpuIndex = g.s.gpuIndex; o.bitrateCap = g.s.bitrateCap;
    o.pauseTones = g.s.pauseTones;
    o.systemAudio = g.s.systemAudio; o.mic = g.s.micEnabled; o.micId = W(g.s.micDevice);
    o.systemId = W(g.s.systemDevice); o.channels = g.s.channels;
    o.allDisplays = g.target.kind == CaptureTarget::AllDisplays;
    o.monitor = g.target.kind == CaptureTarget::Monitor ? g.target.mon : nullptr;
    o.window = g.target.kind == CaptureTarget::Window ? g.target.hwnd : nullptr;
    return o;
}
// Background warm-up (startup and when encoder-relevant settings change): same device/capture/encoder
// selection as a real recording, but exits before the loop. Probe results are cached process-wide.
void startWarmup() {
    if (g.warm.running() || g.rec.running()) return;
    g.warmLogged = false;
    RecordOptions o = optionsFromSettings();
    o.warmupOnly = true; o.systemAudio = false; o.mic = false; o.notify = g.hwnd;
    if (o.window && !IsWindow(o.window)) o.window = nullptr;
    wchar_t tp[MAX_PATH]; GetTempPathW(MAX_PATH, tp);
    o.outputPath = std::wstring(tp) + L"castrec-warmup" + std::to_wstring(GetCurrentProcessId()) + W(extension(g.s.container));
    LARGE_INTEGER q; QueryPerformanceCounter(&q); o.pressQpc = q.QuadPart;
    g.warm.start(o);
}

void forceQuit() {
    g.quitting = true;
    g.rec.stop();
    setStatus(L"Closing\u2026", C_TEXT2);
    ULONGLONG end = GetTickCount64() + kWatchdogMs;  // let the file finish saving before exit
    while (g.rec.running() && GetTickCount64() < end) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        if (!g.rec.waitUntilIdle(50)) Sleep(10);
    }
    if (g.rec.running()) g.rec.abort();
    g.rec.waitUntilIdle(500);
    if (g.recPending) { g.recPending = false; writeSessionLog(g.rec.result(), L"recording (saved on exit)"); }
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
    LARGE_INTEGER pq; QueryPerformanceCounter(&pq);
    // A warm-up still probing the same encoder would contend with this session (AMD session limits); it only
    // runs for a couple of seconds after launch or a settings change, and its time shows in the latency log.
    // No waiting on the launch warm-up: capture starts now and frames are buffered while the encoder (whose
    // probe verdict the warm-up is producing) finishes initialising.
    RecordOptions o = optionsFromSettings();
    if (g.target.kind == CaptureTarget::Window && (!o.window || !IsWindow(o.window))) { setStatus(L"Window closed – pick a source", C_WARN); return; }
    o.pressQpc = pq.QuadPart;
    o.notify = g.hwnd;
    o.outputPath = dir + L"\\" + name + W(extension(g.s.container));
    registerAll();
    if (!g.rec.start(o)) { setStatus(L"Already recording", C_WARN); return; }
    g.recPending = true;
    g.gprev.setRecording(true);
    g.rec.setMicMuted(g.micMuted);
    // start tone plays on WM_APP_RECORDER_STARTED (first captured frame), never blocking the start path
    SetTimer(g.hwnd, TIMER_TICK, 250, nullptr);
    setStatus(L"Recording\u2026", C_ACCENT_B);
    trayUpdate(NIM_MODIFY);
}
void stopRecording() {
    if (!g.rec.isActive()) return;
    playTone(IDR_SND_STOP);
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
        if (g.s.pauseTones) playTone(IDR_SND_TICK);  // off by default: the mic would record it
        if (g.rec.isPaused()) setStatus(L"Paused", C_WARN);
        else setStatus(L"Recording\u2026", C_ACCENT_B);
        trayUpdate(NIM_MODIFY);
    }
}
void toggleMic();
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

std::wstring modsText(UINT m) {
    std::wstring s;
    if (m & MOD_CTRL_) s += L"Ctrl + ";
    if (m & MOD_ALT_) s += L"Alt + ";
    if (m & MOD_SHIFT_) s += L"Shift + ";
    if (m & MOD_WIN_) s += L"Win + ";
    return s;
}
void openLog() {
    std::wstring p = cr::appLogPath();
    cr::appLog(L"log opened from UI");
    if (OpenClipboard(g.hwnd)) {
        EmptyClipboard();
        size_t n = (p.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL hm = GlobalAlloc(GMEM_MOVEABLE, n)) { memcpy(GlobalLock(hm), p.c_str(), n); GlobalUnlock(hm); if (!SetClipboardData(CF_UNICODETEXT, hm)) GlobalFree(hm); }
        CloseClipboard();
    }
    ShellExecuteW(g.hwnd, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    setStatus(L"Log path copied: " + p, C_TEXT2);
}
void endCapture();
void selftestSettings();
void paintToNull();
// ===================== v2 UI (Vey, cast-recorder-ui/v2/SPEC.md + tokens.json) =====================
// Everything is drawn with GDI+ in logical 96-DPI units under a world scale transform. Popups (dropdown lists,
// source picker, mic popover) are layered top-level windows (UpdateLayeredWindow) so they float above the GPU
// preview's swapchain child; the preview's chips are small layered child windows over it.
namespace gp = Gdiplus;

const unsigned T_BG = 0x0E1014, T_SURF = 0x161A20, T_ELEV = 0x1C2128, T_PREV = 0x0A0C0F, T_INPUT = 0x1A1F27, T_INPUT_H = 0x222832,
               T_TEXT = 0xF2F3F5, T_TEXT2 = 0xA8ADB7, T_MUTED = 0x6B7280, T_DIS = 0x4B5563, T_ACC = 0xE5243B, T_ACC_B = 0xFF6B6B,
               T_ACC_H = 0xF0354A, T_ACC_P = 0xC41E33, T_OK = 0x22C55E, T_WARN = 0xF59E0B, T_ERR = 0xEF4444, T_LISTH = 0x232A34,
               T_TRACK = 0x2A303A, T_CLOSEH = 0xE81123;
gp::Color C(unsigned rgb, int a = 255) { return gp::Color((BYTE)a, (BYTE)((rgb >> 16) & 255), (BYTE)((rgb >> 8) & 255), (BYTE)(rgb & 255)); }
gp::Color WA(int a) { return gp::Color((BYTE)a, 255, 255, 255); }  // #FFFFFFxx overlays/borders
const int A_SUBTLE = 0x14, A_DEFAULT = 0x1F, A_STRONG = 0x33, A_HOVER = 0x0A, A_PRESSED = 0x14;
COLORREF toRef(unsigned rgb) { return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }
unsigned fromRef(COLORREF c) { return ((unsigned)GetRValue(c) << 16) | ((unsigned)GetGValue(c) << 8) | GetBValue(c); }

float scale() { return g.dpi / 96.f; }
using RF = gp::RectF;
RF inflate(RF r, float d) { return RF(r.X - d, r.Y - d, r.Width + 2 * d, r.Height + 2 * d); }
bool inRF(const RF& r, float x, float y) { return x >= r.X && y >= r.Y && x < r.X + r.Width && y < r.Y + r.Height; }

// ---------- fonts (Segoe UI; Semibold via its own family) ----------
struct Fonts { gp::Font *title = nullptr, *body = nullptr, *label = nullptr, *caption = nullptr, *section = nullptr, *btn = nullptr,
               *btnP = nullptr, *status = nullptr, *hotkey = nullptr, *tiny = nullptr; } F;
gp::FontFamily* g_ff = nullptr; gp::FontFamily* g_ffSb = nullptr;
void makeUiFonts() {
    for (gp::Font* f : {F.title, F.body, F.label, F.caption, F.section, F.btn, F.btnP, F.status, F.hotkey, F.tiny}) delete f;
    if (!g_ff) {
        for (const wchar_t* face : {L"Segoe UI", L"Segoe UI Variable", L"Arial"}) {
            g_ff = new gp::FontFamily(face);
            if (g_ff->IsAvailable() && g_ff->GetLastStatus() == gp::Ok) break;
            delete g_ff; g_ff = nullptr;
        }
        if (!g_ff) g_ff = gp::FontFamily::GenericSansSerif()->Clone();
        g_ffSb = new gp::FontFamily(L"Segoe UI Semibold");
        if (!g_ffSb->IsAvailable()) { delete g_ffSb; g_ffSb = nullptr; }
    }
    auto sb = [](float px) { return g_ffSb ? new gp::Font(g_ffSb, px, gp::FontStyleRegular, gp::UnitPixel) : new gp::Font(g_ff, px, gp::FontStyleBold, gp::UnitPixel); };
    auto rg = [](float px) { return new gp::Font(g_ff, px, gp::FontStyleRegular, gp::UnitPixel); };
    F.title = sb(13); F.body = rg(13); F.label = rg(12); F.caption = rg(11); F.section = sb(11); F.btn = sb(13); F.btnP = sb(14);
    F.status = rg(12); F.hotkey = rg(12); F.tiny = sb(10);
}

// ---------- drawing helpers ----------
void rrPath(gp::GraphicsPath& p, RF r, float rad) {
    if (rad <= 0.01f) { p.AddRectangle(r); return; }
    float d = std::min(rad * 2, std::min(r.Width, r.Height));
    p.AddArc(r.X, r.Y, d, d, 180, 90); p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90); p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}
void fillRR(gp::Graphics& gr, RF r, float rad, gp::Color c) { gp::GraphicsPath p; rrPath(p, r, rad); gp::SolidBrush b(c); gr.FillPath(&b, &p); }
void strokeRR(gp::Graphics& gr, RF r, float rad, gp::Color c, float w = 1.f) {
    RF q(r.X + w / 2, r.Y + w / 2, r.Width - w, r.Height - w);
    gp::GraphicsPath p; rrPath(p, q, std::max(0.f, rad - w / 2)); gp::Pen pen(c, w); gr.DrawPath(&pen, &p);
}
void card(gp::Graphics& gr, RF r, float rad, gp::Color bg, gp::Color border) { fillRR(gr, r, rad, bg); strokeRR(gr, r, rad, border); }
void fillCircle(gp::Graphics& gr, float cx, float cy, float rad, gp::Color c) { gp::SolidBrush b(c); gr.FillEllipse(&b, cx - rad, cy - rad, rad * 2, rad * 2); }
void hline(gp::Graphics& gr, float x0, float x1, float y, gp::Color c) { gp::SolidBrush b(c); gr.FillRectangle(&b, x0, y, x1 - x0, 1.f); }
enum { AL_L = 0, AL_C = 1, AL_R = 2 };
void txt(gp::Graphics& gr, const std::wstring& s, RF r, gp::Font* f, gp::Color c, int align = AL_L, bool ellipsis = true, bool path = false) {
    gp::StringFormat sf(gp::StringFormat::GenericTypographic());
    sf.SetAlignment(align == AL_L ? gp::StringAlignmentNear : align == AL_C ? gp::StringAlignmentCenter : gp::StringAlignmentFar);
    sf.SetLineAlignment(gp::StringAlignmentCenter);
    sf.SetFormatFlags(sf.GetFormatFlags() | gp::StringFormatFlagsNoWrap | gp::StringFormatFlagsMeasureTrailingSpaces);
    sf.SetTrimming(path ? gp::StringTrimmingEllipsisPath : ellipsis ? gp::StringTrimmingEllipsisCharacter : gp::StringTrimmingNone);
    gp::SolidBrush b(c);
    gr.DrawString(s.c_str(), (INT)s.size(), f, r, &sf, &b);
}
float txtW(gp::Graphics& gr, const std::wstring& s, gp::Font* f) {
    gp::StringFormat sf(gp::StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | gp::StringFormatFlagsMeasureTrailingSpaces);
    RF out; gr.MeasureString(s.c_str(), (INT)s.size(), f, gp::PointF(0, 0), &sf, &out);
    return out.Width;
}

// ---------- icons (embedded PNGs, white; tinted at draw time) ----------
enum Glyph { G_RECORD, G_PAUSE, G_STOP, G_MIC, G_MICMUTED, G_SYS, G_SYSMUTED, G_FOLDER, G_LOG, G_SETTINGS, G_MINIMIZE, G_MAXIMIZE,
             G_RESTORE, G_CLOSE, G_CHEVRON, G_CHECK, G_MONITOR, G_WINDOW, G_FULLSCREEN, G_COUNT };
gp::Bitmap* g_icons[G_COUNT][4] = {};
gp::Bitmap* loadPngResource(int id) {
    HRSRC r = FindResourceW(g.inst, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return nullptr;
    HGLOBAL h = LoadResource(g.inst, r); DWORD n = SizeofResource(g.inst, r);
    const void* data = LockResource(h);
    if (!data || !n) return nullptr;
    IStream* s = SHCreateMemStream((const BYTE*)data, n);
    if (!s) return nullptr;
    gp::Bitmap* tmp = gp::Bitmap::FromStream(s);
    gp::Bitmap* out = nullptr;
    if (tmp && tmp->GetLastStatus() == gp::Ok) {  // decode now: the stream goes away
        out = new gp::Bitmap(tmp->GetWidth(), tmp->GetHeight(), PixelFormat32bppPARGB);
        gp::Graphics gg(out); gg.DrawImage(tmp, 0, 0, (INT)tmp->GetWidth(), (INT)tmp->GetHeight());
    }
    delete tmp; s->Release();
    return out;
}
void loadUiIcons() {
    for (int i = 0; i < G_COUNT; ++i)
        for (int k = 0; k < 4; ++k) if (!g_icons[i][k]) g_icons[i][k] = loadPngResource(1000 + i * 4 + k);
}
// 16 px glyphs: 16 below 125 %, 20 at 125 %, 24 at 150 %, 32 at 200 %+ (SPEC: Window / DPI)
int iconIndex(float logicalSize) {
    float px = logicalSize * scale();
    return px < 19.5f ? 0 : px < 23.5f ? 1 : px < 31.5f ? 2 : 3;
}
void icon(gp::Graphics& gr, int glyph, float x, float y, float size, gp::Color tint, float rotateDeg = 0) {
    gp::Bitmap* b = g_icons[glyph][iconIndex(size)];
    if (!b) return;
    gp::ColorMatrix m = {{{0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, tint.GetA() / 255.f, 0},
                          {tint.GetR() / 255.f, tint.GetG() / 255.f, tint.GetB() / 255.f, 0, 1}}};
    gp::ImageAttributes ia; ia.SetColorMatrix(&m);
    gp::GraphicsState st = gr.Save();
    if (rotateDeg != 0) { gr.TranslateTransform(x + size / 2, y + size / 2); gr.RotateTransform(rotateDeg); gr.TranslateTransform(-(x + size / 2), -(y + size / 2)); }
    gr.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
    gr.DrawImage(b, RF(x, y, size, size), 0, 0, (gp::REAL)b->GetWidth(), (gp::REAL)b->GetHeight(), gp::UnitPixel, &ia);
    gr.Restore(st);
}
// Window icons for the source list (HICON -> premultiplied bitmap, cached per HICON)
std::vector<std::pair<HICON, gp::Bitmap*>> g_winIcons;
gp::Bitmap* winIcon(HICON ic) {
    if (!ic) return nullptr;
    for (auto& p : g_winIcons) if (p.first == ic) return p.second;
    const int n = 32;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = n; bi.bmiHeader.biHeight = -n;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    gp::Bitmap* out = nullptr;
    if (hb) {
        HGDIOBJ o = SelectObject(dc, hb);
        memset(bits, 0, n * n * 4);
        DrawIconEx(dc, 0, 0, ic, n, n, 0, nullptr, DI_NORMAL);
        GdiFlush();
        uint32_t* px = (uint32_t*)bits; bool anyA = false;
        for (int i = 0; i < n * n; ++i) if (px[i] >> 24) { anyA = true; break; }
        if (!anyA) for (int i = 0; i < n * n; ++i) if (px[i]) px[i] |= 0xFF000000u;  // legacy icons without alpha
        out = new gp::Bitmap(n, n, PixelFormat32bppPARGB);
        gp::BitmapData bd; gp::Rect lr(0, 0, n, n);
        if (out->LockBits(&lr, gp::ImageLockModeWrite, PixelFormat32bppPARGB, &bd) == gp::Ok) {
            for (int y = 0; y < n; ++y) memcpy((BYTE*)bd.Scan0 + y * bd.Stride, px + y * n, n * 4);
            out->UnlockBits(&bd);
        }
        SelectObject(dc, o); DeleteObject(hb);
    }
    DeleteDC(dc);
    if (g_winIcons.size() > 200) { for (auto& p : g_winIcons) delete p.second; g_winIcons.clear(); }
    g_winIcons.push_back({ic, out});
    return out;
}
void drawWinIcon(gp::Graphics& gr, HICON ic, float x, float y, float size) {
    if (gp::Bitmap* b = winIcon(ic)) {
        gr.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
        gr.DrawImage(b, RF(x, y, size, size));
    } else icon(gr, G_WINDOW, x, y, size, C(T_TEXT2));
}

// ---------- motion (SPEC: Motion; tokens.json motion) ----------
double nowMs() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER q; QueryPerformanceCounter(&q); return (double)q.QuadPart * 1000.0 / (double)f.QuadPart; }
double cubicBezier(double x1, double y1, double x2, double y2, double t) {
    if (t <= 0) return 0; if (t >= 1) return 1;
    auto bx = [&](double s) { return 3 * (1 - s) * (1 - s) * s * x1 + 3 * (1 - s) * s * s * x2 + s * s * s; };
    auto by = [&](double s) { return 3 * (1 - s) * (1 - s) * s * y1 + 3 * (1 - s) * s * s * y2 + s * s * s; };
    double lo = 0, hi = 1, s = t;
    for (int i = 0; i < 40; ++i) { s = (lo + hi) / 2; if (bx(s) < t) lo = s; else hi = s; }
    return by(s);
}
enum Ease { E_LINEAR, E_OUTEXPO, E_IN, E_STD, E_OUT, E_INOUT };
double ease(int e, double t) {
    switch (e) {
        case E_OUTEXPO: return cubicBezier(0.16, 1, 0.3, 1, t);
        case E_IN: return cubicBezier(0.4, 0, 1, 1, t);
        case E_STD: return cubicBezier(0.2, 0, 0, 1, t);
        case E_OUT: return cubicBezier(0, 0, 0.58, 1, t);
        case E_INOUT: return cubicBezier(0.42, 0, 0.58, 1, t);
    }
    return t < 0 ? 0 : t > 1 ? 1 : t;
}
bool animationsOn() {
    BOOL on = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) on = TRUE;
    return on != FALSE && !g_renderOnly;
}
void kickAnim();
struct Anim {
    float from = 0, to = 0; double t0 = 0; int ms = 0, e = E_LINEAR;
    void set(float v) { from = to = v; ms = 0; }
    void go(float v, int dur, int easing) {
        if (v == to && !active()) { from = to; ms = 0; return; }
        from = value(); to = v; t0 = nowMs(); e = easing; ms = animationsOn() ? dur : 0;
        if (ms) kickAnim();
    }
    double progress() const { return ms ? std::min(1.0, (nowMs() - t0) / ms) : 1.0; }
    float value() const { return ms ? from + (to - from) * (float)ease(e, progress()) : to; }
    bool active() const { return ms && nowMs() - t0 < ms; }
};
// ---------- UI state ----------
struct Region { RF r; int id; };
std::vector<Region> g_regs;          // main window hit regions (logical px), rebuilt every paint
std::vector<RF> g_meterRects;        // repainted on level changes
RF g_recordRect, g_statusRect;
std::map<int, Anim> g_hov, g_tog;    // hover overlays (100 ms) / toggle knobs (140 ms)
Anim g_view, g_chan;                 // view switch (150 ms) / channels thumb (160 ms)
int g_viewIdx = 0;                   // 0 Record, 1 Settings
int g_focus = H_NONE;
bool g_kbUsed = false;               // focus rings only after Tab is used
int g_drag = H_NONE;                 // slider being dragged (main window)
float g_meter[2] = {0, 0};           // displayed level 0..1 (mic, system), 300 ms decay
double g_meterT = 0;
bool g_fakeRec = false, g_fakePaused = false;  // --render-ui recording state
std::vector<DisplayInfo> g_displays;

float hovA(int id) { auto it = g_hov.find(id); return it == g_hov.end() ? 0.f : it->second.value(); }
void addReg(RF r, int id) { g_regs.push_back({r, id}); }
gp::Color mix(unsigned a, unsigned b, float t) {
    auto ch = [&](int s) { return (BYTE)std::lround(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t); };
    return gp::Color(255, ch(16), ch(8), ch(0));
}
bool uiActive() { return g_fakeRec || g.rec.isActive(); }
bool uiPaused() { return g_fakePaused || g.rec.isPaused(); }
bool uiBusy() { return g_fakeRec || g.rec.running(); }
std::wstring hkText(const Hotkey& h) {
    std::wstring s = W(format(h)), o;
    for (wchar_t c : s) { if (c == L'+') o += L" + "; else o += c; }
    return o;
}

// ---------- sources ----------
struct SrcRow { int kind = 0; std::wstring label, right; HMONITOR mon = nullptr; HWND hwnd = nullptr; HICON ic = nullptr; std::wstring exe, device; float y = 0, h = 0; };
enum { SR_ALL = 0, SR_MON = 1, SR_WIN = 2, SR_DIV = 10, SR_SEARCH = 11, SR_CAPTION = 12 };
std::wstring dims(int w, int h) { return std::to_wstring(w) + L" \u00d7 " + std::to_wstring(h); }
RECT virtualRect() {
    RECT u{0, 0, 0, 0}; bool first = true;
    for (auto& d : g_displays) { if (first) { u = d.rect; first = false; } else { u.left = std::min(u.left, d.rect.left); u.top = std::min(u.top, d.rect.top); u.right = std::max(u.right, d.rect.right); u.bottom = std::max(u.bottom, d.rect.bottom); } }
    return u;
}
const DisplayInfo* displayFor(HMONITOR m) {
    for (auto& d : g_displays) if (d.mon == m) return &d;
    for (auto& d : g_displays) if (d.primary) return &d;
    return g_displays.empty() ? nullptr : &g_displays[0];
}
std::wstring g_srcTitle;  // window title for the field
HICON g_srcIcon = nullptr;
std::wstring sourceText() {
    if (g.target.kind == CaptureTarget::Window) return g_srcTitle.empty() ? L"Window" : g_srcTitle;
    if (g.target.kind == CaptureTarget::AllDisplays) { RECT u = virtualRect(); return L"Full screen \u00b7 " + dims(u.right - u.left, u.bottom - u.top); }
    const DisplayInfo* d = displayFor(g.target.mon);
    return d ? L"Monitor " + std::to_wstring(d->index) + L" \u00b7 " + dims(d->rect.right - d->rect.left, d->rect.bottom - d->rect.top) : L"Monitor";
}
void sourceSize(int& w, int& h) {
    w = h = 0;
    if (g.target.kind == CaptureTarget::Window) { RECT r{}; if (g.target.hwnd && GetWindowRect(g.target.hwnd, &r)) { w = r.right - r.left; h = r.bottom - r.top; } }
    else if (g.target.kind == CaptureTarget::AllDisplays) { RECT u = virtualRect(); w = u.right - u.left; h = u.bottom - u.top; }
    else if (const DisplayInfo* d = displayFor(g.target.mon)) { w = d->rect.right - d->rect.left; h = d->rect.bottom - d->rect.top; }
    if (g.gprev.live() && g.gprev.sourceWidth()) { w = (int)g.gprev.sourceWidth(); h = (int)g.gprev.sourceHeight(); }
}
// Saved source -> live target. A saved window that's gone (or a monitor that's unplugged) falls back to Full
// screen for this session without overwriting the saved choice, so it's picked up again when it's back.
void resolveSource() {
    g_displays = listDisplays();
    g.target = CaptureTarget{}; g.target.kind = CaptureTarget::AllDisplays;
    g_srcTitle.clear(); g_srcIcon = nullptr;
    if (g.s.sourceKind == "monitor") {
        g.target.kind = CaptureTarget::Monitor;
        g.target.mon = nullptr;
        for (auto& d : g_displays) if (U(d.device) == g.s.sourceMonitor) g.target.mon = d.mon;
        if (!g.target.mon) {
            if (!g.s.sourceMonitor.empty()) cr::appLog(L"source: saved monitor " + W(g.s.sourceMonitor) + L" not found, using the primary monitor");
            if (const DisplayInfo* d = displayFor(nullptr)) g.target.mon = d->mon;
        }
    } else if (g.s.sourceKind == "window") {
        HWND h = resolveSavedWindow(W(g.s.sourceExe), W(g.s.sourceTitle));
        if (h) {
            g.target.kind = CaptureTarget::Window; g.target.hwnd = h;
            for (auto& w : listCaptureWindows()) if (w.hwnd == h) { g_srcTitle = w.title; g_srcIcon = w.icon; }
        } else {
            cr::appLog(L"source: saved window " + W(g.s.sourceExe) + L" \"" + W(g.s.sourceTitle) + L"\" not found, using Full screen");
            setStatus(L"Saved window isn't open \u2013 using Full screen", toRef(T_TEXT2));
        }
    }
    g.gprev.setTarget(g.target);
}
void selectSource(const SrcRow& r) {
    if (r.kind == SR_ALL) { g.target = CaptureTarget{}; g.target.kind = CaptureTarget::AllDisplays; g.s.sourceKind = "all"; g_srcTitle.clear(); }
    else if (r.kind == SR_MON) { g.target = CaptureTarget{}; g.target.kind = CaptureTarget::Monitor; g.target.mon = r.mon; g.s.sourceKind = "monitor"; g.s.sourceMonitor = U(r.device); g_srcTitle.clear(); }
    else if (r.kind == SR_WIN) {
        g.target = CaptureTarget{}; g.target.kind = CaptureTarget::Window; g.target.hwnd = r.hwnd;
        g.s.sourceKind = "window"; g.s.sourceExe = U(r.exe); g.s.sourceTitle = U(r.label); g_srcTitle = r.label; g_srcIcon = r.ic;
    } else return;
    g.gprev.setTarget(g.target);
    save();
    setStatus(L"Ready", toRef(T_TEXT2));
    startWarmup();
}
bool rowSelected(const SrcRow& r) {
    if (r.kind == SR_ALL) return g.target.kind == CaptureTarget::AllDisplays;
    if (r.kind == SR_MON) return g.target.kind == CaptureTarget::Monitor && g.target.mon == r.mon;
    if (r.kind == SR_WIN) return g.target.kind == CaptureTarget::Window && g.target.hwnd == r.hwnd;
    return false;
}

// ---------- dropdown model ----------
const wchar_t* kEncV2[] = {L"Auto", L"H.264 \u00b7 NVENC", L"H.264 \u00b7 AMF", L"H.264 \u00b7 Quick Sync", L"H.264 \u00b7 Software"};
int ddCount(int id) {
    switch (id) {
        case H_DD_FPS: return 3; case H_DD_QUAL: return 4; case H_DD_ENC: return 5;
        case H_DD_GPU: return 1 + (int)g.gpus.size(); case H_DD_RES: return 3; case H_DD_CONT: return 3;
        case H_DD_SYSDEV: return 1 + (int)g.outs.size(); case H_DD_MICDEV: return 1 + (int)g.mics.size();
    }
    return 0;
}
std::wstring ddItem(int id, int i) {
    switch (id) {
        case H_DD_FPS: return kFpsL[i]; case H_DD_QUAL: return kQualL[i]; case H_DD_ENC: return kEncV2[i];
        case H_DD_GPU: return i == 0 ? std::wstring(L"Auto (display's GPU)") : g.gpus[i - 1];
        case H_DD_RES: { if (i) return kResL[i]; int w, h; sourceSize(w, h); return w ? L"Native (" + dims(w, h) + L")" : std::wstring(L"Native"); }
        case H_DD_CONT: return kContL[i];
        case H_DD_SYSDEV: return i == 0 ? std::wstring(L"Default output") : g.outs[i - 1].name;
        case H_DD_MICDEV: return i == 0 ? std::wstring(L"Default microphone") : g.mics[i - 1].name;
    }
    return L"";
}
int ddSel(int id) {
    switch (id) {
        case H_DD_FPS: return g.s.fps == 15 ? 0 : g.s.fps == 30 ? 1 : 2;
        case H_DD_QUAL: return (int)g.s.quality; case H_DD_ENC: return (int)g.s.encoder;
        case H_DD_GPU: return g.s.gpuIndex < 0 || g.s.gpuIndex >= (int)g.gpus.size() ? 0 : g.s.gpuIndex + 1;
        case H_DD_RES: return (int)g.s.scale; case H_DD_CONT: return (int)g.s.container;
        case H_DD_SYSDEV: for (size_t k = 0; k < g.outs.size(); ++k) if (U(g.outs[k].id) == g.s.systemDevice) return (int)k + 1; return 0;
        case H_DD_MICDEV: for (size_t k = 0; k < g.mics.size(); ++k) if (U(g.mics[k].id) == g.s.micDevice) return (int)k + 1; return 0;
    }
    return 0;
}
void restartMeter();
void applyAudioFx() {
    auto& a = audioFx();
    a.micGain = g.s.micVolume / 100.f; a.noiseSuppression = g.s.noiseSuppression;
    a.gateDb = (float)g.s.noiseGateDb; a.pushToTalk = g.s.pushToTalk; a.monitoring = g.s.monitoring;
    a.micMuted = g.s.micMuted; a.systemMuted = g.s.systemMuted;
}
void ddSet(int id, int i) {
    switch (id) {
        case H_DD_FPS: g.s.fps = kFps[i]; break; case H_DD_QUAL: g.s.quality = kQual[i]; break;
        case H_DD_ENC: g.s.encoder = kEnc[i]; break; case H_DD_GPU: g.s.gpuIndex = i - 1; g.s.gpuName = i > 0 ? U(g.gpus[i - 1]) : std::string(); break;
        case H_DD_RES: g.s.scale = kRes[i]; break; case H_DD_CONT: g.s.container = kCont[i]; break;
        case H_DD_SYSDEV: g.s.systemDevice = i > 0 ? U(g.outs[i - 1].id) : std::string(); break;
        case H_DD_MICDEV: g.s.micDevice = i > 0 ? U(g.mics[i - 1].id) : std::string(); break;
    }
    save();
    if (id == H_DD_SYSDEV || id == H_DD_MICDEV) restartMeter();
    if (id == H_DD_FPS || id == H_DD_ENC || id == H_DD_GPU || id == H_DD_RES || id == H_DD_CONT) startWarmup();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}
// While recording: source, encoder, container and resolution (SPEC: App states), plus the audio devices/routing,
// which only apply to the next recording. Volume, noise suppression/gate, monitoring and PTT apply live.
bool locked(int id) {
    if (!uiBusy()) return false;
    switch (id) {
        case H_SRC: case H_DD_ENC: case H_DD_CONT: case H_DD_RES: case H_DD_GPU: case H_DD_SYSDEV: case H_DD_MICDEV:
        case H_TG_SYS: case H_TG_MIC: case H_CH_MONO: case H_CH_STEREO: case H_HK0: case H_HK1: case H_HK2: case H_HKPTT: case H_BROWSE:
            return true;
    }
    return false;
}

// ---------- popup (layered window: dropdown lists, source picker, mic popover) ----------
enum PopKind { PK_NONE, PK_LIST, PK_SOURCE, PK_MIC };
struct Popup {
    HWND hwnd = nullptr;
    int kind = PK_NONE, dd = H_NONE;
    RF anchor;                 // field rect, logical client coords
    float x = 0, y = 0, w = 0, h = 0;  // card rect, logical client coords
    float contentH = 0, scroll = 0;
    bool above = false, closing = false;
    Anim open, close;
    std::vector<Region> regs;
    int hover = -1, kb = -1, pressed = -1;
    std::map<int, Anim> hov;
    std::wstring search;
    std::vector<SrcRow> rows;
    bool dragVol = false;
} P;
const float kShadow = 18.f;   // logical margin around the card for the shadow
void renderPopup();
int popupIndexOf(int id) { return id >= P_BASE && id < P_BASE + 900 ? id - P_BASE : -1; }
std::vector<int> selectableRows() {
    std::vector<int> v;
    if (P.kind == PK_LIST) { for (int i = 0; i < ddCount(P.dd); ++i) v.push_back(i); }
    else if (P.kind == PK_SOURCE) { for (size_t i = 0; i < P.rows.size(); ++i) if (P.rows[i].kind <= SR_WIN) v.push_back((int)i); }
    return v;
}
std::wstring lowerW(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
void buildSourceRows() {
    P.rows.clear();
    g_displays = listDisplays();
    RECT u = virtualRect();
    float y = 6;
    auto add = [&](SrcRow r, float h, float gapAfter) { r.y = y; r.h = h; y += h + gapAfter; P.rows.push_back(r); };
    SrcRow all; all.kind = SR_ALL; all.label = L"Full screen"; all.right = L"All displays \u00b7 " + dims(u.right - u.left, u.bottom - u.top);
    add(all, 32, 2);
    for (auto& d : g_displays) {
        SrcRow m; m.kind = SR_MON; m.mon = d.mon; m.device = d.device; m.label = L"Monitor " + std::to_wstring(d.index);
        m.right = dims(d.rect.right - d.rect.left, d.rect.bottom - d.rect.top) + (d.primary ? L" \u00b7 Primary" : L"");
        add(m, 32, 2);
    }
    SrcRow dv; dv.kind = SR_DIV; add(dv, 17, 0);
    SrcRow se; se.kind = SR_SEARCH; add(se, 30, 6);
    SrcRow ca; ca.kind = SR_CAPTION; ca.label = L"WINDOWS"; add(ca, 20, 2);
    std::wstring q = lowerW(P.search);
    int n = 0;
    for (auto& w : listCaptureWindows()) {
        if (!q.empty() && lowerW(w.title).find(q) == std::wstring::npos && lowerW(w.exe).find(q) == std::wstring::npos) continue;
        SrcRow r; r.kind = SR_WIN; r.hwnd = w.hwnd; r.ic = w.icon; r.exe = w.exe; r.label = w.title;
        add(r, 34, 0); ++n;
    }
    if (!n) { SrcRow none; none.kind = SR_CAPTION; none.label = q.empty() ? L"No windows" : L"No matching windows"; add(none, 30, 0); }
    P.contentH = y + 6;
}
void layoutPopup() {
    RECT c{}; if (g.hwnd) GetClientRect(g.hwnd, &c); else c = RECT{0, 0, (LONG)(1100 * scale()), (LONG)(700 * scale())};
    const float CH = c.bottom / scale();
    if (P.kind == PK_MIC) {
        P.w = 280; P.h = 168;
        P.x = P.anchor.X + P.anchor.Width / 2 - P.w / 2; P.y = P.anchor.Y - 12 - P.h;
        P.above = true; return;
    }
    if (P.kind == PK_LIST) { P.w = P.anchor.Width; P.contentH = 8.f + ddCount(P.dd) * 32.f; }
    else { P.w = 420; buildSourceRows(); }
    P.h = std::min(440.f, P.contentH);
    P.x = P.anchor.X; P.y = P.anchor.Y + P.anchor.Height + 4;
    P.above = false;
    if (P.y + P.h > CH - 8 && P.anchor.Y - 4 - P.h > 48) { P.y = P.anchor.Y - 4 - P.h; P.above = true; }
    P.scroll = std::max(0.f, std::min(P.scroll, P.contentH - P.h));
}
void ensurePopupWindow();
void openPopup(int kind, int dd, RF anchor) {
    ensurePopupWindow();
    P.kind = kind; P.dd = dd; P.anchor = anchor; P.closing = false; P.scroll = 0; P.hover = -1; P.pressed = -1;
    P.search.clear(); P.hov.clear(); P.dragVol = false;
    layoutPopup();
    P.kb = -1;
    if (kind == PK_LIST) P.kb = ddSel(dd);
    else if (kind == PK_SOURCE) for (size_t i = 0; i < P.rows.size(); ++i) if (rowSelected(P.rows[i])) P.kb = (int)i;
    P.open.set(0); P.open.go(1, 160, E_OUTEXPO);
    if (!P.open.ms) P.open.set(1);
    renderPopup();
    if (P.hwnd) ShowWindow(P.hwnd, SW_SHOWNOACTIVATE);
    kickAnim();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}
void closePopup(bool animated = true) {
    if (P.kind == PK_NONE || P.closing) return;
    if (P.dragVol) { P.dragVol = false; ReleaseCapture(); save(); }
    P.closing = true;
    P.close.set(0);
    if (animated) P.close.go(1, 110, E_IN);
    if (!P.close.active()) { if (P.hwnd) ShowWindow(P.hwnd, SW_HIDE); P.kind = PK_NONE; P.closing = false; }
    else kickAnim();
    if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE);
}
void popupActivate(int idx) {
    if (P.kind == PK_LIST) { int dd = P.dd; closePopup(); if (idx >= 0 && idx < ddCount(dd)) ddSet(dd, idx); }
    else if (P.kind == PK_SOURCE) {
        if (idx < 0 || idx >= (int)P.rows.size() || P.rows[idx].kind > SR_WIN) return;
        SrcRow r = P.rows[idx];
        closePopup();
        selectSource(r);
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}
void popupType(const std::wstring& s) {  // search box: typing goes here while the source list is open
    if (P.kind != PK_SOURCE) return;
    P.search = s;
    float keepH = P.h;
    buildSourceRows();
    P.h = keepH;  // the card doesn't jump while filtering
    P.scroll = 0;
    auto sel = selectableRows();
    P.kb = -1;
    for (int i : sel) if (P.rows[i].kind == SR_WIN) { P.kb = i; break; }
    renderPopup();
}
void popupKey(UINT vk) {
    if (P.kind == PK_LIST || P.kind == PK_SOURCE) {
        auto sel = selectableRows();
        if (sel.empty()) return;
        int pos = -1;
        for (size_t i = 0; i < sel.size(); ++i) if (sel[i] == P.kb) pos = (int)i;
        if (vk == VK_DOWN) pos = pos < 0 ? 0 : std::min((int)sel.size() - 1, pos + 1);
        else if (vk == VK_UP) pos = pos < 0 ? 0 : std::max(0, pos - 1);
        else if (vk == VK_RETURN) { if (pos >= 0) popupActivate(sel[pos]); return; }
        else return;
        P.kb = sel[pos];
        // keep it visible
        float ry = 0, rh = 32;
        if (P.kind == PK_LIST) ry = 4 + P.kb * 32.f; else { ry = P.rows[P.kb].y; rh = P.rows[P.kb].h; }
        if (ry < P.scroll) P.scroll = ry - 4;
        if (ry + rh > P.scroll + P.h) P.scroll = ry + rh - P.h + 4;
        P.scroll = std::max(0.f, std::min(P.scroll, P.contentH - P.h));
        renderPopup();
    }
}
void setMicVolume(int v, bool persist) {
    v = std::max(0, std::min(200, v));
    g.s.micVolume = v; applyAudioFx();
    if (persist) save();
    if (P.kind == PK_MIC) renderPopup();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}
void toggleMic();
void popupClick(int id, float x) {
    if (id == P_MUTE) { toggleMic(); renderPopup(); return; }
    if (id == P_SL_VOL) return;  // handled as a drag
    int idx = popupIndexOf(id);
    if (idx >= 0) popupActivate(idx);
    (void)x;
}
// ---------- widgets ----------
void focusRing(gp::Graphics& gr, RF r, float rad, int id) {
    if (g_kbUsed && g_focus == id) strokeRR(gr, inflate(r, 3), rad + 3, C(T_TEXT2, 200), 1.5f);
}
float chevronAngle(int id) {
    if (P.kind == PK_NONE || P.dd != id) return 0;
    return P.closing ? 180.f * (1.f - (float)ease(E_IN, P.close.progress())) : 180.f * P.open.value();
}
bool popupOpenFor(int id) { return P.kind != PK_NONE && !P.closing && P.dd == id; }
void ddField(gp::Graphics& gr, RF r, int id, const std::wstring& t, int glyph = -1, HICON wicon = nullptr) {
    bool dis = locked(id), open = popupOpenFor(id);
    float hv = dis ? 0.f : hovA(id);
    fillRR(gr, r, 6, mix(T_INPUT, T_INPUT_H, open ? 1.f : hv));
    strokeRR(gr, r, 6, open || (g_kbUsed && g_focus == id) ? C(T_ACC) : WA(A_DEFAULT));
    float x = r.X + 12, cy = r.Y + r.Height / 2;
    if (wicon) { drawWinIcon(gr, wicon, x, cy - 8, 16); x += 26; }
    else if (glyph >= 0) { icon(gr, glyph, x, cy - 8, 16, C(dis ? T_DIS : T_TEXT2)); x += 26; }
    txt(gr, t, RF(x, r.Y, r.X + r.Width - 36 - x, r.Height), F.body, C(dis ? T_DIS : T_TEXT));
    icon(gr, G_CHEVRON, r.X + r.Width - 28, cy - 8, 16, C(dis ? T_DIS : T_TEXT2), chevronAngle(id));
    addReg(r, id);
}
void toggleW(gp::Graphics& gr, float x, float cy, int id, bool on) {
    RF r(x, cy - 10, 36, 20);
    auto it = g_tog.find(id);
    if (it == g_tog.end()) { g_tog[id].set(on ? 1.f : 0.f); it = g_tog.find(id); }
    else if (it->second.to != (on ? 1.f : 0.f)) it->second.go(on ? 1.f : 0.f, 140, E_STD);
    float k = it->second.value();
    bool dis = locked(id);
    fillRR(gr, r, 10, mix(T_ELEV, T_ACC, k));
    if (k < 0.99f) strokeRR(gr, r, 10, WA((int)(A_STRONG * (1 - k))));
    fillCircle(gr, x + 10 + 16 * k, cy, 7, dis ? C(T_TEXT2) : C(0xFFFFFF));
    if (hovA(id) > 0 && !dis) fillRR(gr, r, 10, WA((int)(A_HOVER * hovA(id))));
    focusRing(gr, r, 10, id);
    addReg(inflate(r, 4), id);
}
void sliderW(gp::Graphics& gr, float x, float cy, float w, int id, float frac, bool dis = false) {
    frac = std::max(0.f, std::min(1.f, frac));
    fillRR(gr, RF(x, cy - 2, w, 4), 2, C(T_TRACK));
    fillRR(gr, RF(x, cy - 2, w * frac, 4), 2, C(dis ? T_DIS : T_ACC));
    float kx = x + w * frac;
    float hv = std::max(hovA(id), g_drag == id || (P.dragVol && id == P_SL_VOL) ? 1.f : 0.f);
    if (hv > 0) fillCircle(gr, kx, cy, 12, WA((int)(A_SUBTLE * hv)));
    fillCircle(gr, kx, cy, 7, C(dis ? T_TEXT2 : 0xFFFFFF));
    if (g_kbUsed && g_focus == id) { gp::Pen p(C(T_TEXT2, 200), 1.5f); gr.DrawEllipse(&p, kx - 10, cy - 10, 20.f, 20.f); }
}
// Level meter: 4 px segments, 2 px gap (tokens meterSegW/Gap); green, the top 15 % amber.
void meterW(gp::Graphics& gr, float x, float y, float w, float h, float level, bool on) {
    int n = (int)((w + 2) / 6);
    int lit = on ? (int)std::lround(level * n) : 0;
    for (int i = 0; i < n; ++i) {
        gp::Color c = i < lit ? (i >= (int)(n * 0.85f) ? C(T_WARN) : C(T_OK)) : WA(A_SUBTLE);
        gp::SolidBrush b(c);
        gr.FillRectangle(&b, x + i * 6.f, y, 4.f, h);
    }
    g_meterRects.push_back(RF(x - 1, y - 1, w + 2, h + 2));
}
void buttonBg(gp::Graphics& gr, RF r, float rad, int id, unsigned bg, gp::Color border, bool dis = false) {
    fillRR(gr, r, rad, C(bg));
    if (!dis) {
        float hv = hovA(id);
        if (hv > 0) fillRR(gr, r, rad, WA((int)(A_HOVER * hv)));
        if (g.pressed == id && g.hover == id) fillRR(gr, r, rad, WA(A_PRESSED));
    }
    strokeRR(gr, r, rad, border);
    focusRing(gr, r, rad, id);
}

// ---------- title bar ----------
RF tbBtn(float W, int k) { return RF(W - 138 + 46.f * k, 0, 46, 40); }  // 0 min, 1 max, 2 close
void paintTitleBar(gp::Graphics& gr, float W) {
    gp::SolidBrush bg(C(T_BG)); gr.FillRectangle(&bg, 0.f, 0.f, W, 40.f);
    if (g.icoSmall) {  // app icon (dark tile, white C, red dot): the .ico, via GDI
        gp::Bitmap* b = nullptr;
        static gp::Bitmap* appIco = nullptr;
        if (!appIco) {
            HICON big = (HICON)LoadImageW(g.inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 32, 32, 0);
            appIco = winIcon(big ? big : g.icoSmall);
        }
        b = appIco;
        if (b) { gr.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic); gr.DrawImage(b, RF(14, 12, 16, 16)); }
    }
    txt(gr, L"Cast Recorder", RF(40, 0, 104, 40), F.title, C(T_TEXT), AL_L, false);
    const wchar_t* tabs[2] = {L"Record", L"Settings"};
    const float tx[2] = {150, 222}, tw[2] = {68, 80};
    for (int i = 0; i < 2; ++i) {
        RF r(tx[i], 7, tw[i], 26);
        int id = i ? H_TAB_SET : H_TAB_REC;
        bool act = g_viewIdx == i;
        if (act) fillRR(gr, r, 6, C(T_ELEV));
        else if (hovA(id) > 0) fillRR(gr, r, 6, WA((int)(A_HOVER * hovA(id))));
        txt(gr, tabs[i], r, act ? F.btn : F.body, C(act ? T_TEXT : (hovA(id) > 0.5f ? T_TEXT2 : T_MUTED)), AL_C, false);
        focusRing(gr, r, 6, id);
        addReg(r, id);
    }
    const int ids[3] = {H_MIN, H_MAX, H_CLOSE};
    const bool zoomed = g.hwnd && IsZoomed(g.hwnd);
    const int glyphs[3] = {G_MINIMIZE, zoomed ? G_RESTORE : G_MAXIMIZE, G_CLOSE};
    for (int k = 0; k < 3; ++k) {
        RF r = tbBtn(W, k);
        float hv = hovA(ids[k]);
        if (hv > 0) { gp::SolidBrush hb(k == 2 ? C(T_CLOSEH, (int)(255 * hv)) : WA((int)(A_SUBTLE * hv))); gr.FillRectangle(&hb, r); }
        icon(gr, glyphs[k], r.X + 15, 12, 16, k == 2 && hv > 0.5f ? C(0xFFFFFF) : C(T_TEXT));
        addReg(r, ids[k]);
    }
}

// ---------- Record view ----------
struct RecLayout { RF src, fps, qual, preview, surf, mic, micIcon, sys, pause, record, status; };
RecLayout recLayout(float W, float H) {
    RecLayout L;
    L.src = RF(20, 56, 360, 36);
    L.qual = RF(W - 20 - 110, 56, 110, 36);
    L.fps = RF(L.qual.X - 8 - 104, 56, 104, 36);
    L.preview = RF(20, 104, W - 40, H - 204);
    RF in(L.preview.X + 12, L.preview.Y + 12, L.preview.Width - 24, L.preview.Height - 24);
    float sw = in.Width, sh = sw * 9 / 16;
    if (sh > in.Height) { sh = in.Height; sw = sh * 16 / 9; }
    L.surf = RF(std::floor(in.X + (in.Width - sw) / 2), std::floor(in.Y + (in.Height - sh) / 2), std::floor(sw), std::floor(sh));
    float cy = H - 52;
    L.mic = RF(std::floor(W / 2 - 130), cy - 20, 136, 40);
    L.micIcon = RF(L.mic.X, L.mic.Y, 34, 40);
    L.sys = RF(L.mic.X + 148, cy - 20, 136, 40);
    L.record = RF(W - 20 - 122, cy - 22, 122, 44);
    L.pause = RF(L.record.X - 10 - 44, cy - 22, 44, 44);
    L.status = RF(20, cy - 14, L.mic.X - 40, 28);
    return L;
}
RECT toPx(RF r) { float s = scale(); return RECT{(LONG)std::floor(r.X * s), (LONG)std::floor(r.Y * s), (LONG)std::ceil((r.X + r.Width) * s), (LONG)std::ceil((r.Y + r.Height) * s)}; }
float clientW() { RECT c{}; if (g.hwnd) GetClientRect(g.hwnd, &c); else c.right = (LONG)(1100 * scale()); return c.right / scale(); }
float clientH() { RECT c{}; if (g.hwnd) GetClientRect(g.hwnd, &c); else c.bottom = (LONG)(700 * scale()); return c.bottom / scale(); }
RECT previewRectNow() { return toPx(recLayout(clientW(), clientH()).surf); }

void paintLiveChip(gp::Graphics& gr, float x, float y) {
    std::wstring t = L"Live preview";
    float w = txtW(gr, t, F.caption) + 30;
    fillRR(gr, RF(x, y, w, 22), 11, C(T_PREV, 235));
    strokeRR(gr, RF(x, y, w, 22), 11, WA(A_SUBTLE));
    fillCircle(gr, x + 12, y + 11, 3.5f, C(g.gprev.live() || g.prevLive || g_renderOnly ? T_OK : T_MUTED));
    txt(gr, t, RF(x + 21, y, w - 24, 22), F.caption, C(T_TEXT2), AL_L, false);
}
std::wstring sizeFpsText() { int w, h; sourceSize(w, h); return (w ? dims(w, h) + L" \u00b7 " : L"") + std::to_wstring(g.s.fps) + L" fps"; }
void paintSizeChip(gp::Graphics& gr, RF r) {  // right-aligned muted text with a soft backing for legibility on live content
    std::wstring t = sizeFpsText();
    float w = txtW(gr, t, F.caption) + 16;
    fillRR(gr, RF(r.X + r.Width - w, r.Y, w, 22), 11, gp::Color(110, 10, 12, 15));
    txt(gr, t, RF(r.X + r.Width - w, r.Y, w - 8, 22), F.caption, C(T_TEXT2), AL_R, false);
}
void paintPreviewFallback(gp::Graphics& gr, RF s) {
    fillRR(gr, s, 4, C(0x1A1F27));
    if (g.prevBmp) {
        gp::Bitmap b(g.prevBmp, nullptr);
        gr.SetInterpolationMode(gp::InterpolationModeHighQualityBilinear);
        float bw = (float)b.GetWidth(), bh = (float)b.GetHeight();
        float k = std::min(s.Width / bw, s.Height / bh);
        gr.DrawImage(&b, RF(s.X + (s.Width - bw * k) / 2, s.Y + (s.Height - bh * k) / 2, bw * k, bh * k));
    } else {
        gp::SolidBrush gb(WA(0x08));
        for (float x = s.X + 24; x < s.X + s.Width; x += 24) gr.FillRectangle(&gb, x, s.Y, 1.f, s.Height);
        for (float y = s.Y + 24; y < s.Y + s.Height; y += 24) gr.FillRectangle(&gb, s.X, y, s.Width, 1.f);
        txt(gr, L"Waiting for capture\u2026", s, F.body, C(T_MUTED), AL_C);
    }
}
std::wstring timerText() {
    int sec = g_fakeRec ? 83 : (int)g.rec.elapsedSeconds();
    wchar_t tm[32]; swprintf(tm, 32, L"%02d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    return tm;
}
void paintRecordView(gp::Graphics& gr, float W, float H) {
    RecLayout L = recLayout(W, H);
    bool active = uiActive(), paused = uiPaused();
    RecState st = g.rec.state();
    bool finishing = !g_fakeRec && (st == RecState::Stopping || st == RecState::Finishing);
    // toolbar
    int glyph = g.target.kind == CaptureTarget::AllDisplays ? G_FULLSCREEN : g.target.kind == CaptureTarget::Monitor ? G_MONITOR : G_WINDOW;
    ddField(gr, L.src, H_SRC, sourceText(), glyph, g.target.kind == CaptureTarget::Window ? g_srcIcon : nullptr);
    focusRing(gr, L.src, 6, H_SRC);
    ddField(gr, L.fps, H_DD_FPS, ddItem(H_DD_FPS, ddSel(H_DD_FPS)));
    focusRing(gr, L.fps, 6, H_DD_FPS);
    ddField(gr, L.qual, H_DD_QUAL, ddItem(H_DD_QUAL, ddSel(H_DD_QUAL)));
    focusRing(gr, L.qual, 6, H_DD_QUAL);
    // preview
    card(gr, L.preview, 10, C(T_PREV), WA(A_SUBTLE));
    if (!g.gprev.live()) paintPreviewFallback(gr, L.surf);
    else { gp::SolidBrush b(C(0x000000)); gr.FillRectangle(&b, L.surf); }
    if (active) strokeRR(gr, L.preview, 10, C(T_ACC), 2.f);
    paintLiveChip(gr, L.surf.X + 12, L.surf.Y + 12);
    paintSizeChip(gr, RF(L.surf.X + 12, L.surf.Y + 12, L.surf.Width - 24, 22));
    // status (left)
    {
        RF s = L.status;
        unsigned dotc, tc; std::wstring t;
        if (active) { dotc = paused ? T_WARN : T_ACC; tc = paused ? T_WARN : T_TEXT; t = (paused ? L"Paused  " : L"Recording  ") + timerText(); }
        else if (finishing) { dotc = T_MUTED; tc = T_TEXT2; t = L"Finishing file\u2026"; }
        else {
            unsigned sc = fromRef(g.statusColor);
            t = g.status.empty() ? L"Ready" : g.status;
            bool err = sc == T_ERR;
            dotc = err ? T_ERR : sc == T_WARN ? T_WARN : T_OK; tc = err ? T_ERR : sc == T_WARN ? T_WARN : (t == L"Ready" ? T_TEXT2 : sc);
        }
        fillCircle(gr, s.X + 4, s.Y + s.Height / 2, 4, C(dotc));
        float avail = s.Width - 16;
        bool err = !active && !finishing && fromRef(g.statusColor) == T_ERR;
        float linkW = err ? txtW(gr, L"Open log", F.status) + 14 : 0;
        float tw = std::min(txtW(gr, t, F.status) + 2, avail - linkW);
        txt(gr, t, RF(s.X + 16, s.Y, tw, s.Height), F.status, C(tc));
        if (err) {
            RF lr(s.X + 16 + tw + 10, s.Y + 4, linkW - 10, s.Height - 8);
            txt(gr, L"Open log", lr, F.status, C(hovA(H_STATUSLOG) > 0.5f ? T_TEXT : T_ACC_B), AL_L, false);
            hline(gr, lr.X, lr.X + lr.Width, lr.Y + lr.Height - 2, C(T_ACC_B, 160));
            addReg(lr, H_STATUSLOG);
        }
        g_statusRect = s;
    }
    // audio chips
    {
        bool micOn = g.s.micEnabled, muted = g.s.micMuted;
        bool open = P.kind == PK_MIC && !P.closing;
        RF m = L.mic;
        fillRR(gr, m, 8, mix(T_SURF, T_INPUT_H, hovA(H_MICCHIP) * 0.8f));
        strokeRR(gr, m, 8, open ? C(T_ACC) : WA(A_DEFAULT));
        if (hovA(H_MICICON) > 0) fillRR(gr, RF(m.X + 4, m.Y + 4, 28, 32), 6, WA((int)(A_PRESSED * hovA(H_MICICON))));
        icon(gr, muted ? G_MICMUTED : G_MIC, m.X + 10, m.Y + 12, 16, C(!micOn ? T_DIS : muted ? T_ACC_B : T_TEXT2));
        meterW(gr, m.X + 38, m.Y + 17, m.Width - 38 - 14, 6, g_meter[0], micOn && !muted);
        focusRing(gr, m, 8, H_MICCHIP);
        addReg(m, H_MICCHIP);
        addReg(L.micIcon, H_MICICON);
        RF s = L.sys;
        bool sysOn = g.s.systemAudio, smuted = g.s.systemMuted;
        fillRR(gr, s, 8, mix(T_SURF, T_INPUT_H, hovA(H_SYSCHIP) * 0.8f));
        strokeRR(gr, s, 8, WA(A_DEFAULT));
        icon(gr, smuted ? G_SYSMUTED : G_SYS, s.X + 10, s.Y + 12, 16, C(!sysOn ? T_DIS : smuted ? T_ACC_B : T_TEXT2));
        meterW(gr, s.X + 38, s.Y + 17, s.Width - 38 - 14, 6, g_meter[1], sysOn && !smuted);
        focusRing(gr, s, 8, H_SYSCHIP);
        addReg(s, H_SYSCHIP);
    }
    // Pause (disabled when idle; Paused shows the record glyph = resume)
    {
        bool dis = !active;
        buttonBg(gr, L.pause, 10, H_PAUSE, T_ELEV, WA(A_DEFAULT), dis);
        icon(gr, paused ? G_RECORD : G_PAUSE, L.pause.X + 14, L.pause.Y + 14, 16, C(dis ? T_DIS : paused ? T_ACC_B : T_TEXT));
        addReg(L.pause, H_PAUSE);
    }
    // Record / Stop with the pulse (1200 ms loop, glow alpha .15 <-> .45)
    {
        RF r = L.record;
        if (active || finishing) {
            if (active && !paused) {
                double ph = fmod(nowMs(), 1200.0) / 1200.0;
                double tri = ph < 0.5 ? ph * 2 : 2 - ph * 2;
                float a = 0.15f + 0.30f * (float)ease(E_INOUT, tri);
                if (!animationsOn()) a = 0.3f;
                for (int i = 6; i >= 1; --i) strokeRR(gr, inflate(r, (float)i), 10.f + i, C(T_ACC, (int)(255 * a * (7 - i) / 14.f)), 2.f);
            }
            buttonBg(gr, r, 10, H_RECORD, T_ELEV, finishing ? WA(A_DEFAULT) : C(T_ACC));
            if (!finishing) strokeRR(gr, r, 10, C(T_ACC), 1.5f);
            std::wstring t = finishing ? L"Saving" : L"Stop";
            float tw = txtW(gr, t, F.btnP);
            float x0 = r.X + (r.Width - (16 + 8 + tw)) / 2;
            icon(gr, G_STOP, x0, r.Y + 14, 16, C(finishing ? T_DIS : T_ACC));
            txt(gr, t, RF(x0 + 24, r.Y, tw + 4, r.Height), F.btnP, C(finishing ? T_TEXT2 : T_TEXT), AL_L, false);
        } else {
            float hv = hovA(H_RECORD);
            bool pr = g.pressed == H_RECORD && g.hover == H_RECORD;
            gp::Color bg = pr ? C(T_ACC_P) : mix(T_ACC, T_ACC_H, hv);
            fillRR(gr, r, 10, bg);
            std::wstring t = L"Record";
            float tw = txtW(gr, t, F.btnP);
            float x0 = r.X + (r.Width - (16 + 8 + tw)) / 2;
            icon(gr, G_RECORD, x0, r.Y + 14, 16, C(0xFFFFFF));
            txt(gr, t, RF(x0 + 24, r.Y, tw + 4, r.Height), F.btnP, C(0xFFFFFF), AL_L, false);
            focusRing(gr, r, 10, H_RECORD);
        }
        addReg(r, H_RECORD);
        g_recordRect = inflate(r, 8);
    }
}

// ---------- Settings view ----------
void cardTitle(gp::Graphics& gr, RF c, const wchar_t* t) { txt(gr, t, RF(c.X + 20, c.Y + 14, c.Width - 40, 18), F.section, C(T_MUTED), AL_L, false); }
void label(gp::Graphics& gr, const wchar_t* t, float x, float cy, bool dis = false) { txt(gr, t, RF(x, cy - 12, 180, 24), F.label, C(dis ? T_DIS : T_TEXT2), AL_L, false); }
void hotkeyField(gp::Graphics& gr, RF r, int id, int idx, const Hotkey& hk, bool dimmed = false) {
    bool cap = g.capturing == idx, dis = locked(id);
    fillRR(gr, r, 6, mix(T_INPUT, T_INPUT_H, cap ? 1.f : dis ? 0.f : hovA(id)));
    COLORREF mc = idx < 3 ? g.hkMsgColor[idx] : 0;
    bool bad = idx < 3 && fromRef(mc) == T_ERR && !cap;
    strokeRR(gr, r, 6, cap ? C(T_ACC) : bad ? C(T_ERR) : WA(A_DEFAULT));
    std::wstring t; unsigned tc = T_TEXT;
    if (cap) { t = g.liveMods ? modsText(g.liveMods) + L"\u2026" : L"Press a key\u2026"; tc = T_MUTED; }
    else if (hk.empty()) { t = L"Not set"; tc = T_MUTED; }
    else t = hkText(hk);
    if ((dis || dimmed) && !cap) tc = T_DIS;
    txt(gr, t, r, F.hotkey, C(tc), AL_C);
    if (bad && idx < 3 && !g.hkMsg[idx].empty()) { /* the reason shows as the field's tooltip-like suffix */ }
    focusRing(gr, r, 6, id);
    addReg(r, id);
}
void segmented(gp::Graphics& gr, float x, float cy) {
    RF r(x, cy - 16, 160, 32);
    fillRR(gr, r, 7, C(T_BG));
    strokeRR(gr, r, 7, WA(A_DEFAULT));
    float target = g.s.channels == 2 ? 1.f : 0.f;
    if (g_chan.to != target) g_chan.go(target, 160, E_STD);
    float k = g_chan.value();
    RF th(x + 3 + k * 77, cy - 13, 77, 26);
    fillRR(gr, th, 5, C(T_ELEV));
    strokeRR(gr, th, 5, WA(A_SUBTLE));
    bool dis = locked(H_CH_MONO);
    txt(gr, L"Mono", RF(x + 3, cy - 13, 77, 26), g.s.channels == 1 ? F.btn : F.body, C(dis ? T_DIS : g.s.channels == 1 ? T_TEXT : T_TEXT2), AL_C, false);
    txt(gr, L"Stereo", RF(x + 80, cy - 13, 77, 26), g.s.channels == 2 ? F.btn : F.body, C(dis ? T_DIS : g.s.channels == 2 ? T_TEXT : T_TEXT2), AL_C, false);
    addReg(RF(x, cy - 16, 80, 32), H_CH_MONO);
    addReg(RF(x + 80, cy - 16, 80, 32), H_CH_STEREO);
    focusRing(gr, r, 7, H_CH_MONO);
}
void paintSettingsView(gp::Graphics& gr, float W, float H) {
    (void)W; (void)H;
    RF video(20, 60, 520, 244), hot(20, 316, 520, 170), out(20, 498, 520, 102), audio(560, 60, 520, 540);
    for (RF c : {video, hot, out, audio}) card(gr, c, 10, C(T_SURF), WA(A_SUBTLE));
    // Video
    cardTitle(gr, video, L"VIDEO");
    const wchar_t* vl[4] = {L"Encoder", L"GPU", L"Resolution", L"Container"};
    const int vid[4] = {H_DD_ENC, H_DD_GPU, H_DD_RES, H_DD_CONT};
    for (int i = 0; i < 4; ++i) {
        float top = 110.f + 46 * i;
        label(gr, vl[i], 40, top + 16, locked(vid[i]));
        RF f(180, top, 340, 32);
        ddField(gr, f, vid[i], ddItem(vid[i], ddSel(vid[i])));
        focusRing(gr, f, 6, vid[i]);
    }
    // Hotkeys
    cardTitle(gr, hot, L"HOTKEYS");
    const wchar_t* hl[3] = {L"Start / Stop", L"Pause", L"Mic mute"};
    const Hotkey* hks[3] = {&g.s.toggleRecord, &g.s.pauseToggle, &g.s.micMute};
    for (int i = 0; i < 3; ++i) {
        float top = 362.f + 40 * i;
        label(gr, hl[i], 40, top + 16);
        hotkeyField(gr, RF(180, top, 340, 32), H_HK0 + i, i, *hks[i]);
    }
    // Output
    cardTitle(gr, out, L"OUTPUT");
    {
        RF pf(40, 548, 282, 32);
        fillRR(gr, pf, 6, mix(T_INPUT, T_INPUT_H, hovA(H_PATH)));
        strokeRR(gr, pf, 6, WA(A_DEFAULT));
        RF fi(pf.X + 4, pf.Y + 4, 28, 24);
        if (hovA(H_FOLDERICON) > 0) fillRR(gr, fi, 4, WA((int)(A_PRESSED * hovA(H_FOLDERICON))));
        icon(gr, G_FOLDER, pf.X + 10, pf.Y + 8, 16, C(T_TEXT2));
        txt(gr, outputDir(), RF(pf.X + 36, pf.Y, pf.Width - 44, pf.Height), F.label, C(T_TEXT), AL_L, true, true);
        focusRing(gr, pf, 6, H_PATH);
        addReg(pf, H_PATH);
        addReg(fi, H_FOLDERICON);
        RF br(330, 548, 84, 32), lg(422, 548, 98, 32);
        bool bdis = locked(H_BROWSE);
        buttonBg(gr, br, 6, H_BROWSE, T_INPUT, WA(A_DEFAULT), bdis);
        txt(gr, L"Browse", br, F.body, C(bdis ? T_DIS : T_TEXT), AL_C, false);
        addReg(br, H_BROWSE);
        buttonBg(gr, lg, 6, H_LOG, T_INPUT, WA(A_DEFAULT));
        float lw = txtW(gr, L"Log", F.body);
        float lx = lg.X + (lg.Width - (16 + 8 + lw)) / 2;
        icon(gr, G_LOG, lx, lg.Y + 8, 16, C(T_TEXT2));
        txt(gr, L"Log", RF(lx + 24, lg.Y, lw + 4, lg.Height), F.body, C(T_TEXT), AL_L, false);
        addReg(lg, H_LOG);
    }
    // Audio (labels x+20, controls x+212)
    cardTitle(gr, audio, L"AUDIO");
    const float lx = 580, cx = 772;
    label(gr, L"System audio", lx, 126);
    toggleW(gr, cx - 52, 126, H_TG_SYS, g.s.systemAudio);
    ddField(gr, RF(cx, 110, 288, 32), H_DD_SYSDEV, ddItem(H_DD_SYSDEV, ddSel(H_DD_SYSDEV)), g.s.systemMuted ? G_SYSMUTED : G_SYS);
    focusRing(gr, RF(cx, 110, 288, 32), 6, H_DD_SYSDEV);
    meterW(gr, cx, 150, 288, 4, g_meter[1], g.s.systemAudio && !g.s.systemMuted);
    hline(gr, lx, 1060, 176, WA(A_SUBTLE));
    label(gr, L"Microphone", lx, 210);
    toggleW(gr, cx - 52, 210, H_TG_MIC, g.s.micEnabled);
    ddField(gr, RF(cx, 194, 288, 32), H_DD_MICDEV, ddItem(H_DD_MICDEV, ddSel(H_DD_MICDEV)), g.s.micMuted ? G_MICMUTED : G_MIC);
    focusRing(gr, RF(cx, 194, 288, 32), 6, H_DD_MICDEV);
    meterW(gr, cx, 234, 288, 4, g_meter[0], g.s.micEnabled && !g.s.micMuted);
    float y = 273;
    label(gr, L"Volume", lx, y);
    sliderW(gr, cx, y, 240, H_SL_VOL, g.s.micVolume / 200.f);
    addReg(RF(cx - 10, y - 14, 260, 28), H_SL_VOL);
    txt(gr, std::to_wstring(g.s.micVolume) + L"%", RF(cx + 256, y - 12, 50, 24), F.label, C(T_TEXT2), AL_L, false);
    y += 48;
    label(gr, L"Noise suppression", lx, y);
    toggleW(gr, cx, y, H_TG_NS, g.s.noiseSuppression);
    y += 48;
    label(gr, L"Noise gate", lx, y);
    sliderW(gr, cx, y, 240, H_SL_GATE, (g.s.noiseGateDb + 80) / 80.f);
    addReg(RF(cx - 10, y - 14, 260, 28), H_SL_GATE);
    txt(gr, g.s.noiseGateDb <= -80 ? std::wstring(L"Off") : std::to_wstring(g.s.noiseGateDb) + L" dB", RF(cx + 256, y - 12, 56, 24), F.label, C(T_TEXT2), AL_L, false);
    y += 48;
    label(gr, L"Monitoring", lx, y);
    toggleW(gr, cx, y, H_TG_MON, g.s.monitoring);
    y += 48;
    label(gr, L"Push-to-talk", lx, y);
    toggleW(gr, cx, y, H_TG_PTT, g.s.pushToTalk);
    hotkeyField(gr, RF(cx + 52, y - 16, 140, 32), H_HKPTT, 3, g.s.pttKey, !g.s.pushToTalk);
    y += 48;
    label(gr, L"Channels", lx, y);
    segmented(gr, cx, y);
    txt(gr, L"Changes save automatically.", RF(20, 664 - 9 + (H - 700), 400, 18), F.caption, C(T_MUTED), AL_L, false);
}

// ---------- whole window ----------
void paintView(gp::Graphics& gr, int view, float W, float H) {
    if (view == 0) paintRecordView(gr, W, H); else paintSettingsView(gr, W, H);
}
void setupGraphics(gp::Graphics& gr, bool clearType) {
    gr.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    gr.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    gr.SetTextRenderingHint(clearType ? gp::TextRenderingHintClearTypeGridFit : gp::TextRenderingHintAntiAliasGridFit);
    gr.SetCompositingQuality(gp::CompositingQualityHighSpeed);
}
void paintAll(gp::Graphics& gr, float W, float H) {
    g_regs.clear(); g_meterRects.clear();
    gp::SolidBrush bg(C(T_BG)); gr.FillRectangle(&bg, -1.f, -1.f, W + 2, H + 2);
    paintTitleBar(gr, W);
    if (g_view.active()) {  // view switch: content fades in, translateX 8 -> 0
        float e = g_view.value();
        float s = scale();
        int pw = (int)std::ceil(W * s), ph = (int)std::ceil(H * s);
        gp::Bitmap off(pw, ph, PixelFormat32bppPARGB);
        {
            gp::Graphics og(&off); setupGraphics(og, false);
            og.ScaleTransform(s, s);
            gp::SolidBrush b2(C(T_BG)); og.FillRectangle(&b2, 0.f, 40.f, W, H);
            paintView(og, g_viewIdx, W, H);
        }
        gp::ColorMatrix m = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, e, 0}, {0, 0, 0, 0, 1}}};
        gp::ImageAttributes ia; ia.SetColorMatrix(&m);
        float dx = 8 * (1 - e);
        gr.DrawImage(&off, RF(dx, 40, W, H - 40), 0, 40 * s, (gp::REAL)pw, (gp::REAL)(ph - 40 * s), gp::UnitPixel, &ia);
    } else {
        paintView(gr, g_viewIdx, W, H);
    }
}
// ---------- popup painting ----------
float popHov(int id) { auto it = P.hov.find(id); return it == P.hov.end() ? 0.f : it->second.value(); }
void popReg(RF r, int id) { P.regs.push_back({r, id}); }
void paintListRow(gp::Graphics& gr, RF r, int id, bool sel, bool kb) {
    float hv = std::max(popHov(id), kb && g_kbUsed ? 1.f : 0.f);
    if (sel) fillRR(gr, r, 6, C(T_ACC, 0x24));
    else if (hv > 0) fillRR(gr, r, 6, C(T_LISTH, (int)(255 * hv)));
    if (kb && !sel && hv <= 0) fillRR(gr, r, 6, C(T_LISTH));
    if (sel) icon(gr, G_CHECK, r.X + r.Width - 28, r.Y + r.Height / 2 - 8, 16, C(T_ACC_B));
}
void paintPopupContent(gp::Graphics& gr) {
    P.regs.clear();
    const float w = P.w, h = P.h;
    if (P.kind == PK_LIST) {
        int n = ddCount(P.dd), sel = ddSel(P.dd);
        for (int i = 0; i < n; ++i) {
            RF r(4, 4 + i * 32.f - P.scroll, w - 8, 32);
            if (r.Y + r.Height < 0 || r.Y > h) continue;
            paintListRow(gr, r, P_BASE + i, i == sel, P.kb == i);
            txt(gr, ddItem(P.dd, i), RF(r.X + 10, r.Y, r.Width - 46, r.Height), F.body, C(T_TEXT));
            popReg(r, P_BASE + i);
        }
    } else if (P.kind == PK_SOURCE) {
        for (size_t i = 0; i < P.rows.size(); ++i) {
            const SrcRow& s = P.rows[i];
            RF r(6, s.y - P.scroll, w - 12, s.h);
            if (r.Y + r.Height < 0 || r.Y > h) continue;
            int id = P_BASE + (int)i;
            switch (s.kind) {
                case SR_ALL: case SR_MON: case SR_WIN: {
                    bool sel = rowSelected(s);
                    paintListRow(gr, r, id, sel, P.kb == (int)i);
                    if (s.kind == SR_WIN) drawWinIcon(gr, s.ic, r.X + 10, r.Y + r.Height / 2 - 8, 16);
                    else icon(gr, s.kind == SR_ALL ? G_FULLSCREEN : G_MONITOR, r.X + 10, r.Y + r.Height / 2 - 8, 16, C(T_TEXT2));
                    float rw = s.right.empty() ? 0 : txtW(gr, s.right, F.label) + 4;
                    float rx = r.X + r.Width - (sel ? 36 : 12) - rw;
                    txt(gr, s.label, RF(r.X + 36, r.Y, rx - r.X - 44, r.Height), F.body, C(T_TEXT));
                    if (rw > 0) txt(gr, s.right, RF(rx, r.Y, rw, r.Height), F.label, C(T_MUTED), AL_R, false);
                    popReg(r, id);
                    break;
                }
                case SR_DIV: hline(gr, 12, w - 12, r.Y + 8, WA(A_SUBTLE)); break;
                case SR_SEARCH: {
                    RF sb(8, r.Y, w - 16, 30);
                    fillRR(gr, sb, 6, C(T_INPUT));
                    strokeRR(gr, sb, 6, P.search.empty() ? WA(A_DEFAULT) : C(T_ACC));
                    if (P.search.empty()) txt(gr, L"Search windows\u2026", RF(sb.X + 12, sb.Y, sb.Width - 24, sb.Height), F.label, C(T_MUTED));
                    else {
                        txt(gr, P.search, RF(sb.X + 12, sb.Y, sb.Width - 24, sb.Height), F.label, C(T_TEXT));
                        float cw = std::min(txtW(gr, P.search, F.label), sb.Width - 26);
                        gp::SolidBrush cb(C(T_TEXT)); gr.FillRectangle(&cb, sb.X + 13 + cw, sb.Y + 8, 1.f, 14.f);
                    }
                    popReg(sb, P_SEARCH);
                    break;
                }
                case SR_CAPTION: txt(gr, s.label, RF(14, r.Y, w - 28, r.Height), s.label == L"WINDOWS" ? F.tiny : F.label, C(T_MUTED), AL_L, false); break;
            }
        }
    } else if (P.kind == PK_MIC) {
        std::wstring dev = !g.s.micEnabled ? L"Microphone off" : ddItem(H_DD_MICDEV, ddSel(H_DD_MICDEV));
        icon(gr, g.s.micMuted ? G_MICMUTED : G_MIC, 16, 18, 16, C(g.s.micMuted ? T_ACC_B : T_TEXT2));
        txt(gr, dev, RF(40, 12, w - 40 - 64, 28), F.title, C(T_TEXT));
        RF mb(w - 16 - 36, 12, 36, 28);
        if (g.s.micMuted) fillRR(gr, mb, 6, C(T_ACC, 0x24));
        else if (popHov(P_MUTE) > 0) fillRR(gr, mb, 6, WA((int)(A_SUBTLE * popHov(P_MUTE))));
        strokeRR(gr, mb, 6, g.s.micMuted ? C(T_ACC) : WA(A_DEFAULT));
        icon(gr, G_MICMUTED, mb.X + 10, mb.Y + 6, 16, C(g.s.micMuted ? T_ACC_B : T_TEXT2));
        popReg(mb, P_MUTE);
        txt(gr, L"Volume", RF(16, 50, 120, 20), F.label, C(T_TEXT2), AL_L, false);
        float frac = g.s.micVolume / 200.f;
        fillRR(gr, RF(16, 82, 200, 4), 2, C(T_TRACK));
        fillRR(gr, RF(16, 82, 200 * frac, 4), 2, C(T_ACC));
        float kx = 16 + 200 * frac;
        float hv = std::max(popHov(P_SL_VOL), P.dragVol ? 1.f : 0.f);
        if (hv > 0) fillCircle(gr, kx, 84, 12, WA((int)(A_SUBTLE * hv)));
        fillCircle(gr, kx, 84, 7, C(0xFFFFFF));
        popReg(RF(6, 70, 220, 28), P_SL_VOL);
        txt(gr, std::to_wstring(g.s.micVolume) + L"%", RF(232, 72, 40, 24), F.label, C(T_TEXT2), AL_L, false);
        txt(gr, L"Level", RF(16, 104, 120, 20), F.label, C(T_TEXT2), AL_L, false);
        size_t keep = g_meterRects.size();
        meterW(gr, 16, 132, 246, 6, g_meter[0], g.s.micEnabled && !g.s.micMuted);
        g_meterRects.resize(keep);
    }
    // scrollbar
    if (P.kind != PK_MIC && P.contentH > h + 0.5f) {
        float th = std::max(24.f, h * h / P.contentH), ty = (h - th) * (P.scroll / (P.contentH - h));
        fillRR(gr, RF(w - 6, ty + 2, 3, th - 4), 1.5f, WA(A_STRONG));
    }
}
void drawPopupFrame(gp::Graphics& gr, bool shadow = true) {
    RF r(0, 0, P.w, P.h);
    if (shadow) for (int i = 10; i >= 1; --i) fillRR(gr, RF(r.X - i, r.Y - i + 6, r.Width + 2.f * i, r.Height + 2.f * i), 10.f + i, gp::Color(7, 0, 0, 0));
    if (P.kind == PK_MIC) {  // 8 px arrow pointing at the chip
        gp::PointF tri[3] = {gp::PointF(P.w / 2 - 8, P.h - 1), gp::PointF(P.w / 2 + 8, P.h - 1), gp::PointF(P.w / 2, P.h + 7)};
        gp::SolidBrush ab(C(T_ELEV)); gr.FillPolygon(&ab, tri, 3);
        gp::Pen ap(WA(A_DEFAULT), 1.f); gr.DrawLine(&ap, tri[0].X, tri[0].Y + 0.5f, tri[2].X, tri[2].Y); gr.DrawLine(&ap, tri[2].X, tri[2].Y, tri[1].X, tri[1].Y + 0.5f);
    }
    fillRR(gr, r, 10, C(T_ELEV));
    gp::GraphicsPath clip; rrPath(clip, r, 10);
    gp::GraphicsState st = gr.Save();
    gr.SetClip(&clip, gp::CombineModeIntersect);
    paintPopupContent(gr);
    gr.Restore(st);
    strokeRR(gr, r, 10, WA(A_DEFAULT));
}
void ulw(HWND h, int pw, int ph, POINT* pos, BYTE alpha, const std::function<void(gp::Graphics&)>& draw) {
    if (pw <= 0 || ph <= 0) return;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = pw; bi.bmiHeader.biHeight = -ph;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    HDC sdc = GetDC(nullptr); HDC mem = CreateCompatibleDC(sdc);
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hb) {
        HGDIOBJ old = SelectObject(mem, hb);
        {
            gp::Bitmap bmp(pw, ph, pw * 4, PixelFormat32bppPARGB, (BYTE*)bits);
            gp::Graphics gr(&bmp);
            gr.Clear(gp::Color(0, 0, 0, 0));
            setupGraphics(gr, false);
            draw(gr);
        }
        BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        POINT src{0, 0}; SIZE sz{pw, ph};
        UpdateLayeredWindow(h, sdc, pos, &sz, mem, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old); DeleteObject(hb);
    }
    DeleteDC(mem); ReleaseDC(nullptr, sdc);
}
void renderPopup() {
    if (!P.hwnd || P.kind == PK_NONE || !g.hwnd) return;
    const float s = scale(), M = kShadow;
    const bool up = P.kind == PK_MIC || P.above;
    float o, ty, sy = 1.f;
    if (P.closing) { float e = (float)ease(E_IN, P.close.progress()); o = 1 - e; ty = (up ? 4.f : -4.f) * e; }
    else { float e = P.open.value(); o = std::max(0.f, std::min(1.f, e)); ty = (up ? 6.f : -6.f) * (1 - e); if (P.kind != PK_MIC) sy = 0.98f + 0.02f * e; }
    int pw = (int)std::ceil((P.w + 2 * M) * s), ph = (int)std::ceil((P.h + 2 * M) * s);
    POINT pt{(LONG)std::lround((P.x - M) * s), (LONG)std::lround((P.y - M) * s)};
    ClientToScreen(g.hwnd, &pt);
    ulw(P.hwnd, pw, ph, &pt, (BYTE)std::lround(255 * o), [&](gp::Graphics& gr) {
        gr.ScaleTransform(s, s);
        gr.TranslateTransform(M, M + ty);
        if (sy != 1.f) { float oy = up ? P.h : 0.f; gr.TranslateTransform(0, oy); gr.ScaleTransform(1, sy); gr.TranslateTransform(0, -oy); }
        drawPopupFrame(gr);
    });
}
int popupHit(float x, float y) {  // card-local logical coords
    for (auto it = P.regs.rbegin(); it != P.regs.rend(); ++it) if (inRF(it->r, x, y)) return it->id;
    return inRF(RF(0, 0, P.w, P.h), x, y) ? P_PAD : -1;
}
void popupSetVolFromX(float x) { setMicVolume((int)std::lround((x - 16) / 200.f * 200.f), false); }
LRESULT CALLBACK PopupProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const float s = scale(), M = kShadow;
    auto local = [&](float& x, float& y) { x = GET_X_LPARAM(lp) / s - M; y = GET_Y_LPARAM(lp) / s - M; };
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, P.hover >= P_BASE && P.hover != P_PAD ? IDC_HAND : IDC_ARROW)); return TRUE;
        case WM_MOUSEMOVE: {
            float x, y; local(x, y);
            if (P.dragVol) { popupSetVolFromX(x); return 0; }
            int id = popupHit(x, y);
            if (id != P.hover) {
                if (P.hover >= 0) P.hov[P.hover].go(0, 80, E_LINEAR);
                if (id >= 0 && id != P_PAD) P.hov[id].go(1, 80, E_LINEAR);
                P.hover = id;
                if (id >= P_BASE && id < P_BASE + 900) P.kb = -1;
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t);
                renderPopup();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (P.hover >= 0) P.hov[P.hover].go(0, 80, E_LINEAR);
            P.hover = -1; renderPopup(); return 0;
        case WM_LBUTTONDOWN: {
            float x, y; local(x, y);
            int id = popupHit(x, y);
            P.pressed = id;
            if (id == P_SL_VOL) { P.dragVol = true; SetCapture(h); popupSetVolFromX(x); }
            if (id < 0) closePopup();
            return 0;
        }
        case WM_LBUTTONUP: {
            float x, y; local(x, y);
            if (P.dragVol) { P.dragVol = false; ReleaseCapture(); save(); renderPopup(); return 0; }
            int id = popupHit(x, y);
            if (id >= 0 && id == P.pressed && id != P_PAD) popupClick(id, x);
            P.pressed = -1;
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (P.kind != PK_MIC && P.contentH > P.h) {
                P.scroll = std::max(0.f, std::min(P.contentH - P.h, P.scroll - GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 48.f));
                renderPopup();
            }
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
void ensurePopupWindow() {
    if (P.hwnd || !g.hwnd) return;
    WNDCLASSW wc{}; wc.lpfnWndProc = PopupProc; wc.hInstance = g.inst; wc.lpszClassName = L"CastRecorderPopup"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    P.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, g.hwnd, nullptr, g.inst, nullptr);
}

// ---------- preview chips: layered child windows above the swapchain child ----------
HWND g_chipL = nullptr, g_chipR = nullptr;
float measureText(const std::wstring& t, gp::Font* f) {
    static gp::Bitmap* mb = nullptr; if (!mb) mb = new gp::Bitmap(4, 4, PixelFormat32bppPARGB);
    gp::Graphics gr(mb); setupGraphics(gr, false); return txtW(gr, t, f);
}
LRESULT CALLBACK ChipProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}
void updateChips() {
    if (!g.hwnd) return;
    if (!g_chipL) {
        WNDCLASSW wc{}; wc.lpfnWndProc = ChipProc; wc.hInstance = g.inst; wc.lpszClassName = L"CastRecorderChip";
        RegisterClassW(&wc);
        g_chipL = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT, wc.lpszClassName, L"", WS_CHILD, 0, 0, 1, 1, g.hwnd, nullptr, g.inst, nullptr);
        g_chipR = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT, wc.lpszClassName, L"", WS_CHILD, 0, 0, 1, 1, g.hwnd, nullptr, g.inst, nullptr);
    }
    bool show = g.gprev.live() && g_viewIdx == 0 && !g_view.active() && IsWindowVisible(g.hwnd) && !IsIconic(g.hwnd);
    if (!show) { ShowWindow(g_chipL, SW_HIDE); ShowWindow(g_chipR, SW_HIDE); return; }
    const float s = scale();
    RecLayout L = recLayout(clientW(), clientH());
    float lw = measureText(L"Live preview", F.caption) + 30;
    std::wstring rt = sizeFpsText();
    float rw = measureText(rt, F.caption) + 16;
    auto place = [&](HWND hw, float x, float y, float w, float hgt, const std::function<void(gp::Graphics&)>& d) {
        int pw = (int)std::ceil(w * s), ph = (int)std::ceil(hgt * s);
        SetWindowPos(hw, HWND_TOP, (int)std::lround(x * s), (int)std::lround(y * s), pw, ph, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ulw(hw, pw, ph, nullptr, 255, [&](gp::Graphics& gr) { gr.ScaleTransform(s, s); d(gr); });
    };
    place(g_chipL, L.surf.X + 12, L.surf.Y + 12, lw, 22, [&](gp::Graphics& gr) { paintLiveChip(gr, 0, 0); });
    place(g_chipR, L.surf.X + L.surf.Width - 12 - rw, L.surf.Y + 12, rw, 22, [&](gp::Graphics& gr) { paintSizeChip(gr, RF(0, 0, rw, 22)); });
}
// ---------- audio meters / monitoring / push-to-talk ----------
std::mutex g_meterM;
std::unique_ptr<AudioMixer> g_meterMixer;
std::thread g_meterTh;
// A capture-only mixer that feeds the level meters (and monitoring) whether or not we're recording. It never
// gets a media origin, so it buffers nothing. Opened off the UI thread (WASAPI init is 50-150 ms per device).
void restartMeter() {
    if (g_renderOnly) return;
    if (g_meterTh.joinable()) g_meterTh.join();
    bool sys = g.s.systemAudio, mic = g.s.micEnabled;
    std::wstring mid = W(g.s.micDevice), sid = W(g.s.systemDevice);
    g_meterTh = std::thread([sys, mic, mid, sid] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::unique_ptr<AudioMixer> old;
        { std::lock_guard<std::mutex> l(g_meterM); old = std::move(g_meterMixer); }
        old.reset();
        auto m = std::make_unique<AudioMixer>();
        std::wstring log;
        if (sys || mic) m->start(sys, mic, mid, false, log, sid, false, true);
        { std::lock_guard<std::mutex> l(g_meterM); g_meterMixer = std::move(m); }
        if (SUCCEEDED(co)) CoUninitialize();
    });
}
void stopMeter() {
    if (g_meterTh.joinable()) g_meterTh.join();
    std::lock_guard<std::mutex> l(g_meterM); g_meterMixer.reset();
}
std::atomic<unsigned> g_pttMods{0}, g_pttVk{0};
std::atomic<bool> g_pttQuit{false};
std::thread g_pttTh;
void syncPtt() { g_pttMods = g.s.pttKey.mods; g_pttVk = g.s.pttKey.vk; applyAudioFx(); }
void startPttThread() {
    g_pttTh = std::thread([] {  // held-key polling: works while other apps have focus, no hook needed
        while (!g_pttQuit.load()) {
            bool down = false;
            unsigned vk = g_pttVk.load(), mods = g_pttMods.load();
            if (audioFx().pushToTalk.load() && vk) {
                auto held = [](int k) { return (GetAsyncKeyState(k) & 0x8000) != 0; };
                down = held((int)vk);
                if (mods & MOD_CTRL_) down = down && held(VK_CONTROL);
                if (mods & MOD_ALT_) down = down && held(VK_MENU);
                if (mods & MOD_SHIFT_) down = down && held(VK_SHIFT);
                if (mods & MOD_WIN_) down = down && (held(VK_LWIN) || held(VK_RWIN));
            }
            audioFx().pttDown = down;
            Sleep(10);
        }
    });
}

// ---------- actions ----------
void switchView(int v) {
    if (v == g_viewIdx) return;
    closePopup(false);
    if (g.capturing >= 0) endCapture();
    g_viewIdx = v;
    g_view.set(0); g_view.go(1, 150, E_OUT);
    g.gprev.setVisible(v == 0 && IsWindowVisible(g.hwnd) && !IsIconic(g.hwnd));
    if (v == 1) { g.mics = listMicrophones(); g.outs = listOutputDevices(); enumGpus(); }
    updateChips();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}
void toggleMic() {
    g.s.micMuted = !g.s.micMuted;
    g.micMuted = g.s.micMuted;
    applyAudioFx();
    g.rec.setMicMuted(g.micMuted);
    save();
    if (!g.s.micEnabled) setStatus(L"Microphone is off (turn it on in Settings)", toRef(T_TEXT2));
    else setStatus(g.micMuted ? L"Mic muted" : L"Mic on", toRef(g.micMuted ? T_WARN : T_OK));
    if (P.kind == PK_MIC) renderPopup();
}
void toggleSys() {
    g.s.systemMuted = !g.s.systemMuted;
    applyAudioFx(); save();
    if (!g.s.systemAudio) setStatus(L"System audio is off (turn it on in Settings)", toRef(T_TEXT2));
    else setStatus(g.s.systemMuted ? L"System audio muted" : L"System audio on", toRef(g.s.systemMuted ? T_WARN : T_OK));
}
void setSlider(int id, float x) {
    const float cx = 772;
    float f = std::max(0.f, std::min(1.f, (x - cx) / 240.f));
    if (id == H_SL_VOL) setMicVolume((int)std::lround(f * 200), false);
    else if (id == H_SL_GATE) { g.s.noiseGateDb = (int)std::lround(-80 + f * 80); applyAudioFx(); InvalidateRect(g.hwnd, nullptr, FALSE); }
}
void stepSlider(int id, int d) {
    if (id == H_SL_VOL) setMicVolume(g.s.micVolume + d, true);
    else if (id == H_SL_GATE) { g.s.noiseGateDb = std::max(-80, std::min(0, g.s.noiseGateDb + d)); applyAudioFx(); save(); InvalidateRect(g.hwnd, nullptr, FALSE); }
}
RF regionOf(int id) { for (auto& r : g_regs) if (r.id == id) return r.r; return RF(0, 0, 0, 0); }
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
    Hotkey* dst = g.capturing == 0 ? &g.s.toggleRecord : g.capturing == 1 ? &g.s.pauseToggle : g.capturing == 2 ? &g.s.micMute : &g.s.pttKey;
    UINT mods = currentMods();
    if (isModifierKey(vk)) { g.liveMods = mods; InvalidateRect(g.hwnd, nullptr, FALSE); return true; }
    if (!down && vk != VK_SNAPSHOT) return true;
    if (mods == 0 && vk == VK_ESCAPE) { endCapture(); return true; }
    if (mods == 0 && (vk == VK_BACK || vk == VK_DELETE)) { *dst = Hotkey{}; save(); syncPtt(); endCapture(); return true; }
    Hotkey hk{mods, vk};
    int idx = g.capturing;
    if (keyName(vk).empty()) { setStatus(L"Key not supported", toRef(T_ERR)); return true; }
    const Hotkey* all[4] = {&g.s.toggleRecord, &g.s.pauseToggle, &g.s.micMute, &g.s.pttKey};
    const wchar_t* names[4] = {L"Start / Stop", L"Pause", L"Mic mute", L"Push-to-talk"};
    int clash = -1;
    for (int k = 0; k < 4; ++k) if (k != idx && hk == *all[k]) clash = k;
    Issue is = validate(hk);
    bool bad = idx < 3 && is.severity == Severity::Error;  // push-to-talk is polled, not registered: any held combo works
    if (bad || clash >= 0) {
        g.capturing = -1; g.liveMods = 0;
        registerAll();
        setStatus(W(format(hk)) + (clash >= 0 ? std::wstring(L" is used by ") + names[clash] : L": " + W(is.message)), toRef(T_ERR));
        InvalidateRect(g.hwnd, nullptr, FALSE);
        return true;
    }
    if (hk == g.s.openFolder) g.s.openFolder = Hotkey{};
    *dst = hk;
    save();
    syncPtt();
    endCapture();
    return true;
}
void showWindow() {
    ShowWindow(g.hwnd, IsIconic(g.hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g.hwnd);
}
void click(int id) {
    if (id != H_HK0 && id != H_HK1 && id != H_HK2 && id != H_HKPTT && g.capturing >= 0) endCapture();
    const RF r = regionOf(id);
    switch (id) {
        case H_MIN: ShowWindow(g.hwnd, SW_MINIMIZE); return;
        case H_CLOSE: forceQuit(); return;  // exits; a live recording is stopped and saved first
        case H_MAX: ShowWindow(g.hwnd, IsZoomed(g.hwnd) ? SW_RESTORE : SW_MAXIMIZE); save(); return;
        case H_TAB_REC: switchView(0); return;
        case H_TAB_SET: switchView(1); return;
        case H_LOG: case H_STATUSLOG: openLog(); return;
        case H_RECORD: toggleRecording(); break;
        case H_PAUSE: if (g.rec.isActive()) togglePause(); break;
        case H_MICICON: toggleMic(); break;
        case H_SYSCHIP: toggleSys(); break;
        case H_MICCHIP: openPopup(PK_MIC, H_MICCHIP, r); break;
        case H_SRC:
            if (locked(id)) { setStatus(L"Stop recording to change the source", toRef(T_TEXT2)); break; }
            openPopup(PK_SOURCE, H_SRC, r); break;
        case H_DD_FPS: case H_DD_QUAL: case H_DD_ENC: case H_DD_GPU: case H_DD_RES: case H_DD_CONT: case H_DD_SYSDEV: case H_DD_MICDEV:
            if (locked(id)) { setStatus(L"Stop recording to change this", toRef(T_TEXT2)); break; }
            if (id == H_DD_GPU) enumGpus();
            if (id == H_DD_MICDEV) g.mics = listMicrophones();
            if (id == H_DD_SYSDEV) g.outs = listOutputDevices();
            openPopup(PK_LIST, id, r); break;
        case H_HK0: case H_HK1: case H_HK2: case H_HKPTT:
            if (locked(id)) { setStatus(L"Stop recording to change hotkeys", toRef(T_TEXT2)); break; }
            g.capturing = id == H_HKPTT ? 3 : id - H_HK0;
            g.liveMods = 0;
            unregisterAll();
            SetFocus(g.hwnd);
            break;
        case H_FOLDERICON: openFolder(); break;
        case H_PATH: case H_BROWSE: if (!locked(H_BROWSE)) pickFolder(); break;
        case H_TG_SYS: if (!locked(id)) { g.s.systemAudio = !g.s.systemAudio; save(); restartMeter(); } break;
        case H_TG_MIC: if (!locked(id)) { g.s.micEnabled = !g.s.micEnabled; save(); restartMeter(); } break;
        case H_TG_NS: g.s.noiseSuppression = !g.s.noiseSuppression; applyAudioFx(); save(); break;
        case H_TG_MON: g.s.monitoring = !g.s.monitoring; applyAudioFx(); save(); break;
        case H_TG_PTT: g.s.pushToTalk = !g.s.pushToTalk; syncPtt(); save(); break;
        case H_CH_MONO: case H_CH_STEREO:
            if (!locked(id)) { int c = id == H_CH_MONO ? 1 : 2; if (g.s.channels != c) { g.s.channels = c; save(); } } break;
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}
std::vector<int> focusOrder() {
    if (g_viewIdx == 0) {
        std::vector<int> v = {H_TAB_REC, H_TAB_SET, H_SRC, H_DD_FPS, H_DD_QUAL, H_MICCHIP, H_SYSCHIP};
        if (uiActive()) v.push_back(H_PAUSE);
        v.push_back(H_RECORD);
        return v;
    }
    return {H_TAB_REC, H_TAB_SET, H_DD_ENC, H_DD_GPU, H_DD_RES, H_DD_CONT, H_HK0, H_HK1, H_HK2, H_PATH, H_BROWSE, H_LOG,
            H_TG_SYS, H_DD_SYSDEV, H_TG_MIC, H_DD_MICDEV, H_SL_VOL, H_TG_NS, H_SL_GATE, H_TG_MON, H_TG_PTT, H_HKPTT, H_CH_MONO};
}
void moveFocus(int dir) {
    auto v = focusOrder();
    int n = (int)v.size(), i = -1;
    for (int k = 0; k < n; ++k) if (v[k] == g_focus) i = k;
    i = i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
    g_focus = v[i];
    g_kbUsed = true;
}
int hitTest(float x, float y) {
    for (auto it = g_regs.rbegin(); it != g_regs.rend(); ++it) if (inRF(it->r, x, y)) return it->id;
    return H_NONE;
}
void setHover(int id) {
    if (id == g.hover) return;
    if (g.hover != H_NONE) g_hov[g.hover].go(0, 100, E_LINEAR);
    if (id != H_NONE) g_hov[id].go(1, 100, E_LINEAR);
    g.hover = id;
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// ---------- frame clock (16 ms): animations, meters, record pulse ----------
void kickAnim() { if (g.hwnd && !g_renderOnly) SetTimer(g.hwnd, TIMER_FRAME, 16, nullptr); }
bool anyActive(const std::map<int, Anim>& m) { for (auto& p : m) if (p.second.active()) return true; return false; }
void invalidateLogical(RF r) { RECT px = toPx(r); InvalidateRect(g.hwnd, &px, FALSE); }
void frameTick() {
    double t = nowMs(), dt = g_meterT ? t - g_meterT : 16; g_meterT = t;
    bool changed = false;
    std::atomic<float>* lv[2] = {&audioFx().levelMic, &audioFx().levelSys};
    for (int k = 0; k < 2; ++k) {
        float pk = AudioFx::take(*lv[k]);
        float n = pk > 1e-6f ? std::max(0.f, std::min(1.f, (20.f * std::log10(pk) + 60.f) / 60.f)) : 0.f;
        float d = std::max(n, g_meter[k] - (float)(dt / 300.0));  // 60 Hz attack, 300 ms full-scale decay
        if (d < 0) d = 0;
        if (std::fabs(d - g_meter[k]) > 0.004f) changed = true;
        g_meter[k] = d;
    }
    bool anim = g_view.active() || g_chan.active() || anyActive(g_tog) || anyActive(g_hov);
    if (P.kind != PK_NONE) {
        bool pa = P.open.active() || P.close.active() || anyActive(P.hov);
        if (pa || (changed && P.kind == PK_MIC)) renderPopup();
        if (pa) anim = true;  // the field chevron rotates with the list
        if (P.closing && !P.close.active()) { ShowWindow(P.hwnd, SW_HIDE); P.kind = PK_NONE; P.closing = false; anim = true; }
    }
    static bool viewWasActive = false;
    if (viewWasActive && !g_view.active()) updateChips();
    viewWasActive = g_view.active();
    if (anim) InvalidateRect(g.hwnd, nullptr, FALSE);
    else {
        if (changed) for (auto& r : g_meterRects) invalidateLogical(r);
        if (uiActive()) { invalidateLogical(g_recordRect); invalidateLogical(g_statusRect); }
    }
}
// ---------- window procedure ----------
HBITMAP g_back = nullptr; int g_backW = 0, g_backH = 0; void* g_backBits = nullptr;
void paintTo(HWND h, HDC dc, const RECT& clip) {
    RECT rc; GetClientRect(h, &rc);
    if (!g_back || g_backW != rc.right || g_backH != rc.bottom) {
        if (g_back) DeleteObject(g_back);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = std::max(1L, rc.right); bi.bmiHeader.biHeight = -std::max(1L, rc.bottom);
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        g_back = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &g_backBits, nullptr, 0);
        g_backW = rc.right; g_backH = rc.bottom;
    }
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, g_back);
    {
        gp::Graphics gr(mem);
        setupGraphics(gr, true);
        const float s = scale();
        gr.ScaleTransform(s, s);
        gr.SetClip(RF(clip.left / s - 1, clip.top / s - 1, (clip.right - clip.left) / s + 2, (clip.bottom - clip.top) / s + 2));
        paintAll(gr, rc.right / s, rc.bottom / s);
    }
    BitBlt(dc, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top, mem, clip.left, clip.top, SRCCOPY);
    SelectObject(mem, old); DeleteDC(mem);
}
int ncHit(HWND h, POINT sp) {
    POINT p = sp; ScreenToClient(h, &p);
    RECT rc; GetClientRect(h, &rc);
    const float s = scale();
    const int e = (int)std::lround(6 * s);
    if (!IsZoomed(h)) {
        bool L = p.x < e, R = p.x >= rc.right - e, T = p.y < e, B = p.y >= rc.bottom - e;
        if (T && L) return HTTOPLEFT; if (T && R) return HTTOPRIGHT; if (B && L) return HTBOTTOMLEFT; if (B && R) return HTBOTTOMRIGHT;
        if (L) return HTLEFT; if (R) return HTRIGHT; if (T) return HTTOP; if (B) return HTBOTTOM;
    }
    float x = p.x / s, y = p.y / s;
    if (y < 40) {
        float W = rc.right / s;
        if (inRF(tbBtn(W, 1), x, y)) return HTMAXBUTTON;  // Win11 snap layouts flyout
        if (inRF(tbBtn(W, 0), x, y) || inRF(tbBtn(W, 2), x, y)) return HTCLIENT;
        if (inRF(RF(150, 7, 68, 26), x, y) || inRF(RF(222, 7, 80, 26), x, y)) return HTCLIENT;
        return HTCAPTION;  // double-click toggles maximize
    }
    return HTCLIENT;
}
LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const float s = scale();
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp && IsZoomed(h)) {  // maximized borderless: keep the client inside the monitor work area
            auto* p = (NCCALCSIZE_PARAMS*)lp;
            int fx = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            int fy = GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            p->rgrc[0].left += fx; p->rgrc[0].right -= fx; p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
            return 0;
        }
        if (wp) return 0;
        break;
    case WM_NCHITTEST: return ncHit(h, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
    case WM_NCMOUSEMOVE:
        if (wp == HTMAXBUTTON) { setHover(H_MAX); TRACKMOUSEEVENT t{sizeof t, TME_LEAVE | TME_NONCLIENT, h, 0}; TrackMouseEvent(&t); }
        else setHover(H_NONE);
        if (wp == HTMAXBUTTON) return 0;
        break;
    case WM_NCMOUSELEAVE: if (g.hover == H_MAX) setHover(H_NONE); break;
    case WM_NCLBUTTONDOWN:
        if (wp == HTMAXBUTTON) { closePopup(); g.pressed = H_MAX; return 0; }
        closePopup(false);
        break;
    case WM_NCLBUTTONUP:
        if (wp == HTMAXBUTTON) { if (g.pressed == H_MAX) click(H_MAX); g.pressed = H_NONE; return 0; }
        break;
    case WM_NCLBUTTONDBLCLK: if (wp == HTMAXBUTTON) return 0; break;
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize = {(LONG)std::lround(MIN_W * s), (LONG)std::lround(MIN_H * s)};
        return 0;
    }
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        makeFonts(); makeUiFonts();
        closePopup(false);
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        g.gprev.setRect(previewRectNow());
        updateChips();
        return 0;
    }
    case WM_ENTERSIZEMOVE: closePopup(false); break;
    case WM_MOVE: if (P.kind != PK_NONE) closePopup(false); break;
    case WM_SHOWWINDOW:
        if (wp) { updatePreview(); SetTimer(h, TIMER_PREVIEW, 500, nullptr); SetTimer(h, TIMER_FRAME, 16, nullptr); }
        else { KillTimer(h, TIMER_PREVIEW); KillTimer(h, TIMER_FRAME); }
        g.gprev.setVisible(wp != 0 && g_viewIdx == 0);
        break;
    case WM_SIZE:
        closePopup(false);
        g.gprev.setRect(previewRectNow());
        g.gprev.setVisible(wp != SIZE_MINIMIZED && IsWindowVisible(h) && g_viewIdx == 0);
        if (wp == SIZE_MINIMIZED) KillTimer(h, TIMER_FRAME); else SetTimer(h, TIMER_FRAME, 16, nullptr);
        updateChips();
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_APP + 41:  // GPU preview went live (1) or failed (0)
        if (wp) g.gprev.setVisible(IsWindowVisible(h) && !IsIconic(h) && g_viewIdx == 0);
        else { if (g.gprev.hwnd()) ShowWindow(g.gprev.hwnd(), SW_HIDE); updatePreview(); }
        updateChips();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP + 44:  // the preview's window target is gone
        if (g.target.kind == CaptureTarget::Window && !g.rec.running()) {
            g.target = CaptureTarget{}; g.target.kind = CaptureTarget::AllDisplays; g_srcTitle.clear();
            setStatus(L"Window closed", toRef(T_WARN));
            cr::appLog(L"source: captured window closed, preview back on Full screen");
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paintTo(h, dc, ps.rcPaint);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        float x = GET_X_LPARAM(lp) / s, y = GET_Y_LPARAM(lp) / s;
        if (g_drag != H_NONE) { setSlider(g_drag, x); return 0; }
        int id = hitTest(x, y);
        if (id != g.hover) { setHover(id); TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t); }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            bool hand = g.hover != H_NONE && !locked(g.hover) && !(g.hover == H_PAUSE && !uiActive());
            SetCursor(LoadCursor(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSELEAVE: if (g_drag == H_NONE) setHover(H_NONE); return 0;
    case WM_LBUTTONDOWN: {
        float x = GET_X_LPARAM(lp) / s, y = GET_Y_LPARAM(lp) / s;
        int id = hitTest(x, y);
        // a click outside an open popup closes it; clicking its own field again just closes it
        if (P.kind != PK_NONE && !P.closing) {
            int owner = P.dd; closePopup();
            if (id == owner || (owner == H_MICCHIP && id == H_MICCHIP)) { g.pressed = H_NONE; return 0; }
        }
        g.pressed = id;
        if ((id == H_SL_VOL || id == H_SL_GATE) && !locked(id)) { g_drag = id; g_focus = id; setSlider(id, x); }
        SetCapture(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        float x = GET_X_LPARAM(lp) / s, y = GET_Y_LPARAM(lp) / s;
        if (g_drag != H_NONE) { g_drag = H_NONE; save(); g.pressed = H_NONE; InvalidateRect(h, nullptr, FALSE); return 0; }
        int id = hitTest(x, y);
        if (id != H_NONE && id == g.pressed) { if (id != H_PAUSE || uiActive()) click(id); }
        else if (id == H_NONE && g.capturing >= 0) endCapture();
        g.pressed = H_NONE;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (P.kind == PK_SOURCE || P.kind == PK_LIST) { PopupProc(P.hwnd, msg, wp, lp); return 0; }
        break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (hotkeyKey((UINT)wp, true)) return 0;
        if (P.kind != PK_NONE && !P.closing) {
            if (wp == VK_ESCAPE) { closePopup(); return 0; }
            if (wp == VK_UP || wp == VK_DOWN || wp == VK_RETURN) { g_kbUsed = true; popupKey((UINT)wp); return 0; }
            if (P.kind == PK_SOURCE && wp == VK_BACK) { if (!P.search.empty()) popupType(P.search.substr(0, P.search.size() - 1)); return 0; }
            if (P.kind == PK_MIC && (wp == VK_LEFT || wp == VK_RIGHT)) { setMicVolume(g.s.micVolume + (wp == VK_RIGHT ? 1 : -1), true); return 0; }
            if (wp == VK_TAB) { closePopup(); }
            else if (msg == WM_KEYDOWN) break;  // characters arrive as WM_CHAR (search box)
        }
        if (msg == WM_KEYDOWN) {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (wp == VK_TAB) { moveFocus(shift ? -1 : 1); InvalidateRect(h, nullptr, FALSE); return 0; }
            if (g_focus == H_SL_VOL || g_focus == H_SL_GATE) {
                int d = wp == VK_RIGHT || wp == VK_UP ? 1 : wp == VK_LEFT || wp == VK_DOWN ? -1 : wp == VK_PRIOR ? 10 : wp == VK_NEXT ? -10 : 0;
                if (d) { stepSlider(g_focus, d); return 0; }
            }
            if (g_focus == H_CH_MONO && (wp == VK_LEFT || wp == VK_RIGHT)) { click(wp == VK_LEFT ? H_CH_MONO : H_CH_STEREO); return 0; }
            if ((wp == VK_RETURN || wp == VK_SPACE) && g_focus != H_NONE) {
                if (g_focus == H_CH_MONO) click(g.s.channels == 1 ? H_CH_STEREO : H_CH_MONO);
                else if (g_focus != H_SL_VOL && g_focus != H_SL_GATE) click(g_focus);
                return 0;
            }
        }
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (hotkeyKey((UINT)wp, false)) return 0;
        break;
    case WM_SYSCHAR: case WM_CHAR:
        if (g.capturing >= 0) return 0;
        if (P.kind == PK_SOURCE && !P.closing && msg == WM_CHAR && wp >= 32 && wp != 127) { popupType(P.search + (wchar_t)wp); return 0; }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) { if (g.capturing >= 0) endCapture(); closePopup(); }
        break;
    case WM_KILLFOCUS: if (g.capturing >= 0) endCapture(); break;
    case WM_HOTKEY:
        if (wp == HK_TOGGLE) toggleRecording();
        else if (wp == HK_MIC) toggleMic();
        else if (wp == HK_PAUSE) togglePause();
        else if (wp == HK_FOLDER) openFolder();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_FRAME) { frameTick(); return 0; }
        if (wp == TIMER_SELFTEST + 1) { KillTimer(h, wp); click(H_CLOSE); return 0; }  // stop + save + exit
        if (wp == TIMER_SELFTEST) { KillTimer(h, TIMER_SELFTEST); selftestSettings(); return 0; }
        if (wp == TIMER_PREVIEW) {
            static int n = 0;
            if (!g.rec.running() && IsWindowVisible(h) && !IsIconic(h) && !g.gprev.live() && g_viewIdx == 0) { updatePreview(); InvalidateRect(h, nullptr, FALSE); }
            if (++n % 2 == 0) updateChips();  // source size / fps text
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
                    setStatus(L"Stopped (finalize took too long)", toRef(T_WARN));
                    KillTimer(h, TIMER_TICK);
                }
            } else finishSince = 0;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_APP + 5:
        toggleRecording();
        SetTimer(h, TIMER_SELFTEST + 1, 2000, nullptr);
        return 0;
    case WM_APP_WARMUP_DONE:
        g.warm.waitUntilIdle(200);
        if (!g.warm.running()) { writeSessionLog(g.warm.result(), L"warm-up (encoder probe cache)"); g.warmLogged = true; }
        return 0;
    case WM_APP_RECORDER_STARTED:
        if (g.rec.isActive()) playTone(IDR_SND_START);
        return 0;
    case WM_APP_RECORDER_DONE: {
        g.rec.waitUntilIdle(100);
        if (!g.recPending) return 0;  // already handled (e.g. logged by forceQuit)
        g.recPending = false;
        KillTimer(h, TIMER_TICK);
        const RecordResult& r = g.rec.result();
        g.gprev.setRecording(false);
        writeSessionLog(r);
        size_t k = r.file.find_last_of(L'\\');
        std::wstring fname = k == std::wstring::npos ? r.file : r.file.substr(k + 1);
        if (r.ok && r.windowClosed) {
            setStatus(L"Window closed  \u00b7  saved " + fname, toRef(T_WARN));
            g.target = CaptureTarget{}; g.target.kind = CaptureTarget::AllDisplays; g_srcTitle.clear();
            g.gprev.setTarget(g.target);
        } else if (r.ok) {
            std::wstring msg2 = L"Saved " + fname + L"  \u00b7  " + r.encoderUsed;
            if (!r.message.empty()) msg2 += L"  \u00b7  " + r.message;
            if (r.cpuFallback) msg2 += L"  \u00b7  GPU conversion failed, finished on CPU";
            if (r.dropped) msg2 += L"  \u00b7  " + std::to_wstring(r.dropped) + L" dropped";
            if (!r.encoderFallbacks.empty()) msg2 += L"  \u00b7  " + r.encoderFallbacks.front() + L" failed its test encode, fell back";
            setStatus(msg2, toRef(T_OK));
        } else if (r.abandoned) setStatus(L"Encoder got stuck, file discarded. Restart Cast Recorder to record again.", toRef(T_WARN));
        else if (r.timedOut) setStatus(L"Stopped (timed out): " + r.message, toRef(T_WARN));
        else setStatus(r.message.empty() ? L"Recording stopped" : (L"Recording failed: " + r.message), toRef(T_ERR));
        updatePreview();
        InvalidateRect(h, nullptr, FALSE);
        if (g.quitting) DestroyWindow(h);
        return 0;
    }
    case WM_APP + 2:
        showWindow();
        return 0;
    case WM_CLOSE:
        if (!g.quitting) forceQuit();
        return 0;
    case WM_DESTROY:
        save();
        closePopup(false);
        g.gprev.stop();
        unregisterAll();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void paintToNull() { gp::Bitmap b(8, 8, PixelFormat32bppPARGB); gp::Graphics gr(&b); gr.ScaleTransform(scale(), scale()); paintAll(gr, clientW(), clientH()); }
// CI: change settings through the same click paths a user takes, then press X.
void selftestSettings() {
    auto pick = [](int dd, int idx) { click(dd); popupActivate(idx); };
    click(H_TAB_REC);
    pick(H_DD_FPS, 1);   // 30 fps
    pick(H_DD_QUAL, 3);  // Ultra
    // Source: open the picker, type into its search box, take the first matching window (CI starts Notepad)
    click(H_SRC);
    popupType(L"notepad");
    int row = -1;
    for (size_t i = 0; i < P.rows.size(); ++i) if (P.rows[i].kind == SR_WIN) { row = (int)i; break; }
    if (row >= 0) popupActivate(row); else { popupType(L""); popupActivate(0); }
    cr::appLog(L"selftest: source " + sourceText());
    click(H_TAB_SET);
    paintToNull();
    pick(H_DD_CONT, 1);  // MKV
    if (g.s.systemAudio) click(H_TG_SYS);  // off
    if (!g.s.micEnabled) click(H_TG_MIC);  // on (default microphone)
    if (!g.s.noiseSuppression) click(H_TG_NS);
    if (!g.s.pushToTalk) click(H_TG_PTT);
    if (!g.s.monitoring) click(H_TG_MON);
    click(H_CH_STEREO);
    setSlider(H_SL_VOL, 772 + 240 * 0.75f);   // 150 %
    setSlider(H_SL_GATE, 772 + 240 * 0.375f); // -50 dB
    save();
    g.s.micMute = Hotkey{MOD_CTRL_ | MOD_ALT_, 0x7B}; save();  // Ctrl+Alt+F12
    click(H_MAX);
    click(H_CLOSE);
}

// Headless render for docs/CI screenshots (PNG or BMP). States: idle | source | settings | recording | micpop
int encoderClsid(const wchar_t* mime, CLSID* out) {
    UINT n = 0, size = 0; gp::GetImageEncodersSize(&n, &size);
    if (!size) return -1;
    std::vector<BYTE> buf(size); auto* info = (gp::ImageCodecInfo*)buf.data();
    gp::GetImageEncoders(n, size, info);
    for (UINT i = 0; i < n; ++i) if (wcscmp(info[i].MimeType, mime) == 0) { *out = info[i].Clsid; return (int)i; }
    return -1;
}
int renderUi(const std::wstring& path, const std::wstring& state, int dpi) {
    g_renderOnly = true;
    g.dpi = dpi;
    makeFonts(); makeUiFonts(); loadUiIcons();
    enumGpus();
    g.mics = listMicrophones(); g.outs = listOutputDevices();
    resolveSource();
    g.status = L"Ready"; g.statusColor = toRef(T_TEXT2);
    for (int i = 0; i < 3; ++i) g.hkMsgColor[i] = toRef(T_OK);
    g_meter[0] = 0.58f; g_meter[1] = 0.66f;
    g.s.micEnabled = true;  // screenshots show both chips live
    if (state == L"recording") { g_fakeRec = true; }
    if (state == L"settings") { g_viewIdx = 1; }
    updatePreview();
    const float s = scale(), W = 1100, H = 700;
    gp::Bitmap bmp((INT)std::lround(W * s), (INT)std::lround(H * s), PixelFormat32bppARGB);
    {
        gp::Graphics gr(&bmp);
        setupGraphics(gr, false);
        gr.ScaleTransform(s, s);
        paintAll(gr, W, H);
        RecLayout L = recLayout(W, H);
        if (state == L"source" || state == L"dropdown" || state == L"micpop") {
            P.kind = state == L"micpop" ? PK_MIC : PK_SOURCE;
            P.dd = state == L"micpop" ? H_MICCHIP : H_SRC;
            P.anchor = state == L"micpop" ? L.mic : L.src;
            layoutPopup();
            P.open.set(1);
            P.kb = -1;
            paintAll(gr, W, H);  // field shows its open state
            gr.TranslateTransform(P.x, P.y);
            drawPopupFrame(gr);
        }
    }
    CLSID cl{};
    bool png = path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".png") == 0;
    if (encoderClsid(png ? L"image/png" : L"image/bmp", &cl) < 0) return 1;
    return bmp.Save(path.c_str(), &cl, nullptr) == gp::Ok ? 0 : 1;
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
        "  --pattern            record the static test pattern instead of the screen (CI quality/fps)\n"
        "  --render-pattern FILE.bmp  write the test pattern reference image\n"
        "  --system-audio / --mic     record system audio / the default microphone\n"
        "  --test-tone          add a synthetic 440 Hz audio source (CI)\n"
        "  --pause-at S / --resume-at S  pause and resume at S seconds into the recording\n"
        "  --window TEXT        capture one window (title or exe substring) instead of the screen\n"
        "  --source all|monitor[:N]|window:TEXT   what to capture (default: the saved source)\n"
        "  --test-mic           synthetic noisy microphone through the real mic chain (CI)\n"
        "  --noise-suppression on|off / --noise-gate DB (-80 = off) / --mic-volume PCT / --channels 1|2\n"
        "  --list-windows       list capturable windows\n"
        "  --render-ui FILE.bmp  paint the settings window to a BMP (docs/CI)\n"
        "  --ui-state idle|source|settings|recording|micpop\n"
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
    o.systemAudio = false;  // CLI: audio only when asked for
    o.channels = s.channels;
    o.systemId = W(s.systemDevice);
    g.s = s;  // saved audio chain settings (volume, noise suppression, gate) apply to CLI recordings too
    applyAudioFx();
    audioFx().micMuted = false; audioFx().systemMuted = false; audioFx().pushToTalk = false; audioFx().monitoring = false;
    {   // saved source (same fallbacks as the app)
        g_displays = listDisplays();
        if (s.sourceKind == "all") o.allDisplays = true;
        else if (s.sourceKind == "monitor") { for (auto& d : g_displays) if (U(d.device) == s.sourceMonitor) o.monitor = d.mon; }
        else if (s.sourceKind == "window") { o.window = resolveSavedWindow(W(s.sourceExe), W(s.sourceTitle)); if (!o.window) o.allDisplays = true; }
    }
    std::wstring patternPath;
    bool cliWarmup = false;
    std::string warmInfo;
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
        if (a == L"--pattern") { o.forcePattern = true; continue; }
        if (a == L"--system-audio") { o.systemAudio = true; continue; }
        if (a == L"--mic") { o.mic = true; continue; }
        if (a == L"--test-tone") { o.testTone = true; continue; }
        if (a == L"--test-mic") { o.testMic = true; continue; }
        if (a == L"--warmup") { cliWarmup = true; continue; }
        if (a == L"--list-sources") {
            std::string o2;
            for (auto& d : listDisplays()) { char b[200]; snprintf(b, sizeof b, "monitor:%d\t%s\t%ldx%ld%s\n", d.index, U(d.device).c_str(), d.rect.right - d.rect.left, d.rect.bottom - d.rect.top, d.primary ? "\tprimary" : ""); o2 += b; }
            emit(o2, stdout); return 0;
        }
        if (a == L"--list-windows") {
            std::string o2;
            for (auto& w : listCaptureWindows()) {
                char b[64]; snprintf(b, sizeof b, "0x%llx\t%s\t", (unsigned long long)(uintptr_t)w.hwnd, w.minimized ? "min" : "-");
                o2 += b + U(w.exe) + "\t" + U(w.title) + "\n";
            }
            emit(o2, stdout); return 0;
        }
        if (a == L"--print-settings") { emit(toJson(s), stdout); return 0; }
        if (a == L"--settings-file") { ++i; continue; }
        if (a == L"--minimized") continue;
        if (a != L"--record" && a != L"--fps" && a != L"--encoder" && a != L"--container" && a != L"--quality"
            && a != L"--out" && a != L"--log" && a != L"--scale" && a != L"--gpu" && a != L"--bitrate-cap"
            && a != L"--test-hang-finalize" && a != L"--pause-at" && a != L"--resume-at" && a != L"--render-ui" && a != L"--ui-state" && a != L"--dpi" && a != L"--render-pattern" && a != L"--window"
            && a != L"--source" && a != L"--noise-suppression" && a != L"--noise-gate" && a != L"--mic-volume" && a != L"--channels")
            return bad("unknown option " + U(a));
        if (!value(v)) return bad(U(a) + " needs a value");
        if (a == L"--window") {
            o.window = findCaptureWindow(W(v));
            if (!o.window) return bad("no capturable window matches '" + v + "' (see --list-windows)");
            o.allDisplays = false; o.monitor = nullptr;
        } else if (a == L"--source") {
            o.window = nullptr; o.monitor = nullptr; o.allDisplays = false;
            if (v == "all") o.allDisplays = true;
            else if (v.rfind("monitor", 0) == 0) {
                int n = v.size() > 8 && v[7] == ':' ? atoi(v.c_str() + 8) : 0;
                if (n) { if (n > (int)g_displays.size()) return bad("--source monitor:N: there are " + std::to_string(g_displays.size()) + " displays"); o.monitor = g_displays[(size_t)n - 1].mon; }
            } else if (v.rfind("window:", 0) == 0) {
                o.window = findCaptureWindow(W(v.substr(7)));
                if (!o.window) return bad("no capturable window matches '" + v.substr(7) + "' (see --list-windows)");
            } else return bad("--source must be all, monitor[:N] or window:TEXT");
        } else if (a == L"--noise-suppression") {
            if (v != "on" && v != "off") return bad("--noise-suppression must be on or off");
            audioFx().noiseSuppression = v == "on";
        } else if (a == L"--noise-gate") {
            char* end = nullptr; double d = strtod(v.c_str(), &end);
            if (v.empty() || !end || *end || d < -80 || d > 0) return bad("--noise-gate must be -80..0 dB");
            audioFx().gateDb = (float)d;
        } else if (a == L"--mic-volume") {
            char* end = nullptr; long d = strtol(v.c_str(), &end, 10);
            if (v.empty() || !end || *end || d < 0 || d > 200) return bad("--mic-volume must be 0..200");
            audioFx().micGain = d / 100.f;
        } else if (a == L"--channels") {
            if (v != "1" && v != "2") return bad("--channels must be 1 or 2");
            o.channels = atoi(v.c_str());
        } else if (a == L"--record") {
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
        } else if (a == L"--render-pattern") {
            if (v.empty()) return bad("--render-pattern needs a .bmp path");
            patternPath = W(v);
        } else if (a == L"--render-ui") {
            if (v.empty()) return bad("--render-ui needs a .bmp path");
            renderPath = W(v);
        } else if (a == L"--ui-state") {
            if (v != "idle" && v != "dropdown" && v != "source" && v != "settings" && v != "recording" && v != "micpop") return bad("--ui-state must be idle, source, settings, recording or micpop");
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
    if (!patternPath.empty()) {
        const unsigned pw = 1280, ph = 720;
        std::vector<uint32_t> px; testPattern(px, pw, ph);
        BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
        ih.biSize = sizeof ih; ih.biWidth = pw; ih.biHeight = -(LONG)ph; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + pw * ph * 4;
        FILE* f = _wfopen(patternPath.c_str(), L"wb");
        bool ok = f != nullptr;
        if (ok && patternPath.size() > 4 && _wcsicmp(patternPath.c_str() + patternPath.size() - 4, L".yuv") == 0) {
            // Ideal I420 reference: exact float BT.709 limited range, 2x2 box chroma (independent of our converter).
            std::vector<uint8_t> yuv((size_t)pw * ph * 3 / 2);
            auto ch = [&](unsigned x, unsigned y, int k) { return (double)((px[(size_t)y * pw + x] >> (16 - 8 * k)) & 255); };  // k 0=R 1=G 2=B
            auto c8 = [](double v) { v = v < 0 ? 0 : v > 255 ? 255 : v; return (uint8_t)(v + 0.5); };
            for (unsigned y = 0; y < ph; ++y)
                for (unsigned x = 0; x < pw; ++x)
                    yuv[(size_t)y * pw + x] = c8(16 + (0.2126 * ch(x, y, 0) + 0.7152 * ch(x, y, 1) + 0.0722 * ch(x, y, 2)) * 219 / 255);
            uint8_t* U = yuv.data() + (size_t)pw * ph; uint8_t* V = U + (size_t)pw * ph / 4;
            for (unsigned y = 0; y < ph / 2; ++y)
                for (unsigned x = 0; x < pw / 2; ++x) {
                    double r = 0, g2 = 0, b = 0;
                    for (int k = 0; k < 4; ++k) { unsigned xx = 2 * x + (k & 1), yy = 2 * y + (k >> 1); r += ch(xx, yy, 0); g2 += ch(xx, yy, 1); b += ch(xx, yy, 2); }
                    r /= 4; g2 /= 4; b /= 4;
                    double yy = 0.2126 * r + 0.7152 * g2 + 0.0722 * b;
                    U[(size_t)y * (pw / 2) + x] = c8(128 + (b - yy) / 1.8556 * 224 / 255);
                    V[(size_t)y * (pw / 2) + x] = c8(128 + (r - yy) / 1.5748 * 224 / 255);
                }
            ok = fwrite(yuv.data(), 1, yuv.size(), f) == yuv.size();
        } else
            ok = ok && fwrite(&fh, sizeof fh, 1, f) == 1 && fwrite(&ih, sizeof ih, 1, f) == 1 && fwrite(px.data(), 4, px.size(), f) == px.size();
        if (f) fclose(f);
        emit(ok ? "ok=1 render-pattern\n" : "ok=0 render-pattern\n", stdout);
        return ok ? 0 : 1;
    }
    if (!renderPath.empty()) {
        g.inst = GetModuleHandleW(nullptr);
        gp::GdiplusStartupInput gsi; ULONG_PTR gpTok = 0;
        gp::GdiplusStartup(&gpTok, &gsi, nullptr);
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
    if (cliWarmup) {  // what the app does at launch: fill the probe cache, load MF/AAC
        Recorder w; RecordOptions wo = o; wo.warmupOnly = true; wo.systemAudio = wo.mic = wo.testTone = false;
        wo.outputPath = o.outputPath + L".warmup" + W(extension(o.container));
        w.start(wo); w.waitUntilIdle(30000);
        warmInfo = "warmup=" + U(w.result().latencyLog) + "\n";
    }
    Recorder r;
    { LARGE_INTEGER q; QueryPerformanceCounter(&q); o.pressQpc = q.QuadPart; }
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
    std::string extra = warmInfo + "state=" + std::string(toString(r.state())) + "\n";
    if (pausedOnce) { char b[96]; snprintf(b, sizeof b, "paused_ms=%llu pauses=%llu\n", pausedWall, res.pauses); extra += b; }
    if (res.abandoned) {  // same process: MF is poisoned, a second session must be refused
        bool again = r.start(o);
        extra += again ? "second_start=accepted\n" : "second_start=refused\n";
        if (again) { r.stop(); r.waitUntilIdle(5000); }
    }
    char summary[4096];
    snprintf(summary, sizeof summary,
             "ok=%d frames=%llu duplicated=%llu late=%llu dropped=%llu abandoned=%d window_closed=%d\ncapture_fps=%.1f encode_fps=%.1f audio=%d\nstart_ms=%.0f\nlatency=%s\ncapture=%s\nencoder=%s\npath=%s\nfile=%s\nmessage=%s\n",
             res.ok ? 1 : 0, res.frames, res.duplicated, res.late, res.dropped, res.abandoned ? 1 : 0, res.windowClosed ? 1 : 0, res.captureFps, res.encodeFps, res.hasAudio ? 1 : 0, res.startMs, U(res.latencyLog).c_str(),
             U(res.captureUsed).c_str(), U(res.encoderUsed).c_str(), U(res.pathLog).c_str(),
             U(res.file).c_str(), U(res.message).c_str());
    emit(std::string(summary) + extra + "encoder_log:\n" + U(res.encoderLog), stdout);
    cr::appLog(L"=== CLI recording ===\n" + W(std::string(summary) + extra) + res.encoderLog);
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
    for (int i = 1; i + 1 < argc; ++i) if (wcscmp(argv[i], L"--settings-file") == 0) g_settingsOverride = argv[i + 1];
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--selftest-settings") == 0) g_selftest = true;
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--selftest-record") == 0) g_selftest = g_selftestRecord = true;
    for (int i = 1; i < argc; ++i)
        // Any option other than --minimized goes to the console CLI, which validates it (never opens the window).
if (!g_selftest && wcscmp(argv[i], L"--minimized") != 0 && (argv[i][0] == L'-' || wcscmp(argv[i], L"/?") == 0)) {
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
    cr::installCrashLogger();
    g.settingsFile = settingsPath();
    {
        bool exists = GetFileAttributesW(g.settingsFile.c_str()) != INVALID_FILE_ATTRIBUTES;
        bool ok = loadFile(U(g.settingsFile), g.s);
        g_lastSaved = g.s;
        cr::appLog(L"start " + std::wstring(GetCommandLineW()) + L"\nsettings " + (ok ? L"loaded" : exists ? L"INVALID (defaults used)" : L"missing (defaults)") +
                   L" from " + g.settingsFile + L": " + settingsSummary(g.s));
    }
    g.s.fps = normalizeFps(g.s.fps);
    gp::GdiplusStartupInput gsi; ULONG_PTR gpTok = 0;
    gp::GdiplusStartup(&gpTok, &gsi, nullptr);
    loadIcons();
    loadUiIcons();
    enumGpus();
    g.mics = listMicrophones();  // so a saved microphone shows (and is used) right away
    g.outs = listOutputDevices();
    g.micMuted = g.s.micMuted;
    applyAudioFx();
    syncPtt();
    startPttThread();

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
    startWarmup();  // as early as possible: before the window is even shown
    g.dpi = dpiFor(g.hwnd);
    makeFonts();
    makeUiFonts();
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
    registerAll();
    if (g.s.hotkeysMigrated) {  // one-time notice; saving writes v2 so it won't show again
        g.s.hotkeysMigrated = false;
        save();
        setStatus(L"Hotkeys changed to Ctrl+Shift+F9 / Ctrl+Shift+F10 (Alt+F9/F10 clash with NVIDIA). You can change them below.", C_WARN);
    }
    g.status = L"Ready";
    ShowWindow(g.hwnd, (minimized || g.s.startMinimized) ? SW_SHOWMINNOACTIVE : g.s.maximized ? SW_MAXIMIZE : SW_SHOW);
    if (g_selftestRecord) PostMessageW(g.hwnd, WM_APP + 5, 0, 0);  // CI cold start: press Record as soon as the loop runs
    if (g_selftest && !g_selftestRecord) SetTimer(g.hwnd, TIMER_SELFTEST, 4000, nullptr);
    resolveSource();  // before the preview starts, so it opens on the saved source
    g.gprev.start(g.hwnd, previewRectNow(), g.s.captureCursor);
    if (!IsWindowVisible(g.hwnd)) g.gprev.setVisible(false);
    restartMeter();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    g.rec.stop();
    if (!g.rec.waitUntilIdle(3000)) g.rec.abort();
    g.rec.waitUntilIdle(1000);
    g.warm.waitUntilIdle(3000);
    if (!g.warmLogged && !g.warm.running() && g.warm.result().frames == 0 && !g.warm.result().latencyLog.empty())
        writeSessionLog(g.warm.result(), L"warm-up (encoder probe cache)");
    stopMeter();
    g_pttQuit = true;
    if (g_pttTh.joinable()) g_pttTh.join();
    cr::appLog(L"exit");
    if (mtx) CloseHandle(mtx);
    CoUninitialize();
    LocalFree(argv);
    return 0;
}
