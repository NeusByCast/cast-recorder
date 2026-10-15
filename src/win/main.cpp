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
#include "hwenc.h"
#include "preview.h"
#include "audio.h"
#include "applog.h"
#include "windows_enum.h"
#include "winlabel.h"
#include "notify_policy.h"
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
bool g_selftestHotkeys = false;  // --selftest-hotkeys: press every global hotkey with SendInput and check what it did
std::function<void(int)> g_hkTestHook;
void selftestHotkeysStep();
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
    H_TAB_RECS, H_REPLAYPILL, H_TG_REMUX, H_TG_REPLAY, H_DD_REPLAYLEN, H_REPLAYSECS, H_THEME_DARK, H_THEME_LIGHT,
    H_ACC0, H_ACC5 = H_ACC0 + 5, H_HK3, H_DD_FNAME, H_TG_TRACKS, H_RSEARCH, H_TG_AUTOREMUX, H_REMUXALL, H_CTXMENU,
    H_RLIST, H_TOAST_OPEN, H_TOAST_FOLDER, H_TOAST_CLOSE, R_BASE = 3000,
    P_BASE = 1000, P_SEARCH = 2000, P_MUTE, P_SL_VOL, P_PAD
};
bool g_renderOnly = false;  // --render-ui: no windows, no animations

const int kFps[] = {15, 30, 60};
const wchar_t* kFpsL[] = {L"15 FPS", L"30 FPS", L"60 FPS"};
const Quality kQual[] = {Quality::Low, Quality::Medium, Quality::High, Quality::Ultra};
const wchar_t* kQualL[] = {L"Low", L"Medium", L"High", L"Ultra"};
const Encoder kEnc[] = {Encoder::Auto, Encoder::NVENC, Encoder::AMF, Encoder::QSV, Encoder::Software};
const OutputScale kRes[] = {OutputScale::Native, OutputScale::P1440, OutputScale::P1080, OutputScale::P720};
const wchar_t* kResL[] = {L"Native", L"1440p", L"1080p", L"720p"};
const Container kCont[] = {Container::MKV, Container::MP4};
const wchar_t* kContL[] = {L"MKV", L"MP4"};
const int kReplayLen[] = {15, 30, 60, 120, 300, 600, 900};
const wchar_t* kReplayLenL[] = {L"15 s", L"30 s", L"1 min", L"2 min", L"5 min", L"10 min", L"15 min"};
const char* kFname[] = {"cast_date_time", "date_time", "source_date_time", "cast_counter"};
const wchar_t* kFnameL[] = {L"cast_date_time", L"date_time", L"source_date_time", L"cast_counter"};

enum { TK_REPLAY, TK_STARTED, TK_STOPPED, TK_SAVED, TK_ERROR, TK_TEST };  // toast kinds
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr int HK_TOGGLE = 1, HK_FOLDER = 2, HK_PAUSE = 3, HK_MIC = 4, HK_REPLAY = 5;
constexpr UINT TIMER_TICK = 1, TIMER_PREVIEW = 2, TIMER_SELFTEST = 3, TIMER_FRAME = 6, TIMER_TOAST = 7, TIMER_RESCAN = 8, TIMER_HKTEST = 9, TIMER_REPLAYRESTART = 10;
constexpr UINT WM_APP_REPLAY_DONE = WM_APP + 45, WM_APP_REPLAY_SAVED = WM_APP + 46, WM_APP_REMUX_DONE = WM_APP + 48,
               WM_APP_META = WM_APP + 50, WM_APP_DIRCHANGE = WM_APP + 51, WM_APP_AUDIOERR = WM_APP + 52;
constexpr int BASE_W = 1100, BASE_H = 700, MIN_W = 960, MIN_H = 620;  // v2 SPEC: Window
// UI watchdog > worst-case teardown: capture Close 2 s + Finalize (8000/2) + 3 s margin. It only covers a stuck
// Finalize: MKV conversion / remux run later in the background file queue, never under this watchdog, and the
// recording (fragmented MP4) is never deleted by it.
constexpr ULONGLONG kWatchdogMs = 2000 + 8000 / 2 + 3000;

struct App {
    HINSTANCE inst{};
    HWND hwnd{};
    UINT dpi = 96;
    HFONT fTitle{}, fBody{}, fLabel{}, fSmall{}, fSection{}, fBtn{}, fMono{}, fTimer{};
    HICON icoBig{}, icoSmall{}, icoTray{}, icoTrayRec{};
    Settings s;
    std::wstring settingsFile;
    Recorder rec;
    Recorder replay;                     // replay-buffer-only session (no file) while not recording
    std::shared_ptr<ReplayRing> ring;    // shared by the buffer session and recordings
    bool replaySaving = false; double replaySaveT = 0;
    int hover = H_NONE, pressed = H_NONE;
    int capturing = -1;         // hotkey field being edited (0 start/stop, 1 pause), -1 none
    UINT liveMods = 0;
    std::wstring hkMsg[5];
    std::vector<AudioDevice> outs;  // output devices for system audio (index+1 in the dropdown; 0 = default)
    CaptureTarget target;
    COLORREF hkMsgColor[5]{};
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
// Hotkey slots: 0 Start/Stop, 1 Pause, 2 Mic mute, 3 Push-to-talk (polled, not registered), 4 Save replay.
constexpr int kHkSlots = 5;
const wchar_t* kHkNames[kHkSlots] = {L"Start / Stop", L"Pause", L"Mic mute", L"Push-to-talk", L"Save replay"};
Hotkey* hkSlot(int i) {
    switch (i) { case 0: return &g.s.toggleRecord; case 1: return &g.s.pauseToggle; case 2: return &g.s.micMute; case 3: return &g.s.pttKey;
                 default: return &g.s.saveReplay; }
}
const int kHkIds[kHkSlots] = {HK_TOGGLE, HK_PAUSE, HK_MIC, 0, HK_REPLAY};
void unregisterAll() { for (int id : {HK_TOGGLE, HK_FOLDER, HK_PAUSE, HK_MIC, HK_REPLAY}) UnregisterHotKey(g.hwnd, id); }
void registerAll() {
    if (g.rec.running()) return;  // the live session keeps its hotkeys; never unregister mid-recording
    unregisterAll();
    for (int i = 0; i < kHkSlots; ++i) {
        const Hotkey& k = *hkSlot(i);
        g.hkMsg[i].clear();
        if (k.empty()) { g.hkMsgColor[i] = C_MUTED; continue; }
        int clash = -1;
        for (int j = 0; j < i; ++j) if (*hkSlot(j) == k) { clash = j; break; }
        if (clash >= 0) { g.hkMsg[i] = std::wstring(L"Already used by ") + kHkNames[clash]; g.hkMsgColor[i] = C_ERR; continue; }
        if (i == 3) { g.hkMsgColor[i] = C_OK; continue; }  // push-to-talk is polled: any held combo works
        Issue is = validate(k);
        if (is.severity == Severity::Error) { g.hkMsg[i] = W(is.message); g.hkMsgColor[i] = C_ERR; continue; }
        if (!RegisterHotKey(g.hwnd, kHkIds[i], k.mods | MOD_NOREPEAT, k.vk)) { g.hkMsg[i] = L"Already used by another app"; g.hkMsgColor[i] = C_ERR; continue; }
        if (is.severity == Severity::Warning) { g.hkMsg[i] = W(is.message); g.hkMsgColor[i] = C_WARN; }
        else g.hkMsgColor[i] = C_OK;
    }
    // Open-folder combo has no field in the layout but stays active if set and not clashing.
    bool clash = false;
    for (int i = 0; i < kHkSlots; ++i) if (*hkSlot(i) == g.s.openFolder) clash = true;
    if (!g.s.openFolder.empty() && !clash && validate(g.s.openFolder).severity != Severity::Error)
        RegisterHotKey(g.hwnd, HK_FOLDER, g.s.openFolder.mods | MOD_NOREPEAT, g.s.openFolder.vk);
    std::wstring st;
    for (int i : {0, 1, 2, 4}) st += std::wstring(kHkNames[i]) + L"=" + (hkSlot(i)->empty() ? L"-" : W(format(*hkSlot(i)))) + (g.hkMsg[i].empty() ? L"" : L" (" + g.hkMsg[i] + L")") + L"; ";
    cr::appLog(L"hotkeys: " + st);
}

void setStatus(const std::wstring& s, COLORREF c);
std::wstring redactTitle(const std::wstring& t);
std::wstring settingsSummary(const Settings& s0) {
    Settings s = s0; s.sourceTitle = U(redactTitle(W(s.sourceTitle)));  // never log window titles
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
// Startup: move journaled leftovers of a killed remux / conversion to the Recycle Bin. Only files this app created
// (listed in %APPDATA%\\Cast Recorder\\temp-files.journal, named *.castrec-tmp*) - never user files.
std::wstring outputDir();
void sweepPartFiles() {
    std::wstring log;
    int n = sweepTempJournal(outputDir(), log);
    if (n) cr::appLog(log + L"startup: recycled " + std::to_wstring(n) + L" leftover temp file(s)");
}
std::wstring redactTitle(const std::wstring& t);
void save() {
    if (g.hwnd) g.s.maximized = IsZoomed(g.hwnd) != 0;
    if (saveFile(U(g.settingsFile), g.s)) {
        // Redact the title in the Settings copies before anything is formatted: no splitting of logged text.
        Settings a = g_lastSaved, b = g.s;
        a.sourceTitle = U(redactTitle(W(a.sourceTitle))); b.sourceTitle = U(redactTitle(W(b.sourceTitle)));
        if (a.sourceTitle == b.sourceTitle && g_lastSaved.sourceTitle != g.s.sourceTitle) b.sourceTitle += " (changed)";
        std::wstring d = settingsDiff(a, b);
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

// Our own tones (res/sounds, scripts/make-sounds.py): soft filtered-noise glides at -34 dBFS (tick -38, shutter -36).
// Length of a WAVE resource in ms (PCM header), 600 if unknown.
static double toneMs(int id) {
    HRSRC r = FindResourceW(g.inst, MAKEINTRESOURCEW(id), L"WAVE");
    if (!r) return 600;
    DWORD sz = SizeofResource(g.inst, r);
    const BYTE* p = (const BYTE*)LockResource(LoadResource(g.inst, r));
    if (!p || sz < 44) return 600;
    DWORD byteRate = *(const DWORD*)(p + 28);
    return byteRate ? (sz - 44) * 1000.0 / byteRate : 600;
}
void playTone(int id) {
    LARGE_INTEGER a, b, f; QueryPerformanceCounter(&a);
    {   // keep it out of the mic (and of system audio without process-loopback exclusion): output latency + tail
        LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
        long long now100 = (long long)(a.QuadPart / qf.QuadPart * 10000000LL + a.QuadPart % qf.QuadPart * 10000000LL / qf.QuadPart);
        audioFx().markOwnSound(now100 - 200000LL, now100 + (long long)((toneMs(id) + 350.0) * 10000.0));
    }
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
    o.codec = g.s.codec; o.autoRemux = g.s.autoRemux; o.separateTracks = g.s.separateTracks;
    o.allDisplays = g.target.kind == CaptureTarget::AllDisplays;
    o.monitor = g.target.kind == CaptureTarget::Monitor ? g.target.mon : nullptr;
    o.window = g.target.kind == CaptureTarget::Window ? g.target.hwnd : nullptr;
    return o;
}
// Background warm-up (startup and when encoder-relevant settings change): same device/capture/encoder
// selection as a real recording, but exits before the loop. Probe results are cached process-wide.
// What the replay buffer session depends on: a change to anything else (theme, hotkeys, file names,
// output folder, ...) must not throw the buffered seconds away.
std::wstring replaySig() {
    RecordOptions o = optionsFromSettings();
    wchar_t b[256];
    swprintf(b, 256, L"%d|%d|%d|%d|%d|%d|%u|%d|%d|%d|%d|%d|%p|%p|%d|%d", o.fps, (int)o.quality, (int)o.encoder, (int)o.codec, o.gpuIndex, (int)o.scale,
             o.bitrateCap, o.systemAudio ? 1 : 0, o.mic ? 1 : 0, o.channels, o.separateTracks ? 1 : 0, o.allDisplays ? 1 : 0, (void*)o.monitor, (void*)o.window,
             o.captureCursor ? 1 : 0, o.codec == Codec::Auto ? (int)o.container * 2 + (o.autoRemux ? 1 : 0) : -1);
    return std::wstring(b) + L"|" + o.micId + L"|" + o.systemId;
}
std::wstring g_replaySig;  // signature the running buffer session was started with
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

void stopReplayBuffer();
void queuePost(const cr::PostJob& pj); bool remuxBusy();
void forceQuit() {
    g.quitting = true;
    stopReplayBuffer();
    for (int i = 0; i < 300 && g.replaySaving; ++i) {  // let a replay being written finish
        MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    }
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
    if (g.recPending) {
        g.recPending = false;
        const RecordResult& r = g.rec.result();
        writeSessionLog(r, L"recording (saved on exit)");
        if (r.ok && r.post.needed()) queuePost(r.post);
    }
    DestroyWindow(g.hwnd);
}

std::wstring nextFileName(const std::wstring& dir);
void stopReplayBuffer();
void startReplayBuffer();
void ensureRing();
void startRecording() {
    if (g.rec.running()) return;
    if (g.rec.mfAbandoned()) { setStatus(L"Restart Cast Recorder to record again (encoder got stuck)", C_WARN); return; }
    std::wstring dir = outputDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) { setStatus(L"Can't create the save folder", C_ERR); return; }
    std::wstring name = nextFileName(dir);
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
    stopReplayBuffer();  // the recording session takes over feeding the same ring (one capture + encoder)
    if (g.s.replayEnabled) { ensureRing(); o.ring = g.ring; }
    if (!g.rec.start(o)) { setStatus(L"Already recording", C_WARN); startReplayBuffer(); return; }
    g.recPending = true;
    g.gprev.setRecording(true);
    g.rec.setMicMuted(g.micMuted);
    // start tone plays on WM_APP_RECORDER_STARTED (first captured frame), never blocking the start path
    SetTimer(g.hwnd, TIMER_TICK, 250, nullptr);
    setStatus(L"Recording\u2026", C_ACCENT_B);
    trayUpdate(NIM_MODIFY);
}
struct Toast; Toast* showToast(int kind, const std::wstring& title, const std::wstring& sub, const std::wstring& path, bool inApp, bool testVisible);
void notice(int kind, const std::wstring& title, const std::wstring& sub, const std::wstring& path);
void stopRecording() {
    if (!g.rec.isActive()) return;
    playTone(IDR_SND_STOP);
    g.rec.stop();
    // no "stopped" toast: the "Recording saved" / "Recording failed" one follows a moment later (one toast per stop)
    cr::appLog(L"toast: \"Recording stopped\" folded into the saved/failed notice");
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

// v1.1 themes (tokens.json themes.dark/light) + accents. Mutable: applyTheme() swaps the palette.
unsigned T_BG, T_SURF, T_ELEV, T_PREV, T_INPUT, T_INPUT_H, T_TEXT, T_TEXT2, T_MUTED, T_DIS, T_ACC, T_ACC_B, T_ACC_H, T_ACC_P,
         T_LISTH, T_TRACK, T_SEGBG, T_SEGTH;
const unsigned T_OK = 0x22C55E, T_WARN = 0xF59E0B, T_ERR = 0xEF4444, T_CLOSEH = 0xE81123;
unsigned g_borderBase = 0xFFFFFF;
bool g_light = false;
struct AccentDef { const char* name; unsigned base, bright, hover, pressed; };
const AccentDef kAccents[6] = {{"red", 0xE5243B, 0xFF6B6B, 0xF0354A, 0xC41E33}, {"orange", 0xF97316, 0xFB923C, 0xFA8231, 0xD9620F},
                               {"green", 0x22C55E, 0x4ADE80, 0x2FD06A, 0x1AA04B}, {"blue", 0x3B82F6, 0x60A5FA, 0x4F8FF7, 0x2563EB},
                               {"purple", 0x8B5CF6, 0xA78BFA, 0x9A6FF7, 0x7C3AED}, {"pink", 0xEC4899, 0xF472B6, 0xEF5AA5, 0xDB2777}};
bool systemLight() {
    DWORD v = 0, n = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &v, &n) != ERROR_SUCCESS) return false;
    return v != 0;
}
int accentIndex() { for (int i = 0; i < 6; ++i) if (g.s.accent == kAccents[i].name) return i; return 0; }
void applyTheme() {
    g_light = g.s.theme == "light" || (g.s.theme == "auto" && systemLight());
    if (g_light) {
        T_BG = 0xF4F5F7; T_SURF = 0xFFFFFF; T_ELEV = 0xEEF0F3; T_INPUT = 0xFFFFFF; T_INPUT_H = 0xE8EBEF; T_LISTH = 0xE8EBEF; T_PREV = 0xE3E6EB;
        T_TEXT = 0x14161A; T_TEXT2 = 0x4B5260; T_MUTED = 0x8A919E; T_DIS = 0xB4BAC4; T_TRACK = 0xD5D9E0; T_SEGBG = 0xE8EBEF; T_SEGTH = 0xFFFFFF;
        g_borderBase = 0x0B0D12;
    } else {
        T_BG = 0x0E1014; T_SURF = 0x161A20; T_ELEV = 0x1C2128; T_INPUT = 0x1A1F27; T_INPUT_H = 0x232A34; T_LISTH = 0x232A34; T_PREV = 0x0A0C0F;
        T_TEXT = 0xF2F3F5; T_TEXT2 = 0xA8ADB7; T_MUTED = 0x6B7280; T_DIS = 0x4B5563; T_TRACK = 0x2A303A; T_SEGBG = 0x0E1014; T_SEGTH = 0x161A20;
        g_borderBase = 0xFFFFFF;
    }
    const AccentDef& a = kAccents[accentIndex()];
    T_ACC = a.base; T_ACC_B = g_light ? a.pressed : a.bright; T_ACC_H = a.hover; T_ACC_P = a.pressed;
    if (g.hwnd) { BOOL dark = !g_light; DwmSetWindowAttribute(g.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark); }
}
gp::Color C(unsigned rgb, int a = 255) { return gp::Color((BYTE)a, (BYTE)((rgb >> 16) & 255), (BYTE)((rgb >> 8) & 255), (BYTE)(rgb & 255)); }
gp::Color WA(int a) { return C(g_borderBase, a); }  // borderBase at an alpha: borders and hover overlays in both themes
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
float txtW(gp::Graphics& gr, const std::wstring& s, gp::Font* f) {
    gp::StringFormat sf(gp::StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | gp::StringFormatFlagsMeasureTrailingSpaces);
    RF out; gr.MeasureString(s.c_str(), (INT)s.size(), f, gp::PointF(0, 0), &sf, &out);
    return out.Width;
}
// Text that does not fit gets an explicit "..." (end) or a middle ellipsis (paths: drive and last folder stay
// visible). Measured with the same typographic format as drawing, so it never clips mid-glyph at any DPI.
std::wstring fitText(gp::Graphics& gr, const std::wstring& s, gp::Font* f, float maxW, bool path) {
    if (s.empty() || txtW(gr, s, f) <= maxW + 0.25f) return s;
    const std::wstring el = L"\u2026";
    if (maxW <= txtW(gr, el, f)) return std::wstring();
    auto make = [&](size_t n) {
        if (!path) { std::wstring h = s.substr(0, n); while (!h.empty() && h.back() == L' ') h.pop_back(); return h + el; }
        size_t head = std::min(n, std::max<size_t>(3, n * 2 / 5)), tail = n - head;
        return s.substr(0, head) + el + s.substr(s.size() - tail);
    };
    size_t lo = 0, hi = s.size() - 1;  // largest n that fits
    while (lo < hi) { size_t mid = (lo + hi + 1) / 2; if (txtW(gr, make(mid), f) <= maxW + 0.25f) lo = mid; else hi = mid - 1; }
    return make(lo);
}
// Truncated text in the main window: hovering it shows the full string (collected while painting).
struct TruncTip { RF r; std::wstring t; };
std::vector<TruncTip> g_truncs;
bool g_collectTruncs = false;
float g_mouseX = -1, g_mouseY = -1;
// --render-ui --ui-long: worst-case strings for the clipping audit
bool g_renderLong = false;
const wchar_t* kLongTitle = L"\u0420\u0435\u0434\u0430\u043a\u0442\u043e\u0440 \u2014 Quarterly planning review (final, really final v7) - Microsoft PowerPoint";
void txt(gp::Graphics& gr, const std::wstring& s, RF r, gp::Font* f, gp::Color c, int align = AL_L, bool ellipsis = true, bool path = false) {
    gp::StringFormat sf(gp::StringFormat::GenericTypographic());
    sf.SetAlignment(align == AL_L ? gp::StringAlignmentNear : align == AL_C ? gp::StringAlignmentCenter : gp::StringAlignmentFar);
    sf.SetLineAlignment(gp::StringAlignmentCenter);
    sf.SetFormatFlags(sf.GetFormatFlags() | gp::StringFormatFlagsNoWrap | gp::StringFormatFlagsMeasureTrailingSpaces);
    sf.SetTrimming(gp::StringTrimmingNone);
    std::wstring t = (ellipsis || path) ? fitText(gr, s, f, r.Width, path) : s;
    gp::SolidBrush b(c);
    gr.DrawString(t.c_str(), (INT)t.size(), f, r, &sf, &b);
    if (t.size() != s.size() && g_collectTruncs) {
        gp::Matrix m; gr.GetTransform(&m); gp::REAL e[6]; m.GetElements(e);
        const float k = scale();
        g_truncs.push_back({RF((r.X * e[0] + e[4]) / k, (r.Y * e[3] + e[5]) / k, r.Width * e[0] / k, r.Height * e[3] / k), s});
    }
}

// ---------- icons (embedded PNGs, white; tinted at draw time) ----------
enum Glyph { G_RECORD, G_PAUSE, G_STOP, G_MIC, G_MICMUTED, G_SYS, G_SYSMUTED, G_FOLDER, G_LOG, G_SETTINGS, G_MINIMIZE, G_MAXIMIZE,
             G_RESTORE, G_CLOSE, G_CHEVRON, G_CHECK, G_MONITOR, G_WINDOW, G_FULLSCREEN, G_CHECKCIRCLE, G_MOON, G_SUN, G_PLAY,
             G_REMUX, G_REPLAY, G_SEARCH, G_COUNT };
// Pre-rendered at every device size the UI draws (scripts/gen_icons.py): drawn 1:1 on the pixel grid, never rescaled.
const int kIconPx[] = {16, 20, 24, 25, 28, 30, 32, 35, 40, 48, 50, 60};
constexpr int kIconSizes = (int)(sizeof kIconPx / sizeof kIconPx[0]);
gp::Bitmap* g_icons[G_COUNT][kIconSizes] = {};
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
        for (int k = 0; k < kIconSizes; ++k) if (!g_icons[i][k]) g_icons[i][k] = loadPngResource(1000 + i * 16 + k);
}
// Draws a glyph at logical (x, y, size). The current transform is resolved to device pixels; the glyph rendered for
// exactly that pixel size is blitted 1:1 at a whole-pixel position (no resampling, no half-pixel blur). Sizes between
// the pre-rendered ones use the next larger bitmap, downscaled with high-quality bicubic filtering.
void icon(gp::Graphics& gr, int glyph, float x, float y, float size, gp::Color tint, float rotateDeg = 0) {
    gp::Matrix m; gr.GetTransform(&m);
    gp::REAL e[6]; m.GetElements(e);  // m11 m12 m21 m22 dx dy (scale + translate only in this UI)
    const float sc = e[0] > 0 ? e[0] : 1.f;
    const int px = std::max(1, (int)std::lround(size * sc));
    int k = 0;
    while (k < kIconSizes - 1 && kIconPx[k] < px) ++k;
    gp::Bitmap* b = g_icons[glyph][k];
    if (!b) return;
    const float X = std::round(x * e[0] + e[4]), Y = std::round(y * e[3] + e[5]);
    gp::ColorMatrix cm = {{{0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, tint.GetA() / 255.f, 0},
                           {tint.GetR() / 255.f, tint.GetG() / 255.f, tint.GetB() / 255.f, 0, 1}}};
    gp::ImageAttributes ia; ia.SetColorMatrix(&cm);
    ia.SetWrapMode(gp::WrapModeTileFlipXY);  // no dark fringe from sampling outside the bitmap
    gp::GraphicsState st = gr.Save();
    gr.ResetTransform();
    if (rotateDeg != 0) { gr.TranslateTransform(X + px / 2.f, Y + px / 2.f); gr.RotateTransform(rotateDeg); gr.TranslateTransform(-(X + px / 2.f), -(Y + px / 2.f)); }
    const bool exact = (int)b->GetWidth() == px && rotateDeg == 0;
    gr.SetInterpolationMode(exact ? gp::InterpolationModeNearestNeighbor : gp::InterpolationModeHighQualityBicubic);
    gr.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    gr.DrawImage(b, gp::RectF(X, Y, (gp::REAL)px, (gp::REAL)px), 0, 0, (gp::REAL)b->GetWidth(), (gp::REAL)b->GetHeight(), gp::UnitPixel, &ia);
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
enum Ease { E_LINEAR, E_OUTEXPO, E_IN, E_STD, E_OUT, E_INOUT, E_BACK };
double ease(int e, double t) {
    switch (e) {
        case E_OUTEXPO: return cubicBezier(0.16, 1, 0.3, 1, t);
        case E_IN: return cubicBezier(0.4, 0, 1, 1, t);
        case E_STD: return cubicBezier(0.2, 0, 0, 1, t);
        case E_OUT: return cubicBezier(0, 0, 0.58, 1, t);
        case E_INOUT: return cubicBezier(0.42, 0, 0.58, 1, t);
        case E_BACK: return cubicBezier(0.34, 1.56, 0.64, 1, t);
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

std::map<int, Anim> g_press;         // press scale: 70 ms to 0.97, 160 ms overshoot back (SPEC micro-animations)
struct Tooltip { RF r; std::wstring t; } g_tooltip;
RF g_pillRect;
struct pressScope {
    gp::Graphics& gr; gp::GraphicsState st;
    pressScope(gp::Graphics& g_, RF r, int id) : gr(g_), st(g_.Save()) {
        auto it = g_press.find(id);
        if (it == g_press.end()) return;
        float v = it->second.value();
        if (std::fabs(v) < 0.001f) return;
        float k = 1 - 0.03f * v, cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
        gr.TranslateTransform(cx, cy); gr.ScaleTransform(k, k); gr.TranslateTransform(-cx, -cy);
    }
    ~pressScope() { gr.Restore(st); }
};
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
struct SrcRow { int kind = 0; std::wstring label, right, app, page; HMONITOR mon = nullptr; HWND hwnd = nullptr; HICON ic = nullptr; std::wstring exe, device; float y = 0, h = 0; };
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
std::wstring g_srcTitle;  // window title (raw; saved to settings, used for file names)
std::wstring g_srcApp, g_srcPage;  // field label: app name, then the page title muted ("Brave · Cast Recorder v1.0 release")
void setSrcWindow(const std::wstring& title, const std::wstring& app, const std::wstring& page) { g_srcTitle = title; g_srcApp = app; g_srcPage = page; }
// Window titles can hold private text (document names, chats, URLs): the log gets a stable hash instead, the
// settings file keeps the real title so the window can still be found again.
std::wstring redactTitle(const std::wstring& t) {  // length only: no content, no hash (a hash of a short title is guessable)
    return L"(" + std::to_wstring(t.size()) + L" chars)";
}
HICON g_srcIcon = nullptr;
std::wstring sourceText() {
    if (g.target.kind == CaptureTarget::Window) return !g_srcApp.empty() ? cr::windowLabel(g_srcApp, g_srcPage) : g_srcTitle.empty() ? L"Window" : g_srcTitle;
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
    setSrcWindow(L"", L"", L""); g_srcIcon = nullptr;
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
            for (auto& w : listCaptureWindows()) if (w.hwnd == h) { setSrcWindow(w.title, w.app, w.page); g_srcIcon = w.icon; }
        } else {
            cr::appLog(L"source: saved window " + W(g.s.sourceExe) + L" " + redactTitle(W(g.s.sourceTitle)) + L" not found, using Full screen");
            setStatus(L"Saved window isn't open \u2013 using Full screen", toRef(T_TEXT2));
        }
    }
    g.gprev.setTarget(g.target);
}
void selectSource(const SrcRow& r) {
    if (r.kind == SR_ALL) { g.target = CaptureTarget{}; g.target.kind = CaptureTarget::AllDisplays; g.s.sourceKind = "all"; setSrcWindow(L"", L"", L""); }
    else if (r.kind == SR_MON) { g.target = CaptureTarget{}; g.target.kind = CaptureTarget::Monitor; g.target.mon = r.mon; g.s.sourceKind = "monitor"; g.s.sourceMonitor = U(r.device); setSrcWindow(L"", L"", L""); }
    else if (r.kind == SR_WIN) {
        g.target = CaptureTarget{}; g.target.kind = CaptureTarget::Window; g.target.hwnd = r.hwnd;
        g.s.sourceKind = "window"; g.s.sourceExe = U(r.exe); g.s.sourceTitle = U(r.label); setSrcWindow(r.label, r.app, r.page); g_srcIcon = r.ic;
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
// Encoder list: Auto + every codec x vendor the selected GPU supports (hwenc probeCodecSupport), "H.264 \u00b7 NVENC".
struct EncChoice { Codec codec; Encoder enc; std::wstring label; };
std::vector<EncChoice> g_encChoices;
Encoder encoderOfVendor(const std::wstring& v) {
    if (v == L"NVIDIA") return Encoder::NVENC; if (v == L"AMD") return Encoder::AMF; if (v == L"Intel") return Encoder::QSV;
    if (v == L"Software") return Encoder::Software; return Encoder::Auto;
}
const wchar_t* vendorLabel(Encoder e) {
    switch (e) { case Encoder::NVENC: return L"NVENC"; case Encoder::AMF: return L"AMF"; case Encoder::QSV: return L"Quick Sync"; case Encoder::Software: return L"Software"; default: return L"Auto"; }
}
const wchar_t* codecLabel(Codec c) { return c == Codec::HEVC ? L"HEVC" : c == Codec::AV1 ? L"AV1" : L"H.264"; }
void probeEncoders() {
    g_encChoices.clear();
    g_encChoices.push_back({Codec::Auto, Encoder::Auto, L"Auto"});
    LUID luid{}; bool haveLuid = false;
    if (g.s.gpuIndex >= 0) {
        IDXGIFactory1* f = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) {
            IDXGIAdapter1* a = nullptr;
            if (f->EnumAdapters1((UINT)g.s.gpuIndex, &a) == S_OK) { DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d); luid = d.AdapterLuid; haveLuid = true; a->Release(); }
            f->Release();
        }
    }
    std::wstring log;
    for (auto& c : probeCodecSupport(haveLuid ? &luid : nullptr)) {
        Codec cc = c.codec == L"HEVC" ? Codec::HEVC : c.codec == L"AV1" ? Codec::AV1 : Codec::H264;
        Encoder e = encoderOfVendor(c.vendor);
        if (e == Encoder::Auto) continue;
        bool dup = false;
        for (auto& x : g_encChoices) if (x.codec == cc && x.enc == e) dup = true;
        if (dup) continue;
        g_encChoices.push_back({cc, e, std::wstring(codecLabel(cc)) + L" \u00b7 " + vendorLabel(e)});
        log += g_encChoices.back().label + L"; ";
    }
    std::stable_sort(g_encChoices.begin() + 1, g_encChoices.end(), [](const EncChoice& a, const EncChoice& b) { return (int)a.codec < (int)b.codec; });
    cr::appLog(L"encoders for " + (haveLuid ? W(g.s.gpuName) : std::wstring(L"Auto GPU")) + L": " + log);
}
int encSel() { for (size_t i = 0; i < g_encChoices.size(); ++i) if (g_encChoices[i].codec == g.s.codec && g_encChoices[i].enc == g.s.encoder) return (int)i; return -1; }
std::wstring encoderText() {
    int i = encSel();
    if (i >= 0) return g_encChoices[i].label;
    if (g.s.codec == Codec::Auto && g.s.encoder == Encoder::Auto) return L"Auto";
    return std::wstring(g.s.codec == Codec::Auto ? L"Auto" : codecLabel(g.s.codec)) + L" \u00b7 " + vendorLabel(g.s.encoder);
}
std::wstring g_ctxPath;  // recording the context menu acts on
const wchar_t* kCtxL[] = {L"Open", L"Show in folder", L"Remux to MP4", L"Rename", L"Delete"};
std::vector<int> replayPresets();
int ddCount(int id) {
    switch (id) {
        case H_DD_FPS: return 3; case H_DD_QUAL: return 4; case H_DD_ENC: return (int)g_encChoices.size();
        case H_DD_GPU: return 1 + (int)g.gpus.size(); case H_DD_RES: return 4; case H_DD_CONT: return 2;
        case H_DD_SYSDEV: return 1 + (int)g.outs.size(); case H_DD_MICDEV: return 1 + (int)g.mics.size();
        case H_DD_REPLAYLEN: return (int)replayPresets().size() + 1; case H_DD_FNAME: return 4; case H_CTXMENU: return 5;
    }
    return 0;
}
// ---- instant replay memory budget ----
void sourceSize(int& w, int& h);
struct ReplayBudget { unsigned long long perSec = 0; size_t cap = 0; int effSecs = 0; };
ReplayBudget replayBudget(int secs) {
    int w = 0, h = 0; sourceSize(w, h);
    if (w <= 0 || h <= 0) { w = 1920; h = 1080; }
    int maxH = g.s.scale == OutputScale::P1440 ? 1440 : g.s.scale == OutputScale::P1080 ? 1080 : g.s.scale == OutputScale::P720 ? 720 : 0;
    if (maxH && h > maxH) { w = (int)((long long)w * maxH / h) & ~1; h = maxH; }
    unsigned br = bitrateFor(g.s.quality, w, h, g.s.fps, g.s.bitrateCap);
    ReplayBudget b;
    b.perSec = ReplayRing::estimateBytes(1, br, g.s.channels, g.s.separateTracks ? 3 : 1, g.s.systemAudio || g.s.micEnabled);
    b.cap = ReplayRing::defaultMaxBytes();
    unsigned long long fit = b.perSec ? b.cap / b.perSec : (unsigned long long)secs;
    b.effSecs = (int)std::min<unsigned long long>((unsigned long long)secs, fit);
    return b;
}
// 15 s .. 5 min always; 10 / 15 min only when they fit in the RAM cap at the current quality
int replayEffSecs(int secs) { return replayBudget(secs).effSecs; }
std::vector<int> replayPresets() {
    std::vector<int> v = {15, 30, 60, 120, 300};
    for (int s : {600, 900}) if (replayBudget(s).effSecs >= s) v.push_back(s);
    return v;
}
std::wstring presetLabel(int s) { for (int i = 0; i < 7; ++i) if (kReplayLen[i] == s) return kReplayLenL[i]; return std::to_wstring(s) + L" s"; }
RF replayLenEditRect();
std::wstring ddItem(int id, int i) {
    switch (id) {
        case H_DD_FPS: return kFpsL[i]; case H_DD_QUAL: return kQualL[i];
        case H_DD_ENC: return i >= 0 && i < (int)g_encChoices.size() ? g_encChoices[i].label : encoderText();
        case H_DD_GPU: return i == 0 ? std::wstring(L"Auto (display's GPU)") : g.gpus[i - 1];
        case H_DD_RES: { if (i) return kResL[i]; int w, h; sourceSize(w, h); return w ? L"Native (" + dims(w, h) + L")" : std::wstring(L"Native"); }
        case H_DD_CONT: return kContL[i];
        case H_DD_SYSDEV: return i == 0 ? std::wstring(L"Default output") : g.outs[i - 1].name;
        case H_DD_MICDEV: return i == 0 ? std::wstring(L"Default microphone") : g.mics[i - 1].name;
        case H_DD_REPLAYLEN: { auto ps = replayPresets(); return i < (int)ps.size() ? presetLabel(ps[i]) : std::wstring(L"Custom (5\u2013600 s)\u2026"); }
        case H_DD_FNAME: return kFnameL[i];
        case H_CTXMENU: return kCtxL[i];
    }
    return L"";
}
int ddSel(int id) {
    switch (id) {
        case H_DD_FPS: return g.s.fps == 15 ? 0 : g.s.fps == 30 ? 1 : 2;
        case H_DD_QUAL: return (int)g.s.quality; case H_DD_ENC: return encSel();
        case H_DD_GPU: return g.s.gpuIndex < 0 || g.s.gpuIndex >= (int)g.gpus.size() ? 0 : g.s.gpuIndex + 1;
        case H_DD_RES: for (int k = 0; k < 4; ++k) if (kRes[k] == g.s.scale) return k; return 0;
        case H_DD_CONT: return g.s.container == Container::MKV ? 0 : 1;
        case H_DD_SYSDEV: for (size_t k = 0; k < g.outs.size(); ++k) if (U(g.outs[k].id) == g.s.systemDevice) return (int)k + 1; return 0;
        case H_DD_MICDEV: for (size_t k = 0; k < g.mics.size(); ++k) if (U(g.mics[k].id) == g.s.micDevice) return (int)k + 1; return 0;
        case H_DD_REPLAYLEN: { auto ps = replayPresets(); for (size_t k = 0; k < ps.size(); ++k) if (ps[k] == g.s.replaySeconds) return (int)k; return (int)ps.size(); }
        case H_DD_FNAME: for (int k = 0; k < 4; ++k) if (g.s.fileNamePattern == kFname[k]) return k; return 0;
        case H_CTXMENU: return -1;
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
void restartReplayBuffer();
void startReplayBuffer();
void beginEdit(int kind, RF r, const std::wstring& text, const std::wstring& path);
void ctxAction(int item, int action);
void applyReplaySeconds(int v) {
    g.s.replaySeconds = v; save();
    if (g.ring) g.ring->setSeconds(v);
    ReplayBudget b = replayBudget(v);
    if (b.effSecs < v) cr::appLog(L"replay: " + std::to_wstring(v) + L" s needs ~" + std::to_wstring(b.perSec * (unsigned long long)v >> 20) + L" MB, capped at " + std::to_wstring(b.cap >> 20) + L" MB -> about " + std::to_wstring(b.effSecs) + L" s");
    InvalidateRect(g.hwnd, nullptr, FALSE);
}
// Encoder-relevant change: the replay buffer (when on) restarts on the new config, otherwise the warm-up re-probes.
void scheduleReplayRestart();
void encoderSettingsChanged() { cr::resetEncoderDecisions(); if (g.s.replayEnabled && !g.rec.running()) scheduleReplayRestart(); else startWarmup(); }
void ddSet(int id, int i) {
    if (id == H_CTXMENU) { ctxAction(0, i); return; }
    switch (id) {
        case H_DD_FPS: g.s.fps = kFps[i]; break; case H_DD_QUAL: g.s.quality = kQual[i]; break;
        case H_DD_ENC: if (i >= 0 && i < (int)g_encChoices.size()) { g.s.codec = g_encChoices[i].codec; g.s.encoder = g_encChoices[i].enc; } break;
        case H_DD_GPU: g.s.gpuIndex = i - 1; g.s.gpuName = i > 0 ? U(g.gpus[i - 1]) : std::string(); probeEncoders(); break;
        case H_DD_RES: g.s.scale = kRes[i]; break; case H_DD_CONT: g.s.container = kCont[i]; break;
        case H_DD_SYSDEV: g.s.systemDevice = i > 0 ? U(g.outs[i - 1].id) : std::string(); break;
        case H_DD_MICDEV: g.s.micDevice = i > 0 ? U(g.mics[i - 1].id) : std::string(); break;
        case H_DD_REPLAYLEN: {
            auto ps = replayPresets();
            if (i < (int)ps.size()) { applyReplaySeconds(ps[i]); return; }
            }
            beginEdit(2, replayLenEditRect(), std::to_wstring(g.s.replaySeconds), L""); return;
        case H_DD_FNAME: g.s.fileNamePattern = kFname[i]; break;
    }
    save();
    if (id == H_DD_SYSDEV || id == H_DD_MICDEV) { restartMeter(); if (g.replay.running()) scheduleReplayRestart(); }
    if (id == H_DD_FPS || id == H_DD_QUAL || id == H_DD_ENC || id == H_DD_GPU || id == H_DD_RES || id == H_DD_CONT) encoderSettingsChanged();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}
// While recording: source, encoder, container and resolution (SPEC: App states), plus the audio devices/routing,
// which only apply to the next recording. Volume, noise suppression/gate, monitoring and PTT apply live.
bool locked(int id) {
    if (!uiBusy()) return false;
    switch (id) {
        case H_SRC: case H_DD_ENC: case H_DD_CONT: case H_DD_RES: case H_DD_GPU: case H_DD_SYSDEV: case H_DD_MICDEV:
        case H_TG_SYS: case H_TG_MIC: case H_CH_MONO: case H_CH_STEREO: case H_HK0: case H_HK1: case H_HK2: case H_HKPTT: case H_HK3:
        case H_BROWSE: case H_TG_REMUX: case H_TG_AUTOREMUX: case H_TG_TRACKS: case H_TG_REPLAY: case H_DD_FNAME:
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
const float kShadow = 26.f;   // logical margin around the card for the shadow (blur 16 + offset 6 + slack)
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
        if (!q.empty() && lowerW(w.title).find(q) == std::wstring::npos && lowerW(w.exe).find(q) == std::wstring::npos && lowerW(w.app).find(q) == std::wstring::npos) continue;
        SrcRow r; r.kind = SR_WIN; r.hwnd = w.hwnd; r.ic = w.icon; r.exe = w.exe; r.label = w.title; r.app = w.app; r.page = w.page;
        add(r, 34, 0); ++n;
    }
    if (g_renderOnly && g_renderLong && q.empty()) {
        SrcRow r; r.kind = SR_WIN; r.label = kLongTitle; r.exe = L"POWERPNT.EXE"; r.app = L"PowerPoint"; r.page = cr::stripAppSuffix(kLongTitle, r.exe, L"Microsoft PowerPoint"); add(r, 34, 0); ++n;
        SrcRow b; b.kind = SR_WIN; b.hwnd = (HWND)(intptr_t)2; b.label = L"Cast Recorder v1.0 release - Brave"; b.exe = L"brave.exe"; b.app = L"Brave"; b.page = L"Cast Recorder v1.0 release"; add(b, 34, 0); ++n;
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
    if (P.kind == PK_LIST) { P.w = P.dd == H_CTXMENU ? 180.f : P.anchor.Width; P.contentH = 8.f + ddCount(P.dd) * 32.f; }
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
// "Brave · page title": app name in the primary color (never squeezed below its width when it fits), the page part
// muted and ellipsized into what is left.
void appTitle(gp::Graphics& gr, const std::wstring& app, const std::wstring& page, RF r, gp::Font* f, gp::Color main, gp::Color muted) {
    if (page.empty()) { txt(gr, app, r, f, main); return; }
    float aw = std::min(txtW(gr, app, f) + 1, r.Width * 0.6f);
    txt(gr, app, RF(r.X, r.Y, aw, r.Height), f, main);
    float x = r.X + aw + 5;
    txt(gr, L"\u00B7  " + page, RF(x, r.Y, std::max(0.f, r.X + r.Width - x), r.Height), f, muted);
}
void ddField(gp::Graphics& gr, RF r, int id, const std::wstring& t, int glyph = -1, HICON wicon = nullptr, const std::wstring* page = nullptr) {
    bool dis = locked(id), open = popupOpenFor(id);
    float hv = dis ? 0.f : hovA(id);
    fillRR(gr, r, 6, mix(T_INPUT, T_INPUT_H, open ? 1.f : hv));
    strokeRR(gr, r, 6, open || (g_kbUsed && g_focus == id) ? C(T_ACC) : WA(A_DEFAULT));
    float x = r.X + 12, cy = r.Y + r.Height / 2;
    if (wicon) { drawWinIcon(gr, wicon, x, cy - 8, 16); x += 26; }
    else if (glyph >= 0) { icon(gr, glyph, x, cy - 8, 16, C(dis ? T_DIS : T_TEXT2)); x += 26; }
    if (page) appTitle(gr, t, *page, RF(x, r.Y, r.X + r.Width - 36 - x, r.Height), F.body, C(dis ? T_DIS : T_TEXT), C(dis ? T_DIS : T_MUTED));
    else txt(gr, t, RF(x, r.Y, r.X + r.Width - 36 - x, r.Height), F.body, C(dis ? T_DIS : T_TEXT));
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
    if (g_light) { gp::Pen kp(WA(A_STRONG), 1.f); gr.DrawEllipse(&kp, kx - 7, cy - 7, 14.f, 14.f); }  // white knob on a white card
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
const int kTabIds[3] = {H_TAB_REC, H_TAB_RECS, H_TAB_SET};
RF tabRect(int i) { const float tx[3] = {150, 222, 322}, tw[3] = {68, 96, 80}; return RF(tx[i], 7, tw[i], 26); }
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
    const wchar_t* tabs[3] = {L"Record", L"Recordings", L"Settings"};
    for (int i = 0; i < 3; ++i) {
        RF r = tabRect(i);
        int id = kTabIds[i];
        bool act = g_viewIdx == i;
        pressScope ps(gr, r, id);
        if (act) fillRR(gr, r, 7, C(T_ELEV));
        else if (hovA(id) > 0) fillRR(gr, r, 7, WA((int)(A_HOVER * hovA(id))));
        txt(gr, tabs[i], r, act ? F.btn : F.body, C(act ? T_TEXT : (hovA(id) > 0.5f ? T_TEXT2 : T_MUTED)), AL_C, false);
        focusRing(gr, r, 7, id);
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

// ---------- v1.1 actions: file names, replay buffer ----------
std::wstring sourceNameForFile() {
    if (g.target.kind == CaptureTarget::Window) return g_srcTitle.empty() ? L"Window" : g_srcTitle;
    if (g.target.kind == CaptureTarget::Monitor) { const DisplayInfo* d = displayFor(g.target.mon); return d ? L"Monitor " + std::to_wstring(d->index) : L"Monitor"; }
    return L"Screen";
}
bool anyExists(const std::wstring& base) {
    for (const wchar_t* e : {L".mp4", L".mkv", L".mov"}) if (GetFileAttributesW((base + e).c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    return false;
}
std::wstring nextFileName(const std::wstring& dir) {
    SYSTEMTIME t; GetLocalTime(&t);
    int counter = 1;
    if (g.s.fileNamePattern == "cast_counter") {
        WIN32_FIND_DATAW fd{};
        HANDLE f = FindFirstFileW((dir + L"\\cast_*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do { int n = 0; if (swscanf(fd.cFileName, L"cast_%d", &n) == 1 && n >= counter) counter = n + 1; } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    std::wstring base = W(fileNameFor(g.s.fileNamePattern, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, U(sourceNameForFile()), counter));
    // source_date_time: the source part may be a window title -> the log shows it as "(N chars)" (saved / remux / recycle lines)
    if (g.s.fileNamePattern == "source_date_time" && base.size() > 22 && g.target.kind == CaptureTarget::Window && !g_srcTitle.empty())
        cr::addLogSecret(base.substr(0, base.size() - 20));
    std::wstring name = base;
    for (int k = 2; anyExists(dir + L"\\" + name) && k < 1000; ++k) name = base + L" (" + std::to_wstring(k) + L")";
    return name;
}
std::wstring stampName(const wchar_t* prefix, const wchar_t* ext, const std::wstring& dir) {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t b[64]; swprintf(b, 64, L"%ls%02d-%02d-%02d", prefix, t.wHour, t.wMinute, t.wSecond);
    std::wstring base = dir + L"\\" + b, p = base + ext;
    for (int k = 2; GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES && k < 1000; ++k) p = base + L" (" + std::to_wstring(k) + L")" + ext;
    return p;
}
std::wstring fileNameOf(const std::wstring& p) { size_t k = p.find_last_of(L"\\/"); return k == std::wstring::npos ? p : p.substr(k + 1); }
std::wstring replayShort(int s) { return s % 60 == 0 && s >= 60 ? std::to_wstring(s / 60) + L"m" : std::to_wstring(s) + L"s"; }
struct ReplayBudget; ReplayBudget replayBudget(int secs); std::vector<int> replayPresets(); int replayEffSecs(int secs);
// Closed-field label: preset name only while it is still a preset; the length the RAM cap really keeps when it is less.
std::wstring replayLenText() {
    const int s = g.s.replaySeconds;
    auto ps = replayPresets();
    bool preset = std::find(ps.begin(), ps.end(), s) != ps.end();
    std::wstring t;
    if (preset) { for (int i = 0; i < 7; ++i) if (kReplayLen[i] == s) t = kReplayLenL[i]; }
    else t = std::to_wstring(s) + L" s (custom)";
    int eff = replayEffSecs(s);
    if (eff > 0 && eff < s) t += L" - keeps " + std::to_wstring(eff) + L" s";
    return t;
}
std::wstring durText(int s) {
    if (s < 60) return std::to_wstring(s) + L" s";
    if (s % 60 == 0) return std::to_wstring(s / 60) + L" min";
    return std::to_wstring(s / 60) + L" min " + std::to_wstring(s % 60) + L" s";
}
std::wstring lastSavedText(double secs) {
    int s = (int)std::lround(secs);
    if (s < 60) return L"Last " + std::to_wstring(s) + (s == 1 ? L" second saved" : L" seconds saved");
    if (s % 60 == 0) return L"Last " + std::to_wstring(s / 60) + (s == 60 ? L" minute saved" : L" minutes saved");
    return L"Last " + std::to_wstring(s / 60) + L" min " + std::to_wstring(s % 60) + L" s saved";
}
void ensureRing() {
    if (!g.ring) g.ring = std::make_shared<ReplayRing>(g.s.replaySeconds);
    else g.ring->setSeconds(g.s.replaySeconds);
    g.ring->setMaxBytes(ReplayRing::defaultMaxBytes());
}
void startReplayBuffer() {
    if (g_renderOnly || !g.hwnd || !g.s.replayEnabled || g.rec.running() || g.replay.running() || g.quitting || g.rec.mfAbandoned()) return;
    if (g.target.kind == CaptureTarget::Window && (!g.target.hwnd || !IsWindow(g.target.hwnd))) return;
    ensureRing();
    RecordOptions o = optionsFromSettings();
    o.bufferOnly = true; o.ring = g.ring; o.notify = g.hwnd; o.doneMsg = WM_APP_REPLAY_DONE;
    wchar_t tp[MAX_PATH]; GetTempPathW(MAX_PATH, tp);
    o.outputPath = std::wstring(tp) + L"castrec-replay" + std::to_wstring(GetCurrentProcessId()) + L".mp4";
    LARGE_INTEGER q; QueryPerformanceCounter(&q); o.pressQpc = q.QuadPart;
    if (g.replay.start(o)) { g_replaySig = replaySig(); cr::appLog(L"replay buffer: started (" + std::to_wstring(g.s.replaySeconds) + L" s, cap " + std::to_wstring(g.ring->maxBytes() >> 20) + L" MB)"); }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}
void stopReplayBuffer() {
    if (!g.replay.running()) return;
    g.replay.stop();
    if (!g.replay.waitUntilIdle(4000)) { g.replay.abort(); g.replay.waitUntilIdle(1000); }
}
void restartReplayBuffer() { stopReplayBuffer(); startReplayBuffer(); }
// Debounced (600 ms) and only if a setting the buffer depends on really changed.
void scheduleReplayRestart() { if (g.hwnd && !g_renderOnly) SetTimer(g.hwnd, TIMER_REPLAYRESTART, 600, nullptr); }
void replayRestartTimer() {
    KillTimer(g.hwnd, TIMER_REPLAYRESTART);
    if (!g.s.replayEnabled || g.rec.running()) return;
    if (!g.replay.running()) { startReplayBuffer(); return; }
    if (replaySig() == g_replaySig) { cr::appLog(L"replay buffer: settings change doesn't affect it, kept running"); return; }
    cr::appLog(L"replay buffer: restarting for new settings");
    restartReplayBuffer();
}
struct ReplaySaved { bool ok; std::wstring path, err; double secs; };
void saveReplay() {
    if (!g.s.replayEnabled) { setStatus(L"Instant replay is off (turn it on in Settings)", toRef(T_TEXT2)); return; }
    if (g.replaySaving) return;
    if (!g.ring || g.ring->bufferedSeconds() < 0.5) { setStatus(L"Replay buffer is still filling", toRef(T_WARN)); return; }
    std::wstring dir = outputDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring path = stampName(L"replay_", L".mp4", dir);
    g.replaySaving = true; g.replaySaveT = nowMs(); kickAnim();
    auto ring = g.ring; HWND h = g.hwnd;
    std::thread([ring, path, h] { cr::guardedRun(L"ui-worker", [&] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto* r = new ReplaySaved{false, path, L"", 0};
        r->ok = ring->save(path, r->secs, r->err);
        if (!PostMessageW(h, WM_APP_REPLAY_SAVED, 0, (LPARAM)r)) delete r;
        if (SUCCEEDED(co)) CoUninitialize();
    }); }).detach();
}
// ---------- inline text field (rename a recording / custom replay length) ----------
struct EditState { int kind = 0; HWND hwnd = nullptr; RF rect; std::wstring path; WNDPROC orig = nullptr; HBRUSH br = nullptr; } g_edit;
void commitEdit(bool ok);
LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) { commitEdit(wp == VK_RETURN); return 0; }
    if (m == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
    if (m == WM_KILLFOCUS) { LRESULT r = CallWindowProcW(g_edit.orig, h, m, wp, lp); PostMessageW(g.hwnd, WM_APP + 49, 0, 0); return r; }
    return CallWindowProcW(g_edit.orig, h, m, wp, lp);
}
void beginEdit(int kind, RF r, const std::wstring& text, const std::wstring& path) {
    if (g_edit.hwnd) commitEdit(false);
    const float s = scale();
    g_edit.kind = kind; g_edit.rect = r; g_edit.path = path;
    DWORD st = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | (kind == 2 ? ES_NUMBER : 0);
    g_edit.hwnd = CreateWindowExW(0, L"EDIT", text.c_str(), st, (int)std::lround(r.X * s), (int)std::lround(r.Y * s), (int)std::lround(r.Width * s), (int)std::lround(r.Height * s),
                                  g.hwnd, nullptr, g.inst, nullptr);
    if (!g_edit.hwnd) { g_edit.kind = 0; return; }
    SendMessageW(g_edit.hwnd, WM_SETFONT, (WPARAM)g.fBody, TRUE);
    SendMessageW(g_edit.hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));
    if (kind == 2) SendMessageW(g_edit.hwnd, EM_SETLIMITTEXT, 3, 0);  // digits only (ES_NUMBER); paste is re-checked strictly
    g_edit.orig = (WNDPROC)SetWindowLongPtrW(g_edit.hwnd, GWLP_WNDPROC, (LONG_PTR)EditProc);
    SetFocus(g_edit.hwnd);
    SendMessageW(g_edit.hwnd, EM_SETSEL, 0, -1);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}
void scanRecordings();
void applyReplaySeconds(int v);
void commitEdit(bool ok) {
    if (!g_edit.hwnd) return;
    HWND e = g_edit.hwnd; g_edit.hwnd = nullptr;
    int kind = g_edit.kind; g_edit.kind = 0;
    wchar_t buf[300] = {}; GetWindowTextW(e, buf, 300);
    std::wstring t = buf;
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    while (!t.empty() && iswspace(t.front())) t.erase(0, 1);
    SetWindowLongPtrW(e, GWLP_WNDPROC, (LONG_PTR)g_edit.orig);
    DestroyWindow(e);
    SetFocus(g.hwnd);
    if (ok && kind == 2 && !t.empty()) {
        int v = 0;
        if (!parseIntStrict(U(t), 5, 600, v)) setStatus(L"Replay length must be a whole number of seconds, 5\u2013600", toRef(T_ERR));
        else applyReplaySeconds(v);
    }
    if (ok && kind == 1 && !t.empty()) {
        const std::wstring& old = g_edit.path;
        size_t sl = old.find_last_of(L'\\'), dot = old.find_last_of(L'.');
        bool badc = t.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos || t.back() == L'.';
        for (wchar_t c : t) if (c < 0x20) badc = true;
        std::wstring nu = old.substr(0, sl + 1) + t + old.substr(dot);
        if (badc) setStatus(L"A file name can't contain < > : \" / \\ | ? *", toRef(T_ERR));
        else if ([&] { std::wstring l = t; for (auto& c : l) c = (wchar_t)towlower(c); return (l.size() >= 5 && l.compare(l.size() - 5, 5, L".part") == 0) || isAppTempName(l); }())
            setStatus(L"Names ending in .part or containing .castrec-tmp are reserved for temporary files", toRef(T_ERR));
        else if (isReservedName(U(t))) setStatus(L"\u201c" + t + L"\u201d is reserved by Windows (CON, PRN, AUX, NUL, COM1\u20139, LPT1\u20139)", toRef(T_ERR));
        else if (old.size() - sl - 1 > 0 && nu.size() >= MAX_PATH) setStatus(L"That name is too long", toRef(T_ERR));
        else if (nu != old && !MoveFileW(old.c_str(), nu.c_str()))
            setStatus(GetLastError() == ERROR_ALREADY_EXISTS ? L"A file with that name already exists" : L"Couldn't rename (the file may be open)", toRef(T_ERR));
        else { cr::appLog(L"recordings: renamed " + old + L" -> " + nu); scanRecordings(); }
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}
// ---------- Recordings: folder scan, metadata/thumbnail worker, folder watcher ----------
struct RecItem {
    std::wstring path, name, ext;     // ext lower-case without the dot
    unsigned long long size = 0; FILETIME mtime{};
    double dur = -1; int w = 0, h = 0; double fps = 0;
    bool replay = false, metaDone = false;
    std::vector<uint32_t> thumbPx; int tw = 0, th = 0;  // filled by the worker
    std::shared_ptr<gp::Bitmap> thumb;                  // made on the UI thread
};
std::vector<RecItem> g_items;
const PROPERTYKEY kPK_Duration = {{0x64440490, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}}, 3};
const PROPERTYKEY kPK_FrameW = {{0x64440491, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}}, 3};
const PROPERTYKEY kPK_FrameH = {{0x64440491, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}}, 4};
const PROPERTYKEY kPK_FrameRate = {{0x64440491, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}}, 6};
std::mutex g_itemsM;
std::wstring g_rsearch; bool g_rsearchFocus = false;
float g_listScroll = 0;
std::atomic<bool> g_metaBusy{false};
std::wstring lowerExt(const std::wstring& n) { size_t d = n.find_last_of(L'.'); std::wstring e = d == std::wstring::npos ? L"" : n.substr(d + 1); return lowerW(e); }
void metaWorker();
void scanRecordings() {
    std::wstring dir = outputDir();
    std::vector<RecItem> found;
    const std::wstring live = g.rec.isActive() ? lowerW(g.rec.currentFile()) : std::wstring();  // the file being recorded
    for (const wchar_t* pat : {L"\\*.mp4", L"\\*.mkv"}) {
        WIN32_FIND_DATAW fd{};
        HANDLE f = FindFirstFileW((dir + pat).c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring n = fd.cFileName, ln = lowerW(n);
            if (isAppTempName(ln)) continue;  // exactly the names rename refuses
            if (!live.empty() && lowerW(dir + L"\\" + n) == live) continue;
            RecItem it;
            it.path = dir + L"\\" + n; it.ext = lowerExt(n);
            it.name = n.substr(0, n.size() - it.ext.size() - 1);
            it.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            it.mtime = fd.ftLastWriteTime;
            it.replay = ln.rfind(L"replay_", 0) == 0;
            found.push_back(std::move(it));
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(found.begin(), found.end(), [](const RecItem& a, const RecItem& b) { return CompareFileTime(&a.mtime, &b.mtime) > 0; });
    {
        std::lock_guard<std::mutex> l(g_itemsM);
        for (auto& n : found)  // keep metadata for unchanged files
            for (auto& o : g_items)
                if (o.path == n.path && o.size == n.size && CompareFileTime(&o.mtime, &n.mtime) == 0) {
                    n.dur = o.dur; n.w = o.w; n.h = o.h; n.fps = o.fps; n.metaDone = o.metaDone; n.thumb = o.thumb;
                    n.thumbPx = o.thumbPx; n.tw = o.tw; n.th = o.th;
                }
        g_items = std::move(found);
    }
    metaWorker();
    if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE);
}
// Duration/size/fps via the shell property system, thumbnail via IShellItemImageFactory (Explorer's own cache).
void metaWorker() {
    if (g_renderOnly || g_metaBusy.exchange(true)) return;
    HWND h = g.hwnd;
    std::thread([h] { cr::guardedRun(L"ui-worker", [&] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (;;) {
            std::wstring path;
            { std::lock_guard<std::mutex> l(g_itemsM); for (auto& it : g_items) if (!it.metaDone) { path = it.path; break; } }
            if (path.empty()) break;
            double dur = -1; int w = 0, hh = 0; double fps = 0;
            std::vector<uint32_t> px; int tw = 0, th = 0;
            IShellItem2* si = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
                ULONGLONG d = 0; ULONG v = 0;
                if (SUCCEEDED(si->GetUInt64(kPK_Duration, &d))) dur = d / 1e7;
                if (SUCCEEDED(si->GetUInt32(kPK_FrameW, &v))) w = (int)v;
                if (SUCCEEDED(si->GetUInt32(kPK_FrameH, &v))) hh = (int)v;
                if (SUCCEEDED(si->GetUInt32(kPK_FrameRate, &v))) fps = v / 1000.0;
                IShellItemImageFactory* f = nullptr;
                if (SUCCEEDED(si->QueryInterface(IID_PPV_ARGS(&f)))) {
                    HBITMAP hb = nullptr;
                    if (SUCCEEDED(f->GetImage(SIZE{128, 72}, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hb)) && hb) {
                        BITMAP bm{}; GetObject(hb, sizeof bm, &bm);
                        if (bm.bmWidth > 0 && bm.bmHeight != 0) {
                            tw = bm.bmWidth; th = std::abs(bm.bmHeight);
                            px.resize((size_t)tw * th);
                            BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = tw; bi.bmiHeader.biHeight = -th;
                            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
                            HDC dc = GetDC(nullptr);
                            if (!GetDIBits(dc, hb, 0, th, px.data(), &bi, DIB_RGB_COLORS)) { px.clear(); tw = th = 0; }
                            ReleaseDC(nullptr, dc);
                            for (auto& p : px) p |= 0xFF000000u;  // video frames are opaque
                        }
                        DeleteObject(hb);
                    }
                    f->Release();
                }
                si->Release();
            }
            {
                std::lock_guard<std::mutex> l(g_itemsM);
                for (auto& it : g_items) if (it.path == path) {
                    it.dur = dur; it.w = w; it.h = hh; it.fps = fps; it.metaDone = true; it.thumbPx = std::move(px); it.tw = tw; it.th = th;
                }
            }
            if (h) PostMessageW(h, WM_APP_META, 0, 0);
        }
        g_metaBusy = false;
        if (SUCCEEDED(co)) CoUninitialize();
    }); }).detach();
}
// Folder watcher: FindFirstChangeNotification on the output folder, debounced refresh on the UI thread.
HANDLE g_watchStop = nullptr; std::thread g_watchTh; std::wstring g_watchDir;
void stopWatcher() { if (g_watchStop) SetEvent(g_watchStop); if (g_watchTh.joinable()) g_watchTh.join(); if (g_watchStop) { CloseHandle(g_watchStop); g_watchStop = nullptr; } }
void startWatcher() {
    if (g_renderOnly) return;
    std::wstring dir = outputDir();
    if (dir == g_watchDir && g_watchTh.joinable()) return;
    stopWatcher();
    g_watchDir = dir;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    g_watchStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE stop = g_watchStop; HWND h = g.hwnd;
    g_watchTh = std::thread([dir, stop, h] { cr::guardedRun(L"ui-worker", [&] {
        HANDLE ch = FindFirstChangeNotificationW(dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (ch == INVALID_HANDLE_VALUE) return;
        HANDLE hs[2] = {stop, ch};
        while (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            PostMessageW(h, WM_APP_DIRCHANGE, 0, 0);
            if (!FindNextChangeNotification(ch)) break;
        }
        FindCloseChangeNotification(ch);
    }); });
}
// Background file jobs, one at a time, in order: manual / auto remux MKV -> MP4 (Media Foundation by default) and
// post-processing of a just-saved recording (MKV conversion, track names). None of these can delete a saved file:
// the source is only removed after the new file is complete.
struct FileJob { std::wstring in; bool post = false; cr::PostJob pj; };
std::mutex g_remuxM;
std::vector<FileJob> g_remuxQueue;               // guarded by g_remuxM
std::wstring g_remuxCur, g_remuxOut;             // guarded by g_remuxM
unsigned long long g_remuxInSize = 0;            // guarded by g_remuxM
bool g_remuxRun = false;                         // guarded by g_remuxM: a worker thread owns the queue
struct RemuxDone { bool ok; std::wstring in, out, err; bool post = false; };
std::wstring remuxTarget(const std::wstring& in) {
    std::wstring base = in.substr(0, in.size() - 4), out = base + L".mp4";
    for (int k = 2; GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES && k < 100; ++k) out = base + L" (" + std::to_wstring(k) + L").mp4";
    return out;
}
void remuxPumpLocked() {  // call with g_remuxM held
    if (g_remuxRun || g_remuxQueue.empty()) return;
    g_remuxRun = true;
    HWND h = g.hwnd;
    std::thread([h] { cr::guardedRun(L"ui-worker", [&] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            FileJob job;
            {
                std::lock_guard<std::mutex> l(g_remuxM);
                // empty check and "no worker" flag flip under one lock: a job queued now can't be stranded
                if (g_remuxQueue.empty()) { g_remuxCur.clear(); g_remuxOut.clear(); g_remuxRun = false; break; }
                job = g_remuxQueue.front(); g_remuxQueue.erase(g_remuxQueue.begin());
                g_remuxCur = job.in;
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                g_remuxInSize = GetFileAttributesExW(job.in.c_str(), GetFileExInfoStandard, &fa) ? (((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) : 0;
            }
            auto* r = new RemuxDone{false, job.in, L"", L"", job.post};
            if (job.post) {
                const std::wstring& dst = job.pj.mkvPath.empty() ? job.pj.file : job.pj.mkvPath;
                { std::lock_guard<std::mutex> l(g_remuxM); g_remuxOut = dst; }
                std::wstring fin, plog;
                r->ok = postProcessRecording(job.pj, fin, plog);
                r->out = fin; r->err = plog;
                cr::appLog(L"post-process " + job.pj.file + L": " + plog);
            } else {
                r->out = remuxTarget(job.in);
                { std::lock_guard<std::mutex> l(g_remuxM); g_remuxOut = r->out; }
                r->ok = remuxFile(job.in, r->out, r->err, L"", 0);
            }
            if (!PostMessageW(h, WM_APP_REMUX_DONE, 0, (LPARAM)r)) delete r;
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }); }).detach();
}
bool remuxBusy() { std::lock_guard<std::mutex> l(g_remuxM); return g_remuxRun; }
void queueRemux(const std::vector<std::wstring>& files) {
    {
        std::lock_guard<std::mutex> l(g_remuxM);
        for (auto& f : files) {
            bool dup = f == g_remuxCur;
            for (auto& q : g_remuxQueue) if (q.in == f) dup = true;
            if (!dup) { FileJob j; j.in = f; g_remuxQueue.push_back(j); }
        }
        remuxPumpLocked();
    }
    if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE);
}
void queuePost(const cr::PostJob& pj) {
    { std::lock_guard<std::mutex> l(g_remuxM); FileJob j; j.in = pj.file; j.post = true; j.pj = pj; g_remuxQueue.push_back(j); remuxPumpLocked(); }
    if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE);
}
std::wstring g_fakeRemuxPath;  // --render-ui: this row shows "Remuxing 42%"
// 0..1 progress for a file being remuxed (output size vs input size), -1 if queued, -2 if not involved
float remuxProgress(const std::wstring& path) {
    if (!g_fakeRemuxPath.empty()) return path == g_fakeRemuxPath ? 0.42f : -2;
    std::lock_guard<std::mutex> l(g_remuxM);
    if (path == g_remuxCur) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        unsigned long long o = GetFileAttributesExW(activeTempFor(g_remuxOut).c_str(), GetFileExInfoStandard, &fa) ? (((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) : 0;
        return g_remuxInSize ? std::min(0.99f, (float)o / (float)g_remuxInSize) : 0.f;
    }
    for (auto& q : g_remuxQueue) if (q.in == path) return -1;
    return -2;
}
bool hasMp4Sibling(const std::wstring& mkv) { return GetFileAttributesW((mkv.substr(0, mkv.size() - 4) + L".mp4").c_str()) != INVALID_FILE_ATTRIBUTES; }
// ---------- Record view ----------
struct RecLayout { RF src, fps, qual, preview, surf, mic, micIcon, sys, pause, record, status, pill; };
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
    L.sys = RF(L.mic.X + 150, cy - 20, 136, 40);
    L.record = RF(W - 20 - 122, cy - 22, 122, 44);
    L.pause = RF(L.record.X - 10 - 44, cy - 22, 44, 44);
    L.pill = RF(170, cy - 15, 138, 30);
    L.status = RF(20, cy - 14, 146, 28);
    return L;
}
RECT toPx(RF r) { float s = scale(); return RECT{(LONG)std::floor(r.X * s), (LONG)std::floor(r.Y * s), (LONG)std::ceil((r.X + r.Width) * s), (LONG)std::ceil((r.Y + r.Height) * s)}; }
float g_renderW = 1100, g_renderH = 700;  // --render-ui logical size
float clientW() { RECT c{}; if (g.hwnd) GetClientRect(g.hwnd, &c); else return g_renderW; return c.right / scale(); }
float clientH() { RECT c{}; if (g.hwnd) GetClientRect(g.hwnd, &c); else return g_renderH; return c.bottom / scale(); }
RECT previewRectNow() { return toPx(recLayout(clientW(), clientH()).surf); }

void paintLiveChip(gp::Graphics& gr, float x, float y) {
    std::wstring t = L"Live preview";
    float w = txtW(gr, t, F.caption) + 30;
    fillRR(gr, RF(x, y, w, 22), 11, gp::Color(235, 10, 12, 15));
    strokeRR(gr, RF(x, y, w, 22), 11, gp::Color(0x14, 255, 255, 255));
    unsigned dot = uiActive() ? T_ACC : (g.gprev.live() || g.prevLive || g_renderOnly ? T_OK : 0x6B7280);
    fillCircle(gr, x + 12, y + 11, 3.5f, C(dot));
    txt(gr, t, RF(x + 21, y, w - 24, 22), F.caption, C(0xD5D8DE), AL_L, false);
}
std::wstring sizeFpsText() { int w, h; sourceSize(w, h); return (w ? dims(w, h) + L" \u00b7 " : L"") + std::to_wstring(g.s.fps) + L" fps"; }
void paintSizeChip(gp::Graphics& gr, RF r) {  // on the captured image: always the dark pill
    std::wstring t = sizeFpsText();
    float w = txtW(gr, t, F.caption) + 22;
    fillRR(gr, RF(r.X + r.Width - w, r.Y, w, 22), 11, gp::Color(235, 10, 12, 15));
    strokeRR(gr, RF(r.X + r.Width - w, r.Y, w, 22), 11, gp::Color(0x14, 255, 255, 255));
    txt(gr, t, RF(r.X + r.Width - w, r.Y, w, 22), F.caption, C(0xD5D8DE), AL_C, false);
}
void paintPreviewFallback(gp::Graphics& gr, RF s) {
    fillRR(gr, s, 6, C(g_light ? 0xD5D9E0 : 0x1A1F27));
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
    int sec = g_fakeRec ? 84 : (int)g.rec.elapsedSeconds();
    wchar_t tm[32]; swprintf(tm, 32, L"%02d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    return tm;
}
std::wstring sizeText(unsigned long long b, bool decimalsBelowGb = true) {
    wchar_t t[32];
    double mb = b / 1048576.0;
    if (mb >= 1024) swprintf(t, 32, L"%.1f GB", mb / 1024);
    else if (decimalsBelowGb) swprintf(t, 32, L"%.1f MB", mb);
    else swprintf(t, 32, L"%.0f MB", mb);
    return t;
}
unsigned long long g_recBytes = 0;  // file size so far (refreshed every second while recording)
void paintRecordView(gp::Graphics& gr, float W, float H) {
    RecLayout L = recLayout(W, H);
    bool active = uiActive(), paused = uiPaused();
    RecState st = g.rec.state();
    bool finishing = !g_fakeRec && (st == RecState::Stopping || st == RecState::Finishing);
    // toolbar
    int glyph = g.target.kind == CaptureTarget::AllDisplays ? G_FULLSCREEN : g.target.kind == CaptureTarget::Monitor ? G_MONITOR : G_WINDOW;
    if (g.target.kind == CaptureTarget::Window && !g_srcApp.empty()) ddField(gr, L.src, H_SRC, g_srcApp, glyph, g_srcIcon, &g_srcPage);
    else ddField(gr, L.src, H_SRC, sourceText(), glyph, g.target.kind == CaptureTarget::Window ? g_srcIcon : nullptr);
    focusRing(gr, L.src, 8, H_SRC);
    ddField(gr, L.fps, H_DD_FPS, ddItem(H_DD_FPS, ddSel(H_DD_FPS)));
    focusRing(gr, L.fps, 8, H_DD_FPS);
    ddField(gr, L.qual, H_DD_QUAL, ddItem(H_DD_QUAL, ddSel(H_DD_QUAL)));
    focusRing(gr, L.qual, 8, H_DD_QUAL);
    // preview
    card(gr, L.preview, 12, C(T_PREV), WA(A_SUBTLE));
    if (!g.gprev.live()) paintPreviewFallback(gr, L.surf);
    else { gp::SolidBrush b(C(0x000000)); gr.FillRectangle(&b, L.surf); }
    if (active) strokeRR(gr, L.preview, 12, C(T_ACC), 2.f);
    paintLiveChip(gr, L.surf.X + 12, L.surf.Y + 12);
    paintSizeChip(gr, RF(L.surf.X + 12, L.surf.Y + 12, L.surf.Width - 24, 22));
    // status (left)
    {
        RF s = L.status;
        float cy = s.Y + s.Height / 2;
        if (active) {
            unsigned dc = paused ? T_WARN : T_ACC;
            double ph = fmod(nowMs(), 1200.0) / 1200.0;
            if (!paused && animationsOn()) fillCircle(gr, s.X + 6, cy, 6 + 4 * (float)ph, C(dc, (int)(90 * (1 - ph))));
            fillCircle(gr, s.X + 6, cy, 5, C(dc));
            std::wstring t = timerText();
            float tw = txtW(gr, t, F.btn);
            txt(gr, t, RF(s.X + 18, s.Y, tw + 4, s.Height), F.btn, C(paused ? T_WARN : T_TEXT), AL_L, false);
            std::wstring sz = paused ? std::wstring(L"Paused") : sizeText(g_fakeRec ? 48ull << 20 : g_recBytes, false);
            txt(gr, sz, RF(s.X + 18 + tw + 12, s.Y, s.Width - tw - 30, s.Height), F.status, C(paused ? T_WARN : T_MUTED), AL_L, false);
        } else {
            unsigned dotc, tc; std::wstring t;
            if (finishing) { dotc = T_MUTED; tc = T_TEXT2; t = L"Saving\u2026"; }
            else {
                unsigned sc = fromRef(g.statusColor);
                t = g.status.empty() ? L"Ready" : g.status;
                bool err = sc == T_ERR;
                dotc = err ? T_ERR : sc == T_WARN ? T_WARN : T_OK;
                tc = err ? T_ERR : sc == T_WARN ? T_WARN : sc == T_OK && t != L"Ready" ? T_OK : T_TEXT2;
            }
            fillCircle(gr, s.X + 6, cy, 4, C(dotc));
            bool err = !finishing && fromRef(g.statusColor) == T_ERR;
            // The status can be long (saved file, errors): it may run under the pill when the pill is hidden.
            float avail = (g.s.replayEnabled || g_fakeRec ? L.pill.X - 8 : L.mic.X - 16) - (s.X + 18);
            float linkW = err ? txtW(gr, L"Open log", F.status) + 14 : 0;
            float tw = std::min(txtW(gr, t, F.status) + 2, avail - linkW);
            txt(gr, t, RF(s.X + 18, s.Y, tw, s.Height), F.status, C(tc));
            if (err) {
                RF lr(s.X + 18 + tw + 10, s.Y + 4, linkW - 10, s.Height - 8);
                txt(gr, L"Open log", lr, F.status, C(hovA(H_STATUSLOG) > 0.5f ? T_TEXT : T_ACC_B), AL_L, false);
                hline(gr, lr.X, lr.X + lr.Width, lr.Y + lr.Height - 2, C(T_ACC_B, 160));
                addReg(lr, H_STATUSLOG);
            }
        }
        g_statusRect = RF(s.X, s.Y, std::max(s.Width, L.mic.X - 16 - s.X), s.Height);
    }
    // replay pill (only while the buffer is on)
    if (g.s.replayEnabled) {
        RF p = L.pill;
        pressScope ps(gr, p, H_REPLAYPILL);
        fillRR(gr, p, 15, mix(T_SURF, T_INPUT_H, hovA(H_REPLAYPILL)));
        strokeRR(gr, p, 15, WA(A_DEFAULT));
        bool spin = g.replaySaving && nowMs() - g.replaySaveT < 300;
        icon(gr, G_REPLAY, p.X + 12, p.Y + 7, 16, C(T_TEXT2), spin ? (float)fmod(nowMs() * 0.72, 360.0) : 0.f);
        std::wstring t = L"Replay \u00b7 " + replayShort(g.s.replaySeconds);
        txt(gr, t, RF(p.X + 34, p.Y, p.Width - 56, p.Height), F.label, C(T_TEXT), AL_L, false);
        bool live = g_renderOnly || g.replay.isActive() || (g.rec.isActive() && g.ring);
        double ph = fmod(nowMs(), 1600.0) / 1600.0, tri = ph < 0.5 ? ph * 2 : 2 - ph * 2;
        float a = live ? (animationsOn() ? 0.4f + 0.6f * (float)ease(E_INOUT, tri) : 1.f) : 1.f;
        fillCircle(gr, p.X + p.Width - 16, p.Y + 15, 4, C(live ? T_OK : T_MUTED, (int)(255 * a)));
        focusRing(gr, p, 15, H_REPLAYPILL);
        addReg(p, H_REPLAYPILL);
        g_pillRect = p;
    } else g_pillRect = RF(0, 0, 0, 0);
    // audio chips
    {
        bool micOn = g.s.micEnabled, muted = g.s.micMuted;
        bool open = P.kind == PK_MIC && !P.closing;
        RF m = L.mic;
        { pressScope ps(gr, m, H_MICCHIP);
        fillRR(gr, m, 8, mix(T_SURF, T_INPUT_H, hovA(H_MICCHIP) * 0.8f));
        strokeRR(gr, m, 8, open ? C(T_ACC) : WA(A_DEFAULT));
        if (hovA(H_MICICON) > 0) fillRR(gr, RF(m.X + 4, m.Y + 4, 28, 32), 6, WA((int)(A_PRESSED * hovA(H_MICICON))));
        icon(gr, muted ? G_MICMUTED : G_MIC, m.X + 10, m.Y + 12, 16, C(!micOn ? T_DIS : muted ? T_ACC_B : T_TEXT2));
        meterW(gr, m.X + 38, m.Y + 17, m.Width - 38 - 14, 6, g_meter[0], micOn && !muted);
        focusRing(gr, m, 8, H_MICCHIP); }
        addReg(m, H_MICCHIP);
        addReg(L.micIcon, H_MICICON);
        RF s = L.sys;
        bool sysOn = g.s.systemAudio, smuted = g.s.systemMuted;
        { pressScope ps(gr, s, H_SYSCHIP);
        fillRR(gr, s, 8, mix(T_SURF, T_INPUT_H, hovA(H_SYSCHIP) * 0.8f));
        strokeRR(gr, s, 8, WA(A_DEFAULT));
        icon(gr, smuted ? G_SYSMUTED : G_SYS, s.X + 10, s.Y + 12, 16, C(!sysOn ? T_DIS : smuted ? T_ACC_B : T_TEXT2));
        meterW(gr, s.X + 38, s.Y + 17, s.Width - 38 - 14, 6, g_meter[1], sysOn && !smuted);
        focusRing(gr, s, 8, H_SYSCHIP); }
        addReg(s, H_SYSCHIP);
    }
    // Pause (disabled when idle; Paused shows the record glyph = resume)
    {
        bool dis = !active;
        pressScope ps(gr, L.pause, dis ? H_NONE : H_PAUSE);
        buttonBg(gr, L.pause, 12, H_PAUSE, T_ELEV, WA(A_DEFAULT), dis);
        icon(gr, paused ? G_RECORD : G_PAUSE, L.pause.X + 14, L.pause.Y + 14, 16, C(dis ? T_DIS : paused ? T_ACC_B : T_TEXT));
        addReg(L.pause, H_PAUSE);
    }
    // Record / Stop with the pulse (1200 ms loop, glow alpha .15 <-> .45)
    {
        RF r = L.record;
        pressScope ps(gr, r, H_RECORD);
        float bloom = 0;
        { auto it = g_press.find(H_RECORD); if (it != g_press.end()) bloom = std::max(0.f, it->second.value()); }
        if (active || finishing) {
            if (active && !paused) {
                double ph = fmod(nowMs(), 1200.0) / 1200.0;
                double tri = ph < 0.5 ? ph * 2 : 2 - ph * 2;
                float a = 0.15f + 0.30f * (float)ease(E_INOUT, tri);
                if (!animationsOn()) a = 0.3f;
                a = std::max(a, 0.25f + 0.2f * bloom);
                for (int i = 6; i >= 1; --i) strokeRR(gr, inflate(r, (float)i), 12.f + i, C(T_ACC, (int)(255 * a * (7 - i) / 14.f)), 2.f);
            }
            buttonBg(gr, r, 12, H_RECORD, T_ELEV, finishing ? WA(A_DEFAULT) : gp::Color(0, 0, 0, 0));
            if (!finishing) strokeRR(gr, r, 12, C(T_ACC), 2.f);
            std::wstring t = finishing ? L"Saving" : L"Stop";
            float tw = txtW(gr, t, F.btnP);
            float x0 = r.X + (r.Width - (16 + 8 + tw)) / 2;
            icon(gr, G_STOP, x0, r.Y + 14, 16, C(finishing ? T_DIS : T_ACC));
            txt(gr, t, RF(x0 + 24, r.Y, tw + 4, r.Height), F.btnP, C(finishing ? T_TEXT2 : T_TEXT), AL_L, false);
        } else {
            float hv = hovA(H_RECORD);
            if (bloom > 0) for (int i = 6; i >= 1; --i) strokeRR(gr, inflate(r, (float)i), 12.f + i, C(T_ACC, (int)(255 * 0.45f * bloom * (7 - i) / 14.f)), 2.f);
            bool pr = g.pressed == H_RECORD && g.hover == H_RECORD;
            gp::Color bg = pr ? C(T_ACC_P) : mix(T_ACC, T_ACC_H, hv);
            fillRR(gr, r, 12, bg);
            std::wstring t = L"Record";
            float tw = txtW(gr, t, F.btnP);
            float x0 = r.X + (r.Width - (16 + 8 + tw)) / 2;
            icon(gr, G_RECORD, x0, r.Y + 14, 16, C(0xFFFFFF));
            txt(gr, t, RF(x0 + 24, r.Y, tw + 4, r.Height), F.btnP, C(0xFFFFFF), AL_L, false);
            focusRing(gr, r, 12, H_RECORD);
        }
        addReg(r, H_RECORD);
        g_recordRect = inflate(r, 8);
    }
}
// ---------- Settings view (v1.1: 3 columns x 340, SPEC "Settings view") ----------
void cardTitle(gp::Graphics& gr, RF c, const wchar_t* t) { txt(gr, t, RF(c.X + 18, c.Y + 12, c.Width - 36, 18), F.section, C(T_MUTED), AL_L, false); }
// maxW: room up to the control on the right; labels ellipsize (with a tooltip) instead of running under it
void label(gp::Graphics& gr, const wchar_t* t, float x, float cy, bool dis = false, float maxW = 220) { txt(gr, t, RF(x, cy - 12, maxW, 24), F.label, C(dis ? T_DIS : T_TEXT2), AL_L, true); }
void hotkeyField(gp::Graphics& gr, RF r, int id, int idx, const Hotkey& hk, bool dimmed = false) {
    bool cap = g.capturing == idx, dis = locked(id);
    pressScope ps(gr, r, id);
    fillRR(gr, r, 8, mix(T_INPUT, T_INPUT_H, cap ? 0.f : dis ? 0.f : hovA(id)));
    COLORREF mc = g.hkMsgColor[idx];
    bool bad = fromRef(mc) == T_ERR && !cap && !hk.empty();
    strokeRR(gr, r, 8, cap ? C(T_ACC) : bad ? C(T_ERR) : WA(A_DEFAULT), cap || bad ? 1.5f : 1.f);
    std::wstring t; unsigned tc = T_TEXT;
    if (cap) { t = g.liveMods ? modsText(g.liveMods) + L"\u2026" : L"Press a key\u2026"; tc = T_MUTED; }
    else if (hk.empty()) { t = L"Not set"; tc = T_MUTED; }
    else t = hkText(hk);
    if ((dis || dimmed) && !cap) tc = T_DIS;
    RF inner(r.X + 8, r.Y, r.Width - 16, r.Height);
    if (!cap && !hk.empty() && txtW(gr, t, F.hotkey) > inner.Width) t = W(format(hk));  // "Ctrl+Alt+Shift+F12": no spaces when tight
    txt(gr, t, inner, F.hotkey, C(tc), AL_C);
    if (bad && hovA(id) > 0.5f && !g.hkMsg[idx].empty()) g_tooltip = {r, g.hkMsg[idx]};
    focusRing(gr, r, 8, id);
    addReg(r, id);
}
// Two-option segmented control. sel 0/1, anim drives the thumb.
void segmented2(gp::Graphics& gr, RF r, int idA, int idB, const wchar_t* a, const wchar_t* b, int sel, Anim& an, int glyphA = -1, int glyphB = -1, bool dis = false) {
    fillRR(gr, r, 8, C(T_SEGBG));
    strokeRR(gr, r, 8, WA(A_DEFAULT));
    float target = (float)sel;
    if (an.to != target) an.go(target, 160, E_STD);
    float k = an.value(), hw = (r.Width - 6) / 2;
    RF th(r.X + 3 + k * hw, r.Y + 3, hw, r.Height - 6);
    fillRR(gr, th, 6, C(g_light ? T_SEGTH : T_ELEV));
    strokeRR(gr, th, 6, WA(A_SUBTLE));
    for (int i = 0; i < 2; ++i) {
        RF c(r.X + 3 + i * hw, r.Y + 3, hw, r.Height - 6);
        const wchar_t* t = i ? b : a;
        int gl = i ? glyphB : glyphA;
        gp::Font* f = sel == i ? F.btn : F.body;
        unsigned col = dis ? T_DIS : sel == i ? T_TEXT : T_TEXT2;
        float tw = txtW(gr, t, f), gw = gl >= 0 ? 22.f : 0.f;
        float x0 = c.X + (c.Width - tw - gw) / 2;
        if (gl >= 0) icon(gr, gl, x0, c.Y + c.Height / 2 - 8, 16, C(col));
        txt(gr, t, RF(x0 + gw, c.Y, tw + 4, c.Height), f, C(col), AL_L, false);
        addReg(RF(r.X + i * r.Width / 2, r.Y, r.Width / 2, r.Height), i ? idB : idA);
    }
    focusRing(gr, r, 8, idA);
}
Anim g_themeSeg;
// Three columns, 340 wide at the default 1100 px; narrower windows (down to 960) shrink them, wider ones centre them.
// Every control is placed relative to its column, so nothing runs off the right edge at the minimum size.
struct SetCols { float x[3]; float w; };
SetCols setCols(float W) {
    SetCols c; c.w = std::floor(std::max(280.f, std::min(340.f, (W - 80) / 3)));
    float total = 3 * c.w + 40, x0 = std::max(20.f, std::floor((W - total) / 2));
    for (int i = 0; i < 3; ++i) c.x[i] = x0 + i * (c.w + 20);
    return c;
}
float g_sliderX = 530, g_sliderW = 123;  // audio sliders (hit testing / dragging follow the painted geometry)
RF replayLenEditRect() { SetCols c = setCols(clientW()); return RF(c.x[0] + 130, 417, c.w - 220, 18); }
void paintSettingsView(gp::Graphics& gr, float W, float H) {
    (void)H;
    const SetCols cl = setCols(W);
    const float a = cl.x[0], b = cl.x[1], c = cl.x[2], cw = cl.w;
    RF video(a, 60, cw, 268), replay(a, 340, cw, 122), look(a, 474, cw, 126), audio(b, 60, cw, 540), hot(c, 60, cw, 260), out(c, 332, cw, 268);
    for (RF r : {video, replay, look, audio, hot, out}) card(gr, r, 12, C(T_SURF), WA(A_SUBTLE));
    const float fx = a + 118, fw = cw - 136;   // dropdown column in the left cards
    const float tx = a + cw - 54;              // toggles
    // Video
    cardTitle(gr, video, L"VIDEO");
    const wchar_t* vl[4] = {L"Encoder", L"GPU", L"Resolution", L"Container"};
    const int vid[4] = {H_DD_ENC, H_DD_GPU, H_DD_RES, H_DD_CONT};
    for (int i = 0; i < 4; ++i) {
        float cy = 119.f + 44 * i;
        label(gr, vl[i], a + 18, cy, locked(vid[i]), fx - a - 26);
        RF f(fx, cy - 16, fw, 32);
        ddField(gr, f, vid[i], vid[i] == H_DD_ENC ? encoderText() : ddItem(vid[i], ddSel(vid[i])));
        focusRing(gr, f, 8, vid[i]);
    }
    if (g.s.codec == Codec::AV1) txt(gr, L"AV1 plays best in MKV", RF(fx, 268, fw, 14), F.caption, C(T_MUTED), AL_L);
    label(gr, L"Auto-remux to MP4", a + 18, 299, locked(H_TG_REMUX), tx - a - 26);
    toggleW(gr, tx, 299, H_TG_REMUX, g.s.autoRemux);
    // Instant replay
    cardTitle(gr, replay, L"INSTANT REPLAY");
    label(gr, L"Replay buffer", a + 18, 390, locked(H_TG_REPLAY), tx - a - 26);
    toggleW(gr, tx, 390, H_TG_REPLAY, g.s.replayEnabled);
    label(gr, L"Length", a + 18, 426, false, fx - a - 26);
    RF lf(fx, 410, fw, 32);
    if (g_edit.kind == 2) { g_edit.rect = lf; fillRR(gr, lf, 8, C(T_INPUT)); strokeRR(gr, lf, 8, C(T_ACC), 1.5f);
                            txt(gr, L"seconds", RF(lf.X + 12, lf.Y, lf.Width - 24, lf.Height), F.label, C(T_MUTED), AL_R, false); }
    else { ddField(gr, lf, H_DD_REPLAYLEN, replayLenText()); focusRing(gr, lf, 8, H_DD_REPLAYLEN); }
    if (g.s.replayEnabled) {
        ReplayBudget rb = replayBudget(g.s.replaySeconds);
        wchar_t cap[32]; swprintf(cap, 32, L"%.1f GB", rb.cap / 1073741824.0);
        if (rb.effSecs < g.s.replaySeconds)
            txt(gr, L"RAM cap " + std::wstring(cap) + L": keeps " + durText(rb.effSecs), RF(fx, 443, fw, 16), F.caption, C(T_WARN), AL_L);
        else {
            wchar_t mb[48]; swprintf(mb, 48, L"Uses about %.0f MB of RAM", (double)(rb.perSec * (unsigned long long)g.s.replaySeconds) / 1048576.0);
            txt(gr, mb, RF(fx, 443, fw, 16), F.caption, C(T_MUTED), AL_L);
        }
    }
    // Appearance
    cardTitle(gr, look, L"APPEARANCE");
    label(gr, L"Theme", a + 18, 530, false, fx - a - 26);
    segmented2(gr, RF(fx, 514, fw, 32), H_THEME_DARK, H_THEME_LIGHT, L"Dark", L"Light", g_light ? 1 : 0, g_themeSeg, G_MOON, G_SUN);
    label(gr, L"Accent", a + 18, 571, false, fx - a - 26);
    const float step = std::min(32.f, std::floor((fw - 24) / 5));
    for (int i = 0; i < 6; ++i) {
        float cx = fx + 12 + step * i, cy = 571;
        int id = H_ACC0 + i;
        bool sel = accentIndex() == i;
        pressScope ps(gr, RF(cx - 12, cy - 12, 24, 24), id);
        if (sel) { gp::Pen pn(C(kAccents[i].base), 2.f); gr.DrawEllipse(&pn, cx - 11, cy - 11, 22.f, 22.f); }
        else if (hovA(id) > 0) { gp::Pen pn(WA((int)(A_STRONG * hovA(id))), 2.f); gr.DrawEllipse(&pn, cx - 11, cy - 11, 22.f, 22.f); }
        fillCircle(gr, cx, cy, 8, C(kAccents[i].base));
        focusRing(gr, RF(cx - 12, cy - 12, 24, 24), 12, id);
        addReg(RF(cx - 14, cy - 14, 28, 28), id);
    }
    // Audio
    cardTitle(gr, audio, L"AUDIO");
    const float lx = b + 18, rx = b + cw - 19;
    const float sx = lx + std::max(100.f, std::min(132.f, 100 + (cw - 293) * 32 / 47)), sw = rx - sx - 48;
    g_sliderX = sx; g_sliderW = sw;
    const float tgl = rx - 36;
    label(gr, L"System audio", lx, 107, locked(H_TG_SYS), tgl - lx - 8);
    toggleW(gr, tgl, 107, H_TG_SYS, g.s.systemAudio);
    ddField(gr, RF(lx, 128, rx - lx, 32), H_DD_SYSDEV, ddItem(H_DD_SYSDEV, ddSel(H_DD_SYSDEV)), g.s.systemMuted ? G_SYSMUTED : G_SYS);
    focusRing(gr, RF(lx, 128, rx - lx, 32), 8, H_DD_SYSDEV);
    meterW(gr, lx, 168, rx - lx, 4, g_meter[1], g.s.systemAudio && !g.s.systemMuted);
    hline(gr, lx, rx, 189, WA(A_SUBTLE));
    label(gr, L"Microphone", lx, 214, locked(H_TG_MIC), tgl - lx - 8);
    toggleW(gr, tgl, 214, H_TG_MIC, g.s.micEnabled);
    ddField(gr, RF(lx, 234, rx - lx, 32), H_DD_MICDEV, ddItem(H_DD_MICDEV, ddSel(H_DD_MICDEV)), g.s.micMuted ? G_MICMUTED : G_MIC);
    focusRing(gr, RF(lx, 234, rx - lx, 32), 8, H_DD_MICDEV);
    meterW(gr, lx, 274, rx - lx, 4, g_meter[0], g.s.micEnabled && !g.s.micMuted);
    float y = 307;
    label(gr, L"Volume", lx, y, false, sx - lx - 14);
    sliderW(gr, sx, y, sw, H_SL_VOL, g.s.micVolume / 200.f);
    addReg(RF(sx - 10, y - 14, sw + 20, 28), H_SL_VOL);
    txt(gr, std::to_wstring(g.s.micVolume) + L"%", RF(sx + sw + 10, y - 12, rx - sx - sw - 10, 24), F.label, C(T_TEXT2), AL_R, false);
    y = 347;
    label(gr, L"Noise suppression", lx, y, false, tgl - lx - 8);
    toggleW(gr, tgl, y, H_TG_NS, g.s.noiseSuppression);
    y = 387;
    label(gr, L"Noise gate", lx, y, false, sx - lx - 14);
    sliderW(gr, sx, y, sw, H_SL_GATE, (g.s.noiseGateDb + 80) / 80.f);
    addReg(RF(sx - 10, y - 14, sw + 20, 28), H_SL_GATE);
    txt(gr, g.s.noiseGateDb <= -80 ? std::wstring(L"Off") : std::to_wstring(g.s.noiseGateDb) + L" dB", RF(sx + sw + 10, y - 12, rx - sx - sw - 10, 24), F.label, C(T_TEXT2), AL_R, false);
    y = 427;
    label(gr, L"Monitoring", lx, y, false, tgl - lx - 8);
    toggleW(gr, tgl, y, H_TG_MON, g.s.monitoring);
    y = 467;
    label(gr, L"Push-to-talk", lx, y, false, sx - lx - 14);
    hotkeyField(gr, RF(sx, y - 15, std::min(116.f, tgl - 12 - sx), 30), H_HKPTT, 3, g.s.pttKey, !g.s.pushToTalk);
    toggleW(gr, tgl, y, H_TG_PTT, g.s.pushToTalk);
    y = 507;
    label(gr, L"Channels", lx, y, locked(H_CH_MONO), sx - lx - 14);
    segmented2(gr, RF(sx, y - 16, rx - sx, 32), H_CH_MONO, H_CH_STEREO, L"Mono", L"Stereo", g.s.channels == 2 ? 1 : 0, g_chan, -1, -1, locked(H_CH_MONO));
    hline(gr, lx, rx, 537, WA(A_SUBTLE));
    label(gr, L"Separate audio tracks", lx, 560, locked(H_TG_TRACKS), tgl - lx - 8);
    txt(gr, L"Track 1 mix \u00b7 2 system \u00b7 3 mic", RF(lx, 572, tgl - lx - 8, 16), F.caption, C(T_MUTED), AL_L);
    toggleW(gr, tgl, 566, H_TG_TRACKS, g.s.separateTracks);
    // Hotkeys
    cardTitle(gr, hot, L"HOTKEYS");
    const int order[4] = {0, 1, 2, 4};
    const int hid[4] = {H_HK0, H_HK1, H_HK2, H_HK3};
    const float hx = c + 132, hw = cw - 150;
    for (int i = 0; i < 4; ++i) {
        float top = 103.f + 50 * i;
        label(gr, kHkNames[order[i]], c + 18, top + 16, false, hx - c - 26);
        hotkeyField(gr, RF(hx, top, hw, 32), hid[i], order[i], *hkSlot(order[i]));
    }
    // Output
    cardTitle(gr, out, L"OUTPUT");
    label(gr, L"Folder", c + 18, 381, false, cw - 36);
    {
        RF pf(c + 18, 392, cw - 36, 32);
        fillRR(gr, pf, 8, mix(T_INPUT, T_INPUT_H, hovA(H_PATH)));
        strokeRR(gr, pf, 8, WA(A_DEFAULT));
        RF fi(pf.X + 4, pf.Y + 4, 28, 24);
        if (hovA(H_FOLDERICON) > 0) fillRR(gr, fi, 6, WA((int)(A_PRESSED * hovA(H_FOLDERICON))));
        icon(gr, G_FOLDER, pf.X + 10, pf.Y + 8, 16, C(T_TEXT2));
        txt(gr, outputDir(), RF(pf.X + 36, pf.Y, pf.Width - 46, pf.Height), F.label, C(T_TEXT), AL_L, true, true);
        focusRing(gr, pf, 8, H_PATH);
        addReg(pf, H_PATH);
        addReg(fi, H_FOLDERICON);
        const float bw2 = std::floor((cw - 36 - 8) / 2);
        RF br(c + 18, 434, bw2, 32), lg(c + cw - 18 - bw2, 434, bw2, 32);
        bool bdis = locked(H_BROWSE);
        { pressScope ps(gr, br, H_BROWSE); buttonBg(gr, br, 8, H_BROWSE, T_INPUT, WA(A_DEFAULT), bdis);
          txt(gr, L"Browse", br, F.body, C(bdis ? T_DIS : T_TEXT), AL_C, false); }
        addReg(br, H_BROWSE);
        { pressScope ps(gr, lg, H_LOG); buttonBg(gr, lg, 8, H_LOG, T_INPUT, WA(A_DEFAULT));
          float lw = txtW(gr, L"Log", F.body);
          float lx2 = std::floor(lg.X + (lg.Width - (16 + 8 + lw)) / 2);
          icon(gr, G_LOG, lx2, lg.Y + 8, 16, C(T_TEXT2));
          txt(gr, L"Log", RF(lx2 + 24, lg.Y, lw + 4, lg.Height), F.body, C(T_TEXT), AL_L, false); }
        addReg(lg, H_LOG);
    }
    hline(gr, c + 18, c + cw - 18, 490, WA(A_SUBTLE));
    label(gr, L"File name", c + 18, 524, false, fx - a - 26);
    ddField(gr, RF(c + 118, 508, cw - 136, 32), H_DD_FNAME, ddItem(H_DD_FNAME, ddSel(H_DD_FNAME)));
    focusRing(gr, RF(c + 118, 508, cw - 136, 32), 8, H_DD_FNAME);
}
// ---------- Recordings view (SPEC "Recordings tab") ----------
float colX(float W, float x) { return 20 + (x - 20) * (W - 40) / 1060.f; }
// Recordings columns are laid out from the right edge so the action area (Remux / progress, 150 px) never
// overlaps the format badge at narrow widths; the name column takes what is left.
struct RecCols { float name, dur, size, date, fmt; };
RecCols recCols(float W) {
    RecCols c; float R = W - 20;
    c.fmt = R - 200; c.date = c.fmt - 150; c.size = c.date - 90; c.dur = c.size - 90; c.name = 40;
    return c;
}
std::wstring durText(double d) {
    if (d < 0) return L"\u2014";
    int s = (int)std::lround(d); wchar_t t[32];
    if (s >= 3600) swprintf(t, 32, L"%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60); else swprintf(t, 32, L"%02d:%02d", s / 60, s % 60);
    return t;
}
std::wstring dateText(const FILETIME& ft) {
    SYSTEMTIME u, l, now; FileTimeToSystemTime(&ft, &u); SystemTimeToTzSpecificLocalTime(nullptr, &u, &l); GetLocalTime(&now);
    auto dayNum = [](const SYSTEMTIME& s) { FILETIME f; SYSTEMTIME d = s; d.wHour = d.wMinute = d.wSecond = d.wMilliseconds = 0; SystemTimeToFileTime(&d, &f);
                                            return (long long)((((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime) / 864000000000ull); };
    long long dd = dayNum(now) - dayNum(l);
    wchar_t t[48];
    static const wchar_t* mon[12] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    if (dd == 0) swprintf(t, 48, L"Today %02d:%02d", l.wHour, l.wMinute);
    else if (dd == 1) swprintf(t, 48, L"Yesterday");
    else if (l.wYear == now.wYear) swprintf(t, 48, L"%ls %d", mon[(l.wMonth + 11) % 12], l.wDay);
    else swprintf(t, 48, L"%ls %d, %d", mon[(l.wMonth + 11) % 12], l.wDay, l.wYear);
    return t;
}
std::vector<int> visibleItems() {  // indexes into g_items matching the search (caller holds g_itemsM)
    std::vector<int> v; std::wstring q = lowerW(g_rsearch);
    for (int i = 0; i < (int)g_items.size(); ++i) if (q.empty() || lowerW(g_items[i].name).find(q) != std::wstring::npos) v.push_back(i);
    return v;
}
RF listCard(float W, float H) { return RF(20, 104, W - 40, H - 204); }
float listContentH() { std::lock_guard<std::mutex> l(g_itemsM); return 44 + visibleItems().size() * 62.f + 6; }
void clampListScroll(float W, float H) { float m = std::max(0.f, listContentH() - listCard(W, H).Height); g_listScroll = std::max(0.f, std::min(g_listScroll, m)); }
void iconBtn(gp::Graphics& gr, RF r, int id, int glyph) {
    pressScope ps(gr, r, id);
    if (hovA(id) > 0) fillRR(gr, r, 8, WA((int)(A_PRESSED * hovA(id))));
    icon(gr, glyph, r.X + r.Width / 2 - 8, r.Y + r.Height / 2 - 8, 16, C(hovA(id) > 0.5f ? T_TEXT : T_TEXT2));
    addReg(r, id);
}
void paintRecordingsView(gp::Graphics& gr, float W, float H) {
    // toolbar
    RF sf(20, 56, 320, 36);
    fillRR(gr, sf, 8, mix(T_INPUT, T_INPUT_H, g_rsearchFocus ? 0.f : hovA(H_RSEARCH)));
    strokeRR(gr, sf, 8, g_rsearchFocus ? C(T_ACC) : WA(A_DEFAULT), g_rsearchFocus ? 1.5f : 1.f);
    icon(gr, G_SEARCH, sf.X + 12, sf.Y + 10, 16, C(T_MUTED));
    if (g_rsearch.empty()) txt(gr, L"Search recordings", RF(sf.X + 36, sf.Y, sf.Width - 48, sf.Height), F.body, C(T_MUTED));
    else txt(gr, g_rsearch, RF(sf.X + 36, sf.Y, sf.Width - 48, sf.Height), F.body, C(T_TEXT));
    if (g_rsearchFocus && fmod(nowMs(), 1000.0) < 600) {
        float cw = std::min(g_rsearch.empty() ? 0.f : txtW(gr, g_rsearch, F.body), sf.Width - 50);
        gp::SolidBrush cb(C(T_TEXT)); gr.FillRectangle(&cb, sf.X + 37 + cw, sf.Y + 10, 1.f, 16.f);
    }
    addReg(sf, H_RSEARCH);
    RF ra(W - 20 - 132, 56, 132, 36);
    { pressScope ps(gr, ra, H_REMUXALL); buttonBg(gr, ra, 8, H_REMUXALL, T_ELEV, WA(A_DEFAULT));
      float tw = txtW(gr, L"Remux all", F.body), x0 = ra.X + (ra.Width - 24 - tw) / 2;
      icon(gr, G_REMUX, x0, ra.Y + 10, 16, C(T_TEXT2));
      txt(gr, L"Remux all", RF(x0 + 24, ra.Y, tw + 4, ra.Height), F.body, C(T_TEXT), AL_L, false); }
    addReg(ra, H_REMUXALL);
    toggleW(gr, ra.X - 16 - 36, 74, H_TG_AUTOREMUX, g.s.autoRemux);
    txt(gr, L"Auto-remux", RF(ra.X - 16 - 36 - 12 - 120, 62, 120, 24), F.label, C(T_TEXT2), AL_R, false);
    // list
    RF lc = listCard(W, H);
    card(gr, lc, 12, C(T_SURF), WA(A_SUBTLE));
    RecCols rc = recCols(W);
    const float cols[5] = {rc.name, rc.dur, rc.size, rc.date, rc.fmt};
    const wchar_t* heads[5] = {L"NAME", L"DURATION", L"SIZE", L"DATE", L"FORMAT"};
    for (int i = 0; i < 5; ++i) txt(gr, heads[i], RF(cols[i], 120, 84, 20), F.tiny, C(T_MUTED), AL_L, false);
    hline(gr, lc.X, lc.X + lc.Width, 144, WA(A_SUBTLE));
    gp::GraphicsState st = gr.Save();
    gr.SetClip(RF(lc.X, 145, lc.Width, lc.Y + lc.Height - 146), gp::CombineModeIntersect);
    std::lock_guard<std::mutex> l(g_itemsM);
    auto vis = visibleItems();
    if (vis.empty()) {
        txt(gr, g_items.empty() ? L"No recordings yet" : L"No matching recordings", RF(lc.X, 190, lc.Width, 24), F.title, C(T_TEXT2), AL_C);
        if (g_items.empty()) txt(gr, L"Recordings, replays and remuxed files in " + outputDir() + L" show up here.", RF(lc.X + 40, 216, lc.Width - 80, 20), F.label, C(T_MUTED), AL_C, true, true);
    }
    for (size_t k = 0; k < vis.size(); ++k) {
        RecItem& it = g_items[vis[k]];
        float top = 150 + k * 62.f - g_listScroll;
        if (top > lc.Y + lc.Height || top + 56 < 145) continue;
        int base = R_BASE + vis[k] * 4;
        RF row(lc.X + 8, top, lc.Width - 16, 56);
        float hv = std::max(hovA(base), (g_ctxPath == it.path && P.kind == PK_LIST && P.dd == H_CTXMENU && !P.closing) ? 1.f : 0.f);
        hv = std::max({hv, hovA(base + 1), hovA(base + 2), hovA(base + 3)});
        if (hv > 0) fillRR(gr, row, 10, C(T_LISTH, (int)(255 * hv)));
        addReg(row, base);
        // thumbnail 64x36 r6
        RF tr(rc.name - 2, top + 10, 64, 36);
        fillRR(gr, tr, 6, C(g_light ? 0xD5D9E0 : 0x222631));
        if (!it.thumb && it.tw > 0 && !it.thumbPx.empty()) {
            auto b = std::make_shared<gp::Bitmap>(it.tw, it.th, PixelFormat32bppARGB);
            gp::BitmapData bd; gp::Rect lr(0, 0, it.tw, it.th);
            if (b->LockBits(&lr, gp::ImageLockModeWrite, PixelFormat32bppARGB, &bd) == gp::Ok) {
                for (int y = 0; y < it.th; ++y) memcpy((BYTE*)bd.Scan0 + y * bd.Stride, it.thumbPx.data() + (size_t)y * it.tw, (size_t)it.tw * 4);
                b->UnlockBits(&bd);
                it.thumb = b;
            }
        }
        if (it.thumb) {
            gp::GraphicsPath cp; rrPath(cp, tr, 6);
            gp::GraphicsState s2 = gr.Save(); gr.SetClip(&cp, gp::CombineModeIntersect);
            float bw = (float)it.thumb->GetWidth(), bh = (float)it.thumb->GetHeight(), kk = std::max(tr.Width / bw, tr.Height / bh);
            gr.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
            gr.DrawImage(it.thumb.get(), RF(tr.X + (tr.Width - bw * kk) / 2, tr.Y + (tr.Height - bh * kk) / 2, bw * kk, bh * kk));
            gr.Restore(s2);
        }
        if (it.replay) { if (it.thumb) fillRR(gr, tr, 6, gp::Color(110, 0, 0, 0)); icon(gr, G_REPLAY, tr.X + 24, tr.Y + 10, 16, C(it.thumb ? 0xFFFFFF : T_TEXT2)); }
        float nx = tr.X + 76;
        bool renaming = g_edit.kind == 1 && g_edit.path == it.path;
        if (!renaming) txt(gr, it.name, RF(nx, top + 8, rc.dur - nx - 16, 20), F.body, C(T_TEXT), AL_L, true, true);  // keep the date suffix
        std::wstring det = it.replay ? std::wstring(L"Instant replay") : it.w ? dims(it.w, it.h) + (it.fps > 0 ? L" \u00b7 " + std::to_wstring((int)std::lround(it.fps)) + L" fps" : L"") : std::wstring();
        if (det.empty() && !it.metaDone) det = L"\u2026";
        txt(gr, det, RF(nx, top + 28, rc.dur - nx - 16, 18), F.label, C(T_MUTED));
        txt(gr, durText(it.dur), RF(rc.dur, top, 84, 56), F.label, C(T_TEXT2), AL_L, false);
        txt(gr, sizeText(it.size), RF(rc.size, top, 84, 56), F.label, C(T_TEXT2), AL_L, false);
        txt(gr, dateText(it.mtime), RF(rc.date, top, 140, 56), F.label, C(T_TEXT2), AL_L, true);
        bool mkv = it.ext == L"mkv";
        RF bd(rc.fmt, top + 17, 44, 22);
        unsigned bc = mkv ? T_WARN : T_OK;
        fillRR(gr, bd, 11, C(bc, 0x29));
        txt(gr, mkv ? L"MKV" : L"MP4", bd, F.tiny, C(bc), AL_C, false);
        float R = lc.X + lc.Width;
        float pr = remuxProgress(it.path);
        if ((mkv && !hasMp4Sibling(it.path)) || pr > -2) {
            RF rb(pr >= 0 ? R - 146 : R - 126, top + 12, pr >= 0 ? 104.f : 84.f, 32);
            pressScope ps(gr, rb, base + 3);
            buttonBg(gr, rb, 8, base + 3, T_ELEV, WA(A_DEFAULT));
            if (pr >= 0) {  // progress bar: output bytes / input bytes (stream copy)
                RF bar(rb.X + 10, rb.Y + 21, rb.Width - 20, 4);
                fillRR(gr, bar, 2, C(T_TRACK));
                if (pr > 0.005f) fillRR(gr, RF(bar.X, bar.Y, std::max(4.f, bar.Width * pr), bar.Height), 2, C(T_ACC));
                std::wstring pt = L"Remuxing " + std::to_wstring((int)std::lround(pr * 100)) + L"%";
                txt(gr, pt, RF(rb.X, rb.Y - 4, rb.Width, rb.Height - 6), F.caption, C(T_TEXT2), AL_C, false);
            } else if (pr == -1) txt(gr, L"Queued", rb, F.label, C(T_MUTED), AL_C, false);
            else {
                float tw = txtW(gr, L"Remux", F.body), x0 = rb.X + (rb.Width - 22 - tw) / 2;
                icon(gr, G_REMUX, x0, rb.Y + 8, 16, C(T_TEXT2));
                txt(gr, L"Remux", RF(x0 + 22, rb.Y, tw + 4, rb.Height), F.body, C(T_TEXT), AL_L, false);
            }
            addReg(rb, base + 3);
        } else {
            iconBtn(gr, RF(R - 90, top + 12, 32, 32), base + 1, G_PLAY);
            iconBtn(gr, RF(R - 55, top + 12, 32, 32), base + 2, G_FOLDER);
        }
    }
    gr.Restore(st);
    float ch = 44 + vis.size() * 62.f + 6;
    if (ch > lc.Height) {  // scrollbar
        float vh = lc.Height - 50, th = std::max(24.f, vh * vh / (ch - 44)), ty = 148 + (vh - th) * (g_listScroll / std::max(1.f, ch - lc.Height));
        fillRR(gr, RF(lc.X + lc.Width - 6, ty, 3, th), 1.5f, WA(A_STRONG));
    }
}

// ---------- whole window ----------
void paintView(gp::Graphics& gr, int view, float W, float H) {
    if (view == 0) paintRecordView(gr, W, H); else if (view == 1) paintRecordingsView(gr, W, H); else paintSettingsView(gr, W, H);
}
void setupGraphics(gp::Graphics& gr, bool clearType) {
    gr.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    gr.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    gr.SetTextRenderingHint(clearType ? gp::TextRenderingHintClearTypeGridFit : gp::TextRenderingHintAntiAliasGridFit);
    gr.SetCompositingQuality(gp::CompositingQualityHighSpeed);
}
gp::Bitmap* g_themeFade = nullptr; Anim g_themeAnim;
void paintAll(gp::Graphics& gr, float W, float H) {
    g_regs.clear(); g_meterRects.clear(); g_tooltip.t.clear(); g_truncs.clear();
    g_collectTruncs = true;
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
    g_collectTruncs = false;
    if (g_tooltip.t.empty() && P.kind == PK_NONE) {  // full text of a truncated label under the mouse
        for (auto& tt : g_truncs) if (tt.r.Contains(g_mouseX, g_mouseY)) {
            float tw = std::min(txtW(gr, tt.t, F.caption) + 20, W - 16);
            float x = std::max(8.f, std::min(tt.r.X, W - 8 - tw)), y = tt.r.Y + tt.r.Height + 4;
            if (y + 24 > H - 4) y = tt.r.Y - 28;
            RF tr(x, y, tw, 24);
            fillRR(gr, tr, 6, C(g_light ? 0x14161A : 0x2A303A));
            txt(gr, tt.t, RF(tr.X + 10, tr.Y, tr.Width - 20, tr.Height), F.caption, C(g_light ? 0xFFFFFF : T_TEXT), AL_L, true, true);
            break;
        }
    }
    if (!g_tooltip.t.empty()) {  // hotkey conflict tooltip ("Already used by ...")
        float tw = txtW(gr, g_tooltip.t, F.caption) + 20;
        RF tr(g_tooltip.r.X + g_tooltip.r.Width - tw, g_tooltip.r.Y - 30, tw, 24);
        fillRR(gr, tr, 6, C(g_light ? 0x14161A : 0x2A303A));
        txt(gr, g_tooltip.t, tr, F.caption, C(g_light ? 0xFFFFFF : T_TEXT), AL_C, false);
        g_tooltip.t.clear();
    }
    if (g_themeFade && g_themeAnim.active()) {  // theme / accent switch: the old frame fades out on top (200 ms)
        float a = 1 - g_themeAnim.value();
        gp::ColorMatrix m = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, a, 0}, {0, 0, 0, 0, 1}}};
        gp::ImageAttributes ia; ia.SetColorMatrix(&m);
        gr.DrawImage(g_themeFade, RF(0, 0, W, H), 0, 0, (gp::REAL)g_themeFade->GetWidth(), (gp::REAL)g_themeFade->GetHeight(), gp::UnitPixel, &ia);
    } else if (g_themeFade) { delete g_themeFade; g_themeFade = nullptr; }
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
                    if (s.kind == SR_WIN && !s.app.empty()) appTitle(gr, s.app, s.page, RF(r.X + 36, r.Y, rx - r.X - 44, r.Height), F.body, C(T_TEXT), C(T_MUTED));
                    else txt(gr, s.label, RF(r.X + 36, r.Y, rx - r.X - 44, r.Height), F.body, C(T_TEXT));
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
// ---------- layered cards: dropdowns, popovers, toasts ----------
// Pixel-exact compositing: the card sits on whole device pixels (no half-pixel blur from a fractional margin), its
// body is painted on an opaque background so text gets ClearType like the main window, then it is clipped to the
// rounded rect by an anti-aliased texture fill. The shadow is a real blur (3 box passes ~ gaussian), cached per
// size/DPI. Animation frames don't repaint: they only move the layered window and change its alpha.
struct LayerSurf {
    HDC mem = nullptr; HBITMAP hb = nullptr; HGDIOBJ old = nullptr; uint32_t* px = nullptr; int w = 0, h = 0;
    bool ensure(int pw, int ph) {
        if (hb && pw == w && ph == h) return true;
        release();
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = pw; bi.bmiHeader.biHeight = -ph;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        HDC sdc = GetDC(nullptr); mem = CreateCompatibleDC(sdc); ReleaseDC(nullptr, sdc);
        void* bits = nullptr;
        hb = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hb) { release(); return false; }
        old = SelectObject(mem, hb); px = (uint32_t*)bits; w = pw; h = ph;
        return true;
    }
    void release() {
        if (mem) { if (old) SelectObject(mem, old); DeleteDC(mem); }
        if (hb) DeleteObject(hb);
        mem = nullptr; hb = nullptr; old = nullptr; px = nullptr; w = h = 0;
    }
    ~LayerSurf() { release(); }
    void present(HWND wnd, POINT pos, BYTE alpha) const {
        BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA}; POINT src{0, 0}; SIZE sz{w, h};
        UpdateLayeredWindow(wnd, nullptr, &pos, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    }
    // Same pixels, new position / opacity: what every animation frame does (no GDI+ work at all).
    static void move(HWND wnd, POINT pos, BYTE alpha) {
        BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        UpdateLayeredWindow(wnd, nullptr, &pos, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
    }
};
// Blurred rounded-rect shadow (premultiplied black), (cw + 2m) x (ch + 2m) device px.
struct ShadowCache { int cw = 0, ch = 0, m = 0, rad = 0, blur = 0, dy = 0, alpha = 0; std::vector<uint32_t> px; };
const std::vector<uint32_t>& shadowFor(int cw, int ch, int m, int radPx, int blurPx, int dyPx, int alpha) {
    static ShadowCache cache[6]; static int next = 0;
    for (auto& c : cache) if (c.cw == cw && c.ch == ch && c.m == m && c.rad == radPx && c.blur == blurPx && c.dy == dyPx && c.alpha == alpha && !c.px.empty()) return c.px;
    ShadowCache& c = cache[next++ % 6];
    c.cw = cw; c.ch = ch; c.m = m; c.rad = radPx; c.blur = blurPx; c.dy = dyPx; c.alpha = alpha;
    const int W = cw + 2 * m, H = ch + 2 * m;
    std::vector<float> a((size_t)W * H, 0.f), t((size_t)W * H);
    {   // coverage mask of the card shape (anti-aliased), offset down by dy
        gp::Bitmap mb(W, H, PixelFormat32bppARGB);
        { gp::Graphics g2(&mb); g2.SetSmoothingMode(gp::SmoothingModeAntiAlias); g2.SetPixelOffsetMode(gp::PixelOffsetModeHalf); g2.Clear(gp::Color(0, 0, 0, 0)); fillRR(g2, RF((float)m, (float)(m + dyPx), (float)cw, (float)ch), (float)radPx, gp::Color(255, 0, 0, 0)); }
        gp::BitmapData bd; gp::Rect rr(0, 0, W, H);
        if (mb.LockBits(&rr, gp::ImageLockModeRead, PixelFormat32bppARGB, &bd) == gp::Ok) {
            for (int y = 0; y < H; ++y) { const uint32_t* row = (const uint32_t*)((BYTE*)bd.Scan0 + (size_t)y * bd.Stride); for (int x = 0; x < W; ++x) a[(size_t)y * W + x] = (row[x] >> 24) / 255.f; }
            mb.UnlockBits(&bd);
        }
    }
    const int r = std::max(1, (int)std::lround(blurPx / 3.0));  // 3 box passes of radius r ~ gaussian sigma ~ blur/3
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < H; ++y) {  // horizontal
            float acc = 0; const float* in = &a[(size_t)y * W]; float* out = &t[(size_t)y * W];
            for (int x = -r; x <= r; ++x) acc += (x >= 0 && x < W) ? in[x] : 0.f;
            for (int x = 0; x < W; ++x) { out[x] = acc / (2 * r + 1); int xo = x - r, xi = x + r + 1; if (xo >= 0) acc -= in[xo]; if (xi < W) acc += in[xi]; }
        }
        for (int x = 0; x < W; ++x) {  // vertical
            float acc = 0;
            for (int y = -r; y <= r; ++y) acc += (y >= 0 && y < H) ? t[(size_t)y * W + x] : 0.f;
            for (int y = 0; y < H; ++y) { a[(size_t)y * W + x] = acc / (2 * r + 1); int yo = y - r, yi = y + r + 1; if (yo >= 0) acc -= t[(size_t)yo * W + x]; if (yi < H) acc += t[(size_t)yi * W + x]; }
        }
    }
    c.px.assign((size_t)W * H, 0);
    for (size_t i = 0; i < c.px.size(); ++i) { uint32_t al = (uint32_t)std::lround(std::min(1.f, a[i]) * alpha); c.px[i] = al << 24; }
    return c.px;
}
struct CardStyle { float radius = 10; unsigned bg = 0; gp::Color border{0, 0, 0, 0}; int blur = 16, dy = 6, shadowA = 0x66; };
// Composes a card into a premultiplied pixel buffer `dst` (pw x ph, origin = the buffer's top-left). The card's
// top-left sits at (mPx, mPx). `body` paints in logical units (card-local), on an opaque style.bg background.
// `under` (optional) paints in logical card-local units before the card fill (e.g. the mic popover's arrow).
void composeCard(uint32_t* dst, int pw, int ph, int mPx, float s, float cwL, float chL, const CardStyle& st,
                 const std::function<void(gp::Graphics&)>& body, const std::function<void(gp::Graphics&)>& under = nullptr) {
    const int cw = std::max(1, (int)std::lround(cwL * s)), ch = std::max(1, (int)std::lround(chL * s));
    const int radPx = (int)std::lround(st.radius * s);
    const auto& sh = shadowFor(cw, ch, mPx, radPx, (int)std::lround(st.blur * s), (int)std::lround(st.dy * s), st.shadowA);
    const int sw = cw + 2 * mPx, shh = ch + 2 * mPx;
    for (int y = 0; y < ph; ++y) {
        uint32_t* row = dst + (size_t)y * pw;
        if (y < shh) { int n = std::min(pw, sw); memcpy(row, &sh[(size_t)y * sw], (size_t)n * 4); if (pw > n) memset(row + n, 0, (size_t)(pw - n) * 4); }
        else memset(row, 0, (size_t)pw * 4);
    }
    // body: opaque, ClearType text (GDI+ on a DIB DC, exactly like the main window)
    LayerSurf b;
    if (!b.ensure(cw, ch)) return;
    {
        gp::Graphics bg(b.mem);
        setupGraphics(bg, true);
        gp::SolidBrush fill(C(st.bg)); bg.FillRectangle(&fill, -1, -1, cw + 2, ch + 2);
        bg.ScaleTransform(s, s);
        body(bg);
    }
    for (size_t i = 0, n = (size_t)cw * ch; i < n; ++i) b.px[i] |= 0xFF000000u;  // GDI leaves alpha undefined
    gp::Bitmap bodyBmp(cw, ch, cw * 4, PixelFormat32bppPARGB, (BYTE*)b.px);
    gp::Bitmap out(pw, ph, pw * 4, PixelFormat32bppPARGB, (BYTE*)dst);
    gp::Graphics g(&out);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    g.SetCompositingQuality(gp::CompositingQualityHighQuality);
    if (under) {
        gp::GraphicsState gs = g.Save();
        g.TranslateTransform((float)mPx, (float)mPx); g.ScaleTransform(s, s);
        under(g);
        g.Restore(gs);
    }
    // card: anti-aliased rounded rect filled with the body texture (pixel-aligned: no resampling of the text)
    gp::TextureBrush tb(&bodyBmp, gp::WrapModeClamp);
    tb.TranslateTransform((float)mPx, (float)mPx);
    gp::GraphicsPath path; rrPath(path, RF((float)mPx, (float)mPx, (float)cw, (float)ch), (float)radPx);
    g.FillPath(&tb, &path);
    // 1 px hairline border on the device pixel grid
    gp::GraphicsPath bp; rrPath(bp, RF(mPx + 0.5f, mPx + 0.5f, cw - 1.f, ch - 1.f), std::max(0.f, radPx - 0.5f));
    gp::Pen pen(st.border, 1.f); g.DrawPath(&pen, &bp);
}
// Vsync-timed animation clock: a helper thread waits on DwmFlush (the compositor's frame) and posts one
// WM_APP_VSYNC at a time while something animates; paced to every Nth frame of the panel's refresh rate.
constexpr UINT WM_APP_VSYNC = WM_APP + 77;
std::atomic<bool> g_vsyncWant{false}, g_vsyncPosted{false};
HANDLE g_vsyncEv = nullptr;
void vsyncKick() {
    if (!g.hwnd || g_renderOnly) return;
    static bool started = false;
    if (!started) {
        started = true;
        g_vsyncEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::thread([] {
            // Every Nth compositor frame, N = floor(refresh / 60): an even cadence at the panel's own rhythm
            // (60/75 Hz -> every frame, 120/144/165 Hz -> every 2nd = 60/72/82 fps, 240 Hz -> every 4th).
            UINT n = 0, div = 1; ULONGLONG rateAt = 0;
            for (;;) {
                if (!g_vsyncWant.load()) { WaitForSingleObject(g_vsyncEv, INFINITE); n = 0; continue; }
                ULONGLONG t = GetTickCount64();
                if (t - rateAt > 1000) {  // the refresh rate can change (monitor switch, VRR / power plan)
                    rateAt = t;
                    DWM_TIMING_INFO ti{}; ti.cbSize = sizeof ti;
                    double hz = 0;
                    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateRefresh.uiDenominator)
                        hz = (double)ti.rateRefresh.uiNumerator / ti.rateRefresh.uiDenominator;
                    if (hz < 1) { DEVMODEW dm{}; dm.dmSize = sizeof dm; if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) hz = dm.dmDisplayFrequency; }
                    div = hz >= 1 ? std::max(1u, (UINT)((hz + 1.0) / 60.0)) : 1;
                }
                if (FAILED(DwmFlush())) { Sleep(16); n = 0; }  // composition off / remote session: plain ~60 Hz timing
                else if (++n % div) continue;
                if (!g_vsyncPosted.exchange(true)) PostMessageW(g.hwnd, WM_APP_VSYNC, 0, 0);
            }
        }).detach();
    }
    if (!g_vsyncWant.exchange(true)) SetEvent(g_vsyncEv);
}

// Plain layered paint (preview chips): logical-unit drawing into a fresh surface.
void ulw(HWND h, int pw, int ph, POINT* pos, BYTE alpha, const std::function<void(gp::Graphics&)>& draw) {
    if (pw <= 0 || ph <= 0) return;
    LayerSurf sf;
    if (!sf.ensure(pw, ph)) return;
    memset(sf.px, 0, (size_t)pw * ph * 4);
    {
        gp::Bitmap bmp(pw, ph, pw * 4, PixelFormat32bppPARGB, (BYTE*)sf.px);
        gp::Graphics gr(&bmp);
        setupGraphics(gr, false);
        draw(gr);
    }
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    POINT src{0, 0}; SIZE sz{pw, ph};
    UpdateLayeredWindow(h, nullptr, pos, &sz, sf.mem, &src, 0, &bf, ULW_ALPHA);
}
LayerSurf g_popSurf;
bool g_popDirty = true;  // content changed since the last compose
void paintPopupArrow(gp::Graphics& gr) {  // mic popover: 8 px arrow pointing at the chip
    gp::PointF tri[3] = {gp::PointF(P.w / 2 - 8, P.h - 1), gp::PointF(P.w / 2 + 8, P.h - 1), gp::PointF(P.w / 2, P.h + 7)};
    gp::SolidBrush ab(C(T_ELEV)); gr.FillPolygon(&ab, tri, 3);
    gp::Pen ap(WA(A_DEFAULT), 1.f); gr.DrawLine(&ap, tri[0].X, tri[0].Y + 0.5f, tri[2].X, tri[2].Y); gr.DrawLine(&ap, tri[2].X, tri[2].Y, tri[1].X, tri[1].Y + 0.5f);
}
CardStyle popupStyle() { CardStyle st; st.radius = 10; st.bg = T_ELEV; st.border = WA(A_DEFAULT); st.blur = 16; st.dy = 6; st.shadowA = g_light ? 0x29 : 0x66; return st; }
// Popup card into a buffer: used by the layered window and by the headless renders (same pixels).
void composePopup(uint32_t* dst, int pw, int ph, int mPx, float s) {
    composeCard(dst, pw, ph, mPx, s, P.w, P.h, popupStyle(), [](gp::Graphics& gr) { paintPopupContent(gr); },
                P.kind == PK_MIC ? std::function<void(gp::Graphics&)>(paintPopupArrow) : nullptr);
}
void popupFrame();
void renderPopup() { g_popDirty = true; popupFrame(); }
void popupFrame() {
    if (!P.hwnd || P.kind == PK_NONE || !g.hwnd) return;
    const float s = scale();
    const int mPx = (int)std::lround(kShadow * s);
    const bool up = P.kind == PK_MIC || P.above;
    float o, ty;
    if (P.closing) { float e = (float)ease(E_IN, P.close.progress()); o = 1 - e; ty = (up ? 4.f : -4.f) * e; }
    else { float e = P.open.value(); o = std::max(0.f, std::min(1.f, e)); ty = (up ? 6.f : -6.f) * (1 - e); }
    const int pw = (int)std::lround(P.w * s) + 2 * mPx, ph = (int)std::lround(P.h * s) + 2 * mPx;
    POINT pt{(LONG)std::lround(P.x * s) - mPx, (LONG)std::lround((P.y + ty) * s) - mPx};
    ClientToScreen(g.hwnd, &pt);
    const BYTE a = (BYTE)std::lround(255 * o);
    if (g_popDirty || g_popSurf.w != pw || g_popSurf.h != ph) {
        if (!g_popSurf.ensure(pw, ph)) return;
        composePopup(g_popSurf.px, pw, ph, mPx, s);
        g_popSurf.present(P.hwnd, pt, a);
        g_popDirty = false;
    } else LayerSurf::move(P.hwnd, pt, a);
}
int popupHit(float x, float y) {  // card-local logical coords
    for (auto it = P.regs.rbegin(); it != P.regs.rend(); ++it) if (inRF(it->r, x, y)) return it->id;
    return inRF(RF(0, 0, P.w, P.h), x, y) ? P_PAD : -1;
}
void popupSetVolFromX(float x) { setMicVolume((int)std::lround((x - 16) / 200.f * 200.f), false); }
LRESULT CALLBACK PopupProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const float s = scale();
    const int mPx = (int)std::lround(kShadow * s);
    auto local = [&](float& x, float& y) { x = (GET_X_LPARAM(lp) - mPx) / s; y = (GET_Y_LPARAM(lp) - mPx) / s; };
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

// ---------- toasts: own top-level layered windows, excluded from capture (SPEC "Toasts") ----------
// On-screen toasts sit at the top-right of the primary monitor, topmost over games, click-through, ~1 s then a glide out.
// The in-app "Recording saved" toast sits 16 px inside the preview's top-right with Open / Show in folder.
void kickToasts();
struct Toast {
    HWND hwnd = nullptr;
    int kind = TK_STARTED;
    bool inApp = false, closing = false, hover = false, excluded = false;
    std::wstring title, sub, path;
    float w = 284, h = 64, x = 0, y = 0;
    double holdMs = 1000, shownAt = 0, hoverAt = 0, pausedMs = 0;
    Anim in, out;
    int hot = 0, pressed = 0;  // H_TOAST_* under the mouse
    bool testVisible = false;  // --test-toast-visible: no capture exclusion (CI control)
    LayerSurf surf;            // composed once; animation frames only move it / change its alpha
    bool dirty = true, animLast = false;
};
std::vector<std::unique_ptr<Toast>> g_toasts;
bool g_affinityWarned = false;
constexpr DWORD kWdaExclude = 0x11;  // WDA_EXCLUDEFROMCAPTURE (Windows 10 2004+)
// Replay / status toast (SPEC "Toasts", mockup-replay-toast.png): 32 px accent-tinted circle with the icon at
// (16, centre), title Semibold 13 and the file name muted 12 at x 58, in-app close button 24 px centred vertically.
void paintToastBody(gp::Graphics& gr, const Toast& t) {
    if (t.kind == TK_TEST) { gp::SolidBrush b(gp::Color(255, 255, 0, 255)); gr.FillRectangle(&b, RF(-1, -1, t.w + 2, t.h + 2)); return; }
    if (t.kind == TK_SAVED) {
        icon(gr, G_CHECKCIRCLE, 16, 16, 20, C(T_OK));
        txt(gr, t.title, RF(46, 12, t.w - 46 - 40, 28), F.title, C(T_TEXT));
        txt(gr, t.sub, RF(46, 36, t.w - 46 - 16, 20), F.label, C(T_MUTED), AL_L, true, true);  // middle ellipsis keeps the size
        if (t.inApp) {
            RF ob(46, 70, 84, 30), fb(138, 70, 148, 30);
            for (int k = 0; k < 2; ++k) {
                RF b = k ? fb : ob; int id = k ? H_TOAST_FOLDER : H_TOAST_OPEN;
                fillRR(gr, b, 8, mix(T_INPUT, T_INPUT_H, t.hot == id ? 1.f : 0.f));
                strokeRR(gr, b, 8, WA(A_DEFAULT));
                const wchar_t* lbl = k ? L"Show in folder" : L"Open";
                float tw = txtW(gr, lbl, F.body), x0 = b.X + (b.Width - 22 - tw) / 2;
                icon(gr, k ? G_FOLDER : G_PLAY, x0, b.Y + 7, 16, C(T_TEXT2));
                txt(gr, lbl, RF(x0 + 22, b.Y, tw + 4, b.Height), F.body, C(T_TEXT), AL_L, false);
            }
        }
    } else {
        unsigned ac = t.kind == TK_ERROR ? T_ERR : T_ACC;
        float cy = t.h / 2;
        fillCircle(gr, 32, cy, 16, C(ac, 0x29));
        int gl = t.kind == TK_REPLAY ? G_REPLAY : t.kind == TK_STARTED ? G_RECORD : t.kind == TK_STOPPED ? G_STOP : G_LOG;
        icon(gr, gl, 24, cy - 8, 16, C(t.kind == TK_ERROR ? T_ERR : T_ACC_B));
        const float right = t.inApp ? 46.f : 16.f;
        if (t.sub.empty()) txt(gr, t.title, RF(58, 0, t.w - 58 - right, t.h), F.title, C(T_TEXT));
        else {
            txt(gr, t.title, RF(58, cy - 19, t.w - 58 - right, 20), F.title, C(T_TEXT));
            txt(gr, t.sub, RF(58, cy + 1, t.w - 58 - right, 18), F.label, C(T_MUTED), AL_L, true, true);
        }
    }
    if (t.inApp) {
        RF cb = t.kind == TK_SAVED ? RF(t.w - 34, 10, 24, 24) : RF(t.w - 36, (t.h - 24) / 2, 24, 24);
        fillRR(gr, cb, 6, WA(t.hot == H_TOAST_CLOSE ? A_STRONG : A_HOVER));
        icon(gr, G_CLOSE, cb.X + 4, cb.Y + 4, 16, C(t.hot == H_TOAST_CLOSE ? T_TEXT : T_MUTED));
    }
}
CardStyle toastStyle(const Toast& t) {
    CardStyle st; st.radius = 14; st.bg = T_ELEV; st.blur = 22; st.dy = 8; st.shadowA = g_light ? 0x29 : 0x70;
    st.border = t.kind == TK_SAVED ? C(T_OK, 140) : t.kind == TK_ERROR ? C(T_ERR, 140) : WA(A_DEFAULT);
    if (t.kind == TK_TEST) { st.shadowA = 0; st.radius = 0; st.border = gp::Color(255, 255, 0, 255); st.bg = 0xFF00FF; }
    return st;
}
// Kept for callers that paint a toast into an existing Graphics (logical units, card at 0,0): body only.
void paintToast(gp::Graphics& gr, const Toast& t) { paintToastBody(gr, t); }
int toastHit(const Toast& t, float x, float y) {
    if (!t.inApp) return 0;
    RF cb = t.kind == TK_SAVED ? RF(t.w - 34, 10, 24, 24) : RF(t.w - 36, (t.h - 24) / 2, 24, 24);
    if (inRF(cb, x, y)) return H_TOAST_CLOSE;
    if (t.kind == TK_SAVED && inRF(RF(46, 70, 84, 30), x, y)) return H_TOAST_OPEN;
    if (t.kind == TK_SAVED && inRF(RF(138, 70, 148, 30), x, y)) return H_TOAST_FOLDER;
    return 0;
}
const float kToastM = 32.f;  // shadow margin (blur 22 + offset 8)
float toastScale() { return g.dpi / 96.f; }
void layoutToasts() {
    // on-screen: primary monitor work area, top-right, stacking downward 8 px apart
    MONITORINFO mi{sizeof mi}; GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const float s = toastScale();
    float sy = mi.rcWork.top / s + 16, ay = 0;
    RecLayout L = recLayout(clientW(), clientH());
    POINT org{0, 0}; if (g.hwnd) ClientToScreen(g.hwnd, &org);
    ay = org.y / s + L.surf.Y + 46;  // below the preview's resolution pill
    for (auto& t : g_toasts) {
        if (t->inApp) { t->x = org.x / s + L.surf.X + L.surf.Width - 16 - t->w; t->y = ay; ay += t->h + 8; }
        else { t->x = mi.rcWork.right / s - 16 - t->w; t->y = sy; sy += t->h + 8; }
    }
}
void composeToast(uint32_t* dst, int pw, int ph, int mPx, float s, const Toast& t) {
    composeCard(dst, pw, ph, mPx, s, t.w, t.h, toastStyle(t), [&](gp::Graphics& gr) { paintToastBody(gr, t); });
}
// One animation frame: compose only when the content changed, otherwise move + fade the existing pixels.
void toastFrame(Toast& t) {
    if (!t.hwnd) return;
    const float s = toastScale();
    const int mPx = (int)std::lround(kToastM * s);
    float o, dx = 0, dy = 0;
    if (t.closing) {
        float e = (float)ease(E_IN, t.out.progress()); o = 1 - e;
        if (!t.inApp) dx = 28 * e;  // on-screen: glide out to the right (SPEC); in-app: fade (tokens toastOut)
    } else { float e = t.in.value(); o = std::max(0.f, std::min(1.f, e)); dy = -8 * (1 - e); }
    const int pw = (int)std::lround(t.w * s) + 2 * mPx, ph = (int)std::lround(t.h * s) + 2 * mPx;
    POINT pt{(LONG)std::lround((t.x + dx) * s) - mPx, (LONG)std::lround((t.y + dy) * s) - mPx};
    const BYTE a = (BYTE)std::lround(255 * o);
    if (t.dirty || t.surf.w != pw || t.surf.h != ph) {
        if (!t.surf.ensure(pw, ph)) return;
        composeToast(t.surf.px, pw, ph, mPx, s, t);
        t.surf.present(t.hwnd, pt, a);
        t.dirty = false;
    } else LayerSurf::move(t.hwnd, pt, a);
}
void renderToast(Toast& t) { t.dirty = true; toastFrame(t); }
void dismissToast(Toast* t) { if (!t->closing) { t->closing = true; t->out.set(0); t->out.go(1, t->inApp ? 160 : 260, E_IN); if (!t->out.ms) t->out.set(1); kickToasts(); } }
void openPath(const std::wstring& p) { ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
void showInFolder(const std::wstring& p) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(p.c_str());
    if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0); ILFree(pidl); }
    else ShellExecuteW(nullptr, L"open", outputDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
LRESULT CALLBACK ToastProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Toast* t = nullptr;
    for (auto& x : g_toasts) if (x->hwnd == h) t = x.get();
    if (!t) return DefWindowProcW(h, m, wp, lp);
    const float s = toastScale();
    const int mPx = (int)std::lround(kToastM * s);
    float x = (GET_X_LPARAM(lp) - mPx) / s, y = (GET_Y_LPARAM(lp) - mPx) / s;
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_NCHITTEST: return t->inApp ? HTCLIENT : HTTRANSPARENT;
        case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, t->hot ? IDC_HAND : IDC_ARROW)); return TRUE;
        case WM_MOUSEMOVE: {
            if (!t->hover) { t->hover = true; t->hoverAt = nowMs(); TRACKMOUSEEVENT tm{sizeof tm, TME_LEAVE, h, 0}; TrackMouseEvent(&tm); }
            int id = toastHit(*t, x, y);
            if (id != t->hot) { t->hot = id; renderToast(*t); }
            return 0;
        }
        case WM_MOUSELEAVE: if (t->hover) { t->pausedMs += nowMs() - t->hoverAt; t->hover = false; } t->hot = 0; renderToast(*t); return 0;
        case WM_LBUTTONDOWN: t->pressed = toastHit(*t, x, y); return 0;
        case WM_LBUTTONUP: {
            int id = toastHit(*t, x, y);
            if (id && id == t->pressed) {
                if (id == H_TOAST_OPEN) openPath(t->path);
                else if (id == H_TOAST_FOLDER) showInFolder(t->path);
                dismissToast(t);
            }
            t->pressed = 0;
            return 0;
        }
    }
    return DefWindowProcW(h, m, wp, lp);
}
bool captureLive() { return g.rec.isActive() || g.replay.running(); }
// Toast windows are created ahead of time (startup) and recycled: showing a toast is a compose + one
// UpdateLayeredWindow, nothing is created on the save path.
std::vector<HWND> g_toastPool;
HWND toastWindowCreate() {
    static bool reg = false;
    if (!reg) { WNDCLASSW wc{}; wc.lpfnWndProc = ToastProc; wc.hInstance = g.inst; wc.lpszClassName = L"CastRecorderToast"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&wc); reg = true; }
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, L"CastRecorderToast", L"", WS_POPUP,
                           0, 0, 1, 1, nullptr, nullptr, g.inst, nullptr);
}
void toastWindowRelease(HWND h) {
    if (!h) return;
    ShowWindow(h, SW_HIDE);
    if (g_toastPool.size() < 3) g_toastPool.push_back(h); else DestroyWindow(h);
}
void prewarmToasts() {
    if (g_renderOnly || !g.hwnd) return;
    while (g_toastPool.size() < 2) { HWND h = toastWindowCreate(); if (!h) break; g_toastPool.push_back(h); }
    // shadows for the common sizes at this DPI (the blur is the only non-trivial part of a compose)
    const float s = toastScale(); const int mPx = (int)std::lround(kToastM * s);
    Toast t; t.kind = TK_REPLAY; t.w = 284; t.h = 64;
    CardStyle st = toastStyle(t);
    shadowFor((int)std::lround(284 * s), (int)std::lround(64 * s), mPx, (int)std::lround(st.radius * s), (int)std::lround(st.blur * s), (int)std::lround(st.dy * s), st.shadowA);
}
Toast* showToast(int kind, const std::wstring& title, const std::wstring& sub, const std::wstring& path, bool inApp, bool testVisible = false) {
    if (g_renderOnly) return nullptr;
    auto t = std::make_unique<Toast>();
    t->kind = kind; t->title = title; t->sub = sub; t->path = path; t->inApp = inApp; t->testVisible = testVisible;
    if (kind == TK_SAVED) { t->w = 320; t->h = inApp ? 116 : 64; t->holdMs = inApp ? 6000 : 1500; }
    else if (kind == TK_ERROR) { t->w = 320; t->h = 64; t->holdMs = inApp ? 6000 : 2500; }
    else if (kind == TK_TEST) { t->w = 240; t->h = 120; t->holdMs = 60000; }
    else if (kind == TK_REPLAY) { t->w = 284; t->h = 64; t->holdMs = inApp ? 3000 : 1000; }  // tokens toastDurationMs.replay (in-app)
    else { t->w = sub.empty() ? 240 : 284; t->h = sub.empty() ? 52 : 64; t->holdMs = 1000; }
    if (!g_toastPool.empty() && !testVisible) { t->hwnd = g_toastPool.back(); g_toastPool.pop_back(); }
    else t->hwnd = toastWindowCreate();
    if (!t->hwnd) return nullptr;
    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | (inApp ? 0 : WS_EX_TRANSPARENT);
    SetWindowLongPtrW(t->hwnd, GWL_EXSTYLE, ex);
    SetWindowLongPtrW(t->hwnd, GWLP_HWNDPARENT, (LONG_PTR)(inApp ? g.hwnd : nullptr));
    t->excluded = testVisible ? (SetWindowDisplayAffinity(t->hwnd, 0), false) : SetWindowDisplayAffinity(t->hwnd, kWdaExclude) != 0;
    if (!t->excluded && !testVisible) {
        if (!g_affinityWarned) { cr::appLog(L"toast: WDA_EXCLUDEFROMCAPTURE unavailable (" + std::to_wstring(GetLastError()) + L"); toasts are skipped while capturing"); g_affinityWarned = true; }
        if (captureLive()) { toastWindowRelease(t->hwnd); return nullptr; }  // never let a toast end up in a recording
    }
    while (g_toasts.size() >= 4) { toastWindowRelease(g_toasts.front()->hwnd); g_toasts.erase(g_toasts.begin()); }
    t->in.set(0); t->in.go(1, 220, E_OUTEXPO); if (!t->in.ms || kind == TK_TEST) t->in.set(1);
    t->shownAt = nowMs();
    Toast* raw = t.get();
    g_toasts.push_back(std::move(t));
    layoutToasts();
    for (auto& x : g_toasts) if (x.get() != raw) toastFrame(*x);  // others only move (stacking)
    renderToast(*raw);
    ShowWindow(raw->hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(raw->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    kickToasts();
    return raw;
}
// Runs on vsync frames while something animates and on a one-shot timer for the hold deadlines.
// Returns true while a toast is animating (the vsync clock keeps running).
bool toastTick() {
    double now = nowMs();
    bool animating = false;
    double nextDue = -1;
    for (size_t i = 0; i < g_toasts.size();) {
        Toast& t = *g_toasts[i];
        const double due = t.shownAt + t.pausedMs + 220 + t.holdMs;
        if (!t.closing && !t.hover && now > due) dismissToast(&t);
        if (t.closing && !t.out.active()) { toastWindowRelease(t.hwnd); g_toasts.erase(g_toasts.begin() + i); layoutToasts(); for (auto& x : g_toasts) toastFrame(*x); continue; }
        const bool act = t.in.active() || t.out.active();
        if (act || t.animLast) toastFrame(t);  // one more frame after the end lands exactly on the final state
        t.animLast = act;
        animating |= act;
        if (!t.closing && !t.hover) nextDue = nextDue < 0 ? due : std::min(nextDue, due);
        ++i;
    }
    if (g.hwnd) {
        if (nextDue >= 0) SetTimer(g.hwnd, TIMER_TOAST, (UINT)std::max(10.0, nextDue - now + 1), nullptr);
        else KillTimer(g.hwnd, TIMER_TOAST);
    }
    if (animating) vsyncKick();
    return animating;
}
void kickToasts() { vsyncKick(); }
void relayoutToasts() { if (g_toasts.empty()) return; layoutToasts(); for (auto& t : g_toasts) renderToast(*t); }
bool appOnScreen() { return g.hwnd && IsWindowVisible(g.hwnd) && !IsIconic(g.hwnd); }
// Notification policy (notify_policy.h): status line only while Cast Recorder is open and in front; one on-screen
// toast when it is minimized or another window is in front. Never a Windows notification.
bool foregroundIsOurs() {
    HWND f = GetForegroundWindow(); DWORD pid = 0;
    if (f) GetWindowThreadProcessId(f, &pid);
    return pid == GetCurrentProcessId();
}
void notice(int kind, const std::wstring& title, const std::wstring& sub, const std::wstring& path) {
    if (!g.hwnd || g.quitting) return;
    if (cr::noticeTarget(IsWindowVisible(g.hwnd) != 0, IsIconic(g.hwnd) != 0, foregroundIsOurs()) == cr::NoticeTarget::StatusOnly) {
        cr::appLog(L"toast: \"" + title + L"\" not shown (app is open and in front; status line only)");
        return;
    }
    showToast(kind, title, sub, path, false);
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
    g_meterTh = std::thread([sys, mic, mid, sid] { cr::guardedRun(L"ui-worker", [&] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::unique_ptr<AudioMixer> old;
        { std::lock_guard<std::mutex> l(g_meterM); old = std::move(g_meterMixer); }
        old.reset();
        auto m = std::make_unique<AudioMixer>();
        std::wstring log;
        if (sys || mic) m->start(sys, mic, mid, false, log, sid, false, true);
        { std::lock_guard<std::mutex> l(g_meterM); g_meterMixer = std::move(m); }
        if (SUCCEEDED(co)) CoUninitialize();
    }); });
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
    g_pttTh = std::thread([] { cr::guardedRun(L"ui-worker", [&] {  // held-key polling: works while other apps have focus, no hook needed
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
    }); });
}

// ---------- actions ----------
void switchView(int v) {
    if (v == g_viewIdx) return;
    closePopup(false);
    if (g.capturing >= 0) endCapture();
    g_viewIdx = v;
    g_view.set(0); g_view.go(1, 150, E_OUT);
    g_rsearchFocus = false;
    if (g_edit.hwnd) commitEdit(false);
    g.gprev.setVisible(v == 0 && IsWindowVisible(g.hwnd) && !IsIconic(g.hwnd));
    if (v == 2) { g.mics = listMicrophones(); g.outs = listOutputDevices(); enumGpus(); }
    if (v == 1) scanRecordings();
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
    float f = std::max(0.f, std::min(1.f, (x - g_sliderX) / std::max(1.f, g_sliderW)));
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
    Hotkey* dst = hkSlot(g.capturing);
    UINT mods = currentMods();
    if (isModifierKey(vk)) { g.liveMods = mods; InvalidateRect(g.hwnd, nullptr, FALSE); return true; }
    if (!down && vk != VK_SNAPSHOT) return true;
    if (mods == 0 && vk == VK_ESCAPE) { endCapture(); return true; }
    if (mods == 0 && (vk == VK_BACK || vk == VK_DELETE)) { *dst = Hotkey{}; save(); syncPtt(); endCapture(); return true; }
    Hotkey hk{mods, vk};
    int idx = g.capturing;
    if (keyName(vk).empty()) { setStatus(L"Key not supported", toRef(T_ERR)); return true; }
    int clash = -1;
    for (int k = 0; k < kHkSlots; ++k) if (k != idx && hk == *hkSlot(k)) clash = k;
    Issue is = validate(hk);
    bool bad = idx != 3 && is.severity == Severity::Error;  // push-to-talk is polled, not registered: any held combo works
    if (bad || clash >= 0) {
        g.capturing = -1; g.liveMods = 0;
        registerAll();
        setStatus(W(format(hk)) + (clash >= 0 ? std::wstring(L" is already used by ") + kHkNames[clash] : L": " + W(is.message)), toRef(T_ERR));
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
extern HBITMAP g_back; extern int g_backW, g_backH; extern void* g_backBits;
void themeChanged() {
    if (g_back && g_backBits && animationsOn() && g.hwnd && IsWindowVisible(g.hwnd)) {
        delete g_themeFade;
        g_themeFade = new gp::Bitmap(g_backW, g_backH, PixelFormat32bppRGB);
        gp::BitmapData bd; gp::Rect lr(0, 0, g_backW, g_backH);
        if (g_themeFade->LockBits(&lr, gp::ImageLockModeWrite, PixelFormat32bppRGB, &bd) == gp::Ok) {
            for (int y = 0; y < g_backH; ++y) memcpy((BYTE*)bd.Scan0 + y * bd.Stride, (BYTE*)g_backBits + (size_t)y * g_backW * 4, (size_t)g_backW * 4);
            g_themeFade->UnlockBits(&bd);
        }
        g_themeAnim.set(0); g_themeAnim.go(1, 200, E_LINEAR);
    }
    applyTheme();
    save();
    updateChips();
    relayoutToasts();
    if (g.hwnd) InvalidateRect(g.hwnd, nullptr, TRUE);
}
std::wstring itemPath(int idx) { std::lock_guard<std::mutex> l(g_itemsM); return idx >= 0 && idx < (int)g_items.size() ? g_items[idx].path : std::wstring(); }
void deleteRecording(const std::wstring& path) {
    std::wstring msg = L"Move \u201c" + fileNameOf(path) + L"\u201d to the Recycle Bin?";
    if (MessageBoxW(g.hwnd, msg.c_str(), L"Delete recording", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) return;
    std::wstring from = path; from.push_back(L'\0');  // double-NUL terminated
    SHFILEOPSTRUCTW op{}; op.hwnd = g.hwnd; op.wFunc = FO_DELETE; op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    int rc = SHFileOperationW(&op);
    if (rc || op.fAnyOperationsAborted) setStatus(L"Couldn't delete (the file may be open)", toRef(T_ERR));
    else cr::appLog(L"recordings: moved to Recycle Bin " + path);
    scanRecordings();
}
void startRename(const std::wstring& path) {
    RF r(0, 0, 0, 0); std::wstring name;
    {
        std::lock_guard<std::mutex> l(g_itemsM);
        auto vis = visibleItems();
        for (size_t k = 0; k < vis.size(); ++k) if (g_items[vis[k]].path == path) {
            float W = clientW(), top = 150 + k * 62.f - g_listScroll, nx = recCols(W).name - 2 + 76;
            r = RF(nx, top + 7, recCols(W).dur - nx - 16, 22); name = g_items[vis[k]].name;
        }
    }
    if (r.Width > 0) beginEdit(1, r, name, path);
}
void ctxAction(int, int action) {
    std::wstring p = g_ctxPath;
    if (p.empty()) return;
    switch (action) {
        case 0: openPath(p); break;
        case 1: showInFolder(p); break;
        case 2: if (lowerExt(p) == L"mkv") queueRemux({p}); else setStatus(L"Already an MP4", toRef(T_TEXT2)); break;
        case 3: startRename(p); break;
        case 4: deleteRecording(p); break;
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}
void remuxAll() {
    std::vector<std::wstring> v;
    { std::lock_guard<std::mutex> l(g_itemsM); for (auto& it : g_items) if (it.ext == L"mkv" && !hasMp4Sibling(it.path)) v.push_back(it.path); }
    if (v.empty()) { setStatus(L"No MKV files to remux", toRef(T_TEXT2)); return; }
    queueRemux(v);
}
void click(int id) {
    bool hk = id == H_HK0 || id == H_HK1 || id == H_HK2 || id == H_HKPTT || id == H_HK3;
    if (!hk && g.capturing >= 0) endCapture();
    if (id != H_RSEARCH) g_rsearchFocus = false;
    const RF r = regionOf(id);
    if (id >= R_BASE) {  // Recordings rows: R_BASE + item*4 + part (0 row, 1 play, 2 folder, 3 remux)
        int item = (id - R_BASE) / 4, part = (id - R_BASE) % 4;
        std::wstring p = itemPath(item);
        if (p.empty()) return;
        if (part == 1) openPath(p);
        else if (part == 2) showInFolder(p);
        else if (part == 3 && lowerExt(p) == L"mkv" && remuxProgress(p) == -2) queueRemux({p});
        InvalidateRect(g.hwnd, nullptr, FALSE);
        return;
    }
    switch (id) {
        case H_MIN: ShowWindow(g.hwnd, SW_MINIMIZE); return;
        case H_CLOSE: forceQuit(); return;  // exits; a live recording is stopped and saved first
        case H_MAX: ShowWindow(g.hwnd, IsZoomed(g.hwnd) ? SW_RESTORE : SW_MAXIMIZE); save(); return;
        case H_TAB_REC: switchView(0); return;
        case H_TAB_RECS: switchView(1); return;
        case H_TAB_SET: switchView(2); return;
        case H_LOG: case H_STATUSLOG: openLog(); return;
        case H_RECORD: toggleRecording(); break;
        case H_PAUSE: if (g.rec.isActive()) togglePause(); break;
        case H_REPLAYPILL: saveReplay(); break;
        case H_MICICON: toggleMic(); break;
        case H_SYSCHIP: toggleSys(); break;
        case H_MICCHIP: openPopup(PK_MIC, H_MICCHIP, r); break;
        case H_SRC:
            if (locked(id)) { setStatus(L"Stop recording to change the source", toRef(T_TEXT2)); break; }
            stopReplayBuffer();  // restarted on the new source by selectSource -> startWarmup
            openPopup(PK_SOURCE, H_SRC, r); break;
        case H_DD_FPS: case H_DD_QUAL: case H_DD_ENC: case H_DD_GPU: case H_DD_RES: case H_DD_CONT: case H_DD_SYSDEV: case H_DD_MICDEV:
        case H_DD_REPLAYLEN: case H_DD_FNAME:
            if (locked(id)) { setStatus(L"Stop recording to change this", toRef(T_TEXT2)); break; }
            if (id == H_DD_GPU) enumGpus();
            if (id == H_DD_ENC && g_encChoices.size() <= 1) probeEncoders();
            if (id == H_DD_MICDEV) g.mics = listMicrophones();
            if (id == H_DD_SYSDEV) g.outs = listOutputDevices();
            openPopup(PK_LIST, id, r); break;
        case H_HK0: case H_HK1: case H_HK2: case H_HKPTT: case H_HK3:
            if (locked(id)) { setStatus(L"Stop recording to change hotkeys", toRef(T_TEXT2)); break; }
            g.capturing = id == H_HKPTT ? 3 : id == H_HK3 ? 4 : id - H_HK0;
            g.liveMods = 0;
            unregisterAll();
            SetFocus(g.hwnd);
            break;
        case H_FOLDERICON: openFolder(); break;
        case H_PATH: case H_BROWSE: if (!locked(H_BROWSE)) { pickFolder(); startWatcher(); } break;
        case H_TG_SYS: if (!locked(id)) { g.s.systemAudio = !g.s.systemAudio; save(); restartMeter(); if (g.replay.running()) scheduleReplayRestart(); } break;
        case H_TG_MIC: if (!locked(id)) { g.s.micEnabled = !g.s.micEnabled; save(); restartMeter(); if (g.replay.running()) scheduleReplayRestart(); } break;
        case H_TG_NS: g.s.noiseSuppression = !g.s.noiseSuppression; applyAudioFx(); save(); break;
        case H_TG_MON: g.s.monitoring = !g.s.monitoring; applyAudioFx(); save(); break;
        case H_TG_PTT: g.s.pushToTalk = !g.s.pushToTalk; syncPtt(); save(); break;
        case H_TG_REMUX: case H_TG_AUTOREMUX:
            if (locked(id)) { setStatus(L"Stop recording to change this", toRef(T_TEXT2)); break; }
            g.s.autoRemux = !g.s.autoRemux; save(); encoderSettingsChanged(); break;
        case H_TG_TRACKS:
            if (locked(id)) { setStatus(L"Stop recording to change this", toRef(T_TEXT2)); break; }
            g.s.separateTracks = !g.s.separateTracks; save(); break;
        case H_TG_REPLAY:
            if (locked(id)) { setStatus(L"Stop recording to change this", toRef(T_TEXT2)); break; }
            g.s.replayEnabled = !g.s.replayEnabled; save();
            if (g.s.replayEnabled) { if (g.warm.running()) g.warm.waitUntilIdle(3000); startReplayBuffer(); }
            else { stopReplayBuffer(); g.ring.reset(); }
            break;
        case H_THEME_DARK: case H_THEME_LIGHT: {
            std::string t = id == H_THEME_LIGHT ? "light" : "dark";
            if ((id == H_THEME_LIGHT) != g_light || g.s.theme != t) { g.s.theme = t; themeChanged(); }
            break;
        }
        case H_CH_MONO: case H_CH_STEREO:
            if (!locked(id)) { int c = id == H_CH_MONO ? 1 : 2; if (g.s.channels != c) { g.s.channels = c; save(); if (g.replay.running()) scheduleReplayRestart(); } } break;
        case H_RSEARCH: g_rsearchFocus = true; SetFocus(g.hwnd); kickAnim(); break;
        case H_REMUXALL: remuxAll(); break;
        default:
            if (id >= H_ACC0 && id <= H_ACC5) { int i = id - H_ACC0; if (g.s.accent != kAccents[i].name) { g.s.accent = kAccents[i].name; themeChanged(); } }
            break;
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}
std::vector<int> focusOrder() {
    if (g_viewIdx == 0) {
        std::vector<int> v = {H_TAB_REC, H_TAB_RECS, H_TAB_SET, H_SRC, H_DD_FPS, H_DD_QUAL};
        if (g.s.replayEnabled) v.push_back(H_REPLAYPILL);
        v.insert(v.end(), {H_MICCHIP, H_SYSCHIP});
        if (uiActive()) v.push_back(H_PAUSE);
        v.push_back(H_RECORD);
        return v;
    }
    if (g_viewIdx == 1) return {H_TAB_REC, H_TAB_RECS, H_TAB_SET, H_RSEARCH, H_TG_AUTOREMUX, H_REMUXALL};
    return {H_TAB_REC, H_TAB_RECS, H_TAB_SET, H_DD_ENC, H_DD_GPU, H_DD_RES, H_DD_CONT, H_TG_REMUX, H_TG_REPLAY, H_DD_REPLAYLEN, H_THEME_DARK,
            H_ACC0, H_ACC0 + 1, H_ACC0 + 2, H_ACC0 + 3, H_ACC0 + 4, H_ACC5,
            H_TG_SYS, H_DD_SYSDEV, H_TG_MIC, H_DD_MICDEV, H_SL_VOL, H_TG_NS, H_SL_GATE, H_TG_MON, H_HKPTT, H_TG_PTT, H_CH_MONO, H_TG_TRACKS,
            H_HK0, H_HK1, H_HK2, H_HK3, H_PATH, H_BROWSE, H_LOG, H_DD_FNAME};
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
    bool anim = g_view.active() || g_chan.active() || g_themeSeg.active() || g_themeAnim.active() || anyActive(g_tog) || anyActive(g_hov) || anyActive(g_press);
    if (P.kind != PK_NONE) {
        bool pa = P.open.active() || P.close.active() || anyActive(P.hov);
        if (anyActive(P.hov) || (changed && P.kind == PK_MIC)) renderPopup();      // content changed: recompose
        else if (P.open.active() || P.close.active()) vsyncKick();                  // motion only: vsync-paced moves
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
        if (g_viewIdx == 0 && g_pillRect.Width > 0) invalidateLogical(g_pillRect);  // replay dot pulse / save spinner
        static double lastSlow = 0;
        if (g_viewIdx == 1 && (g_rsearchFocus || remuxBusy()) && t - lastSlow > 100) { lastSlow = t; InvalidateRect(g.hwnd, nullptr, FALSE); }
    }
    if (g.replaySaving && t - g.replaySaveT > 300 && g_pillRect.Width > 0) invalidateLogical(g_pillRect);
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
        for (int i = 0; i < 3; ++i) if (inRF(tabRect(i), x, y)) return HTCLIENT;
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
    case WM_MOVE: if (P.kind != PK_NONE) closePopup(false); relayoutToasts(); break;
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
        relayoutToasts();
        if (g_viewIdx == 1) clampListScroll(clientW(), clientH());
        if (g_edit.hwnd) commitEdit(false);
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
            g.target = CaptureTarget{}; g.target.kind = CaptureTarget::AllDisplays; setSrcWindow(L"", L"", L"");
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
        {   // repaint when the mouse enters / leaves a truncated label (its tooltip)
            auto which = [&](float mx, float my) { for (size_t i = 0; i < g_truncs.size(); ++i) if (g_truncs[i].r.Contains(mx, my)) return (int)i; return -1; };
            int before = which(g_mouseX, g_mouseY);
            g_mouseX = x; g_mouseY = y;
            if (which(x, y) != before) { InvalidateRect(h, nullptr, FALSE); TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t); }
        }
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
    case WM_MOUSELEAVE: g_mouseX = g_mouseY = -1; InvalidateRect(h, nullptr, FALSE); if (g_drag == H_NONE) setHover(H_NONE); return 0;
    case WM_LBUTTONDOWN: {
        float x = GET_X_LPARAM(lp) / s, y = GET_Y_LPARAM(lp) / s;
        int id = hitTest(x, y);
        // a click outside an open popup closes it; clicking its own field again just closes it
        if (P.kind != PK_NONE && !P.closing) {
            int owner = P.dd; closePopup();
            if (id == owner || (owner == H_MICCHIP && id == H_MICCHIP)) { g.pressed = H_NONE; return 0; }
        }
        g.pressed = id;
        if (id != H_NONE && id != H_SL_VOL && id != H_SL_GATE && !locked(id) && !(id == H_PAUSE && !uiActive())) g_press[id].go(1, 70, E_OUT);
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
        if (g.pressed != H_NONE && g_press.count(g.pressed)) g_press[g.pressed].go(0, 160, E_BACK);
        if (id != H_NONE && id == g.pressed) { if (id != H_PAUSE || uiActive()) click(id); }
        else if (id == H_NONE && g.capturing >= 0) endCapture();
        g.pressed = H_NONE;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (P.kind == PK_SOURCE || (P.kind == PK_LIST && P.dd != H_CTXMENU)) { PopupProc(P.hwnd, msg, wp, lp); return 0; }
        if (g_viewIdx == 1) {
            closePopup(false);
            if (g_edit.hwnd) commitEdit(true);
            g_listScroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 62.f;
            clampListScroll(clientW(), clientH());
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_LBUTTONDBLCLK: {
        float x = GET_X_LPARAM(lp) / s, y = GET_Y_LPARAM(lp) / s;
        int id = hitTest(x, y);
        if (g_viewIdx == 1 && id >= R_BASE && (id - R_BASE) % 4 == 0) { std::wstring p = itemPath((id - R_BASE) / 4); if (!p.empty()) openPath(p); return 0; }
        return WndProc(h, WM_LBUTTONDOWN, wp, lp);  // a fast second click on anything else is just a click
    }
    case WM_RBUTTONUP: {
        float x = GET_X_LPARAM(lp) / s, y = GET_Y_LPARAM(lp) / s;
        int id = hitTest(x, y);
        if (g_viewIdx == 1 && id >= R_BASE) {
            closePopup(false);
            g_ctxPath = itemPath((id - R_BASE) / 4);
            if (!g_ctxPath.empty()) openPopup(PK_LIST, H_CTXMENU, RF(x, y, 0, 0));
            return 0;
        }
        break;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, toRef(T_TEXT)); SetBkColor(dc, toRef(T_INPUT));
        if (g_edit.br) DeleteObject(g_edit.br);
        g_edit.br = CreateSolidBrush(toRef(T_INPUT));
        return (LRESULT)g_edit.br;
    }
    case WM_APP + 49: if (g_edit.hwnd && GetFocus() != g_edit.hwnd) commitEdit(true); return 0;
    case WM_SETTINGCHANGE:
        if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0 && g.s.theme == "auto") { applyTheme(); updateChips(); InvalidateRect(h, nullptr, FALSE); }
        break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (hotkeyKey((UINT)wp, true)) return 0;
        if (g_rsearchFocus && msg == WM_KEYDOWN && P.kind == PK_NONE) {
            if (wp == VK_ESCAPE) { if (g_rsearch.empty()) g_rsearchFocus = false; g_rsearch.clear(); g_listScroll = 0; InvalidateRect(h, nullptr, FALSE); return 0; }
            if (wp == VK_BACK) { if (!g_rsearch.empty()) g_rsearch.pop_back(); g_listScroll = 0; InvalidateRect(h, nullptr, FALSE); return 0; }
        }
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
        if (g_rsearchFocus && msg == WM_CHAR && wp >= 32 && wp != 127) { if (g_rsearch.size() < 80) g_rsearch += (wchar_t)wp; g_listScroll = 0; InvalidateRect(h, nullptr, FALSE); return 0; }
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
        else if (wp == HK_REPLAY) saveReplay();
        if (g_hkTestHook) g_hkTestHook((int)wp);
        return 0;
    case WM_APP_VSYNC: {
        g_vsyncPosted = false;
        bool more = toastTick();
        static bool popAnimLast = false;
        const bool pa = P.kind != PK_NONE && (P.open.active() || P.close.active());
        if (pa || (popAnimLast && P.kind != PK_NONE)) popupFrame();
        popAnimLast = pa;
        more |= pa;
        if (!more) g_vsyncWant = false;
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_FRAME) { frameTick(); return 0; }
        if (wp == TIMER_TOAST) { toastTick(); return 0; }
        if (wp == TIMER_RESCAN) { KillTimer(h, wp); scanRecordings(); return 0; }
        if (wp == TIMER_HKTEST) { selftestHotkeysStep(); return 0; }
        if (wp == TIMER_REPLAYRESTART) { replayRestartTimer(); return 0; }
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
            static ULONGLONG lastSize = 0;
            if (g.rec.isActive() && GetTickCount64() - lastSize >= 1000) {
                lastSize = GetTickCount64();
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                std::wstring f = g.rec.currentFile();
                if (!f.empty() && GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fa)) g_recBytes = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            }
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
        startReplayBuffer();
        return 0;
    case WM_APP_ENCODER_SWITCHED: {  // a stalled encoder was replaced live: small status note, not an error
        std::unique_ptr<std::wstring> m((std::wstring*)lp);
        if (m) { cr::appLog(L"encoder: " + *m); if (!g.quitting) setStatus(*m, C_TEXT2); }
        return 0;
    }
    case WM_APP_RECORDER_STARTED:
        if (g.rec.isActive()) { playTone(IDR_SND_START); notice(TK_STARTED, L"Recording started", L"", L""); }
        return 0;
    case WM_APP_REPLAY_DONE: {
        g.replay.waitUntilIdle(200);
        if (g.replay.running()) return 0;
        const RecordResult& r = g.replay.result();
        writeSessionLog(r, L"replay buffer session");
        if (!r.ok && !r.message.empty() && !g.quitting && !g.rec.running()) setStatus(L"Replay buffer stopped: " + r.message, toRef(T_WARN));
        if (r.windowClosed && !g.rec.running()) {
            g.target = CaptureTarget{}; g.target.kind = CaptureTarget::AllDisplays; setSrcWindow(L"", L"", L""); g.gprev.setTarget(g.target);
            startReplayBuffer();
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_APP_AUDIOERR:
        setStatus(wp ? L"Microphone capture stopped after an internal error (see log)" : L"System audio capture stopped after an internal error (see log)", toRef(T_ERR));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP_REPLAY_SAVED: {
        std::unique_ptr<ReplaySaved> r((ReplaySaved*)lp);
        g.replaySaving = false;
        if (r->ok) {
            playTone(IDR_SND_REPLAY);
            cr::appLog(L"replay saved: " + r->path + L" (" + std::to_wstring(r->secs) + L" s)");
            notice(TK_REPLAY, lastSavedText(r->secs), fileNameOf(r->path), r->path);
            setStatus(L"Saved " + fileNameOf(r->path), toRef(T_OK));
            scanRecordings();
        } else {
            cr::appLog(L"replay save FAILED: " + r->err);
            setStatus(L"Couldn't save the replay: " + r->err, toRef(T_ERR));
            notice(TK_ERROR, L"Replay not saved", r->err, L"");
        }
        if (g_hkTestHook) g_hkTestHook(r->ok ? 105 : 205);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_APP_REMUX_DONE: {
        std::unique_ptr<RemuxDone> r((RemuxDone*)lp);
        if (r->post) {
            for (auto& t : g_toasts) if (t->path == r->in && r->out != r->in) { t->path = r->out; t->sub = fileNameOf(r->out) + t->sub.substr(std::min(t->sub.size(), fileNameOf(r->in).size())); InvalidateRect(t->hwnd, nullptr, FALSE); }
            if (r->ok) setStatus(L"Saved " + fileNameOf(r->out), toRef(T_OK));
            else setStatus(L"Kept " + fileNameOf(r->out) + L"  \u00b7  " + r->err, toRef(T_WARN));
            if (g_hkTestHook) g_hkTestHook(r->ok ? 107 : 207);
        } else {
            cr::appLog(L"remux " + std::wstring(r->ok ? L"ok: " : L"FAILED: ") + r->in + L" -> " + r->out + (r->ok ? L"" : L" (" + r->err + L")"));
            if (r->ok) setStatus(L"Remuxed to " + fileNameOf(r->out), toRef(T_OK));
            else setStatus(L"Remux failed: " + r->err, toRef(T_ERR));
        }
        scanRecordings();
        return 0;
    }
    case WM_APP_META: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_APP_DIRCHANGE: SetTimer(h, TIMER_RESCAN, 400, nullptr); return 0;
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
            g.target = CaptureTarget{}; g.target.kind = CaptureTarget::AllDisplays; setSrcWindow(L"", L"", L"");
            g.gprev.setTarget(g.target);
            notice(TK_SAVED, L"Recording saved", L"Window closed \u00b7 " + fname, r.file);
        } else if (r.ok) {
            std::wstring msg2 = L"Saved " + fname + L"  \u00b7  " + r.encoderUsed;
            if (!r.message.empty()) msg2 += L"  \u00b7  " + r.message;
            if (r.cpuFallback) msg2 += L"  \u00b7  GPU conversion failed, finished on CPU";
            if (r.dropped) msg2 += L"  \u00b7  " + std::to_wstring(r.dropped) + L" dropped";
            if (!r.encoderFallbacks.empty()) msg2 += L"  \u00b7  " + r.encoderFallbacks.front() + L" failed its test encode, fell back";
            setStatus(msg2, toRef(T_OK));
            if (r.post.needed()) queuePost(r.post);  // MKV / track names: background stream copy, the MP4 is already safe
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            unsigned long long sz = GetFileAttributesExW(r.file.c_str(), GetFileExInfoStandard, &fa) ? (((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) : 0;
            std::wstring sub = fname + L" \u00b7 " + sizeText(sz);
            notice(TK_SAVED, L"Recording saved", sub, r.file);
        } else if (r.abandoned) {
            setStatus(r.file.empty() ? L"Encoder got stuck. Restart Cast Recorder to record again." : L"Encoder got stuck - kept " + fname + L". Restart Cast Recorder to record again.", toRef(T_WARN));
            notice(TK_ERROR, L"Encoder got stuck", r.file.empty() ? L"Restart Cast Recorder" : L"Kept " + fname, r.file);
        } else if (!r.file.empty()) {
            setStatus(L"Saved " + fname + L" (" + r.message + L")", toRef(T_WARN));
            notice(TK_SAVED, L"Recording saved", fname, r.file);
        } else if (r.timedOut) {
            setStatus(L"Stopped (timed out): " + r.message, toRef(T_WARN));
            notice(TK_ERROR, L"Recording stopped", L"Timed out", L"");
        } else {
            setStatus(r.message.empty() ? L"Recording stopped" : (L"Recording failed: " + r.message), toRef(T_ERR));
            if (!r.message.empty()) notice(TK_ERROR, L"Recording failed", r.message, L"");
            else notice(TK_STOPPED, L"Recording stopped", L"", L"");
        }
        g_recBytes = 0;
        if (g_hkTestHook) g_hkTestHook(r.ok ? 101 : 201);
        if (!g.quitting) { startReplayBuffer(); if (g_viewIdx == 1) scanRecordings(); }
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
        // Give a running MKV conversion / remux up to 5 s (no message pumping here). If it is still running, the
        // saved source stays as it is; ffmpeg is in a kill-on-close job, so only its partial copy is left behind.
        for (int i = 0; i < 500 && remuxBusy(); ++i) Sleep(10);
        if (remuxBusy()) cr::appLog(L"exit: a remux / MKV conversion was still running - the saved file is kept as it is");
        save();
        closePopup(false);
        for (auto& t : g_toasts) DestroyWindow(t->hwnd);
        g_toasts.clear();
        stopWatcher();
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
    paintToNull();
    pick(H_DD_FPS, 1);   // 30 fps
    pick(H_DD_QUAL, 3);  // Ultra
    // Source: open the picker, type into its search box, take the first matching window (CI starts Notepad)
    click(H_SRC);
    popupType(L"notepad");
    int row = -1;
    for (size_t i = 0; i < P.rows.size(); ++i) if (P.rows[i].kind == SR_WIN) { row = (int)i; break; }
    if (row >= 0) popupActivate(row); else { popupType(L""); popupActivate(0); }
    cr::appLog(L"selftest: source " + (g.target.kind == CaptureTarget::Window ? redactTitle(g_srcTitle) : sourceText()));
    click(H_TAB_SET);
    paintToNull();
    pick(H_DD_CONT, 0);  // MKV (non-default since v1.0)
    pick(H_DD_RES, 1);   // 1440p
    if (g_encChoices.size() > 1) pick(H_DD_ENC, (int)g_encChoices.size() - 1);
    cr::appLog(L"selftest: encoder " + encoderText());
    if (g.s.autoRemux) click(H_TG_REMUX);      // off
    if (!g.s.replayEnabled) click(H_TG_REPLAY);  // on
    pick(H_DD_REPLAYLEN, 2);                     // 1 min
    paintToNull();
    pick(H_DD_REPLAYLEN, (int)replayPresets().size());  // Custom... -> seconds field
    if (g_edit.hwnd) { SetWindowTextW(g_edit.hwnd, L"45"); commitEdit(true); }
    click(H_THEME_LIGHT);
    paintToNull();
    click(H_ACC0 + 3);                           // blue
    if (!g.s.separateTracks) click(H_TG_TRACKS);
    pick(H_DD_FNAME, 1);                         // date_time
    if (g.s.systemAudio) click(H_TG_SYS);  // off
    if (!g.s.micEnabled) click(H_TG_MIC);  // on (default microphone)
    if (!g.s.noiseSuppression) click(H_TG_NS);
    if (!g.s.pushToTalk) click(H_TG_PTT);
    if (!g.s.monitoring) click(H_TG_MON);
    click(H_CH_MONO);  // non-default since v1.0 (stereo)
    setSlider(H_SL_VOL, g_sliderX + g_sliderW * 0.75f);    // 150 %
    setSlider(H_SL_GATE, g_sliderX + g_sliderW * 0.375f);  // -50 dB
    save();
    g.s.micMute = Hotkey{MOD_CTRL_ | MOD_ALT_, 0x7B};     // Ctrl+Alt+F12
    g.s.saveReplay = Hotkey{MOD_CTRL_ | MOD_ALT_, 0x76};  // Ctrl+Alt+F7
    save(); registerAll();
    click(H_TAB_RECS);
    paintToNull();
    cr::appLog(L"selftest: recordings listed " + std::to_wstring(g_items.size()));
    click(H_MAX);
    click(H_CLOSE);
}
// CI: every global hotkey through the real path (SendInput -> RegisterHotKey -> WM_HOTKEY -> action).
std::vector<int> g_hkEvents;
int g_hkStep = 0, g_hkOk = 0, g_hkFail = 0; ULONGLONG g_hkT0 = 0; bool g_hkSent = false, g_hkMic0 = false;
void sendCombo(const Hotkey& k) {
    std::vector<INPUT> in;
    auto key = [&](WORD vk, bool up) { INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = vk; i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0; in.push_back(i); };
    std::vector<WORD> mods;
    if (k.mods & MOD_CTRL_) mods.push_back(VK_CONTROL);
    if (k.mods & MOD_ALT_) mods.push_back(VK_MENU);
    if (k.mods & MOD_SHIFT_) mods.push_back(VK_SHIFT);
    if (k.mods & MOD_WIN_) mods.push_back(VK_LWIN);
    for (WORD m : mods) key(m, false);
    key((WORD)k.vk, false); key((WORD)k.vk, true);
    for (auto it = mods.rbegin(); it != mods.rend(); ++it) key(*it, true);
    UINT n = SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
    cr::appLog(L"selftest hotkey: sent " + W(format(k)) + L" (" + std::to_wstring(n) + L"/" + std::to_wstring(in.size()) + L" events)");
}
bool hkEvent(int e) { return std::find(g_hkEvents.begin(), g_hkEvents.end(), e) != g_hkEvents.end(); }
void selftestHotkeysStep() {
    struct Step { const wchar_t* name; int slot; };
    static const Step steps[] = {{L"Save replay", 4}, {L"Start", 0}, {L"Pause", 1}, {L"Resume", 1},
                                 {L"Mic mute", 2}, {L"Mic unmute", 2}, {L"Stop", 0}};
    const int n = (int)(sizeof steps / sizeof steps[0]);
    ULONGLONG now = GetTickCount64();
    if (!g_hkT0) g_hkT0 = now;
    if (g_hkStep >= n) {
        KillTimer(g.hwnd, TIMER_HKTEST);
        cr::appLog(L"selftest-hotkeys: " + std::to_wstring(g_hkOk) + L"/" + std::to_wstring(n) + L" ok");
        click(H_CLOSE);
        return;
    }
    const Step& st = steps[g_hkStep];
    if (!g_hkSent) {
        bool ready = true;
        if (g_hkStep == 0) ready = g.ring && g.ring->bufferedSeconds() >= 2.0;
        if (g_hkStep == 2) ready = now - g_hkT0 > 1500;  // let the recording run a moment
        if (!ready && now - g_hkT0 < 20000) return;
        g_hkEvents.clear(); g_hkMic0 = g.s.micMuted;
        sendCombo(*hkSlot(st.slot));
        g_hkSent = true; g_hkT0 = now;
        return;
    }
    bool done = false;
    switch (g_hkStep) {
        case 0: done = hkEvent(105); break;
        case 1: done = g.rec.isActive(); break;
        case 2: done = g.rec.isPaused(); break;
        case 3: done = g.rec.isActive() && !g.rec.isPaused(); break;
        case 4: case 5: done = g.s.micMuted != g_hkMic0; break;
        case 6: done = hkEvent(101); break;
    }
    bool timeout = now - g_hkT0 > (g_hkStep == 6 || g_hkStep == 0 ? 20000u : 6000u);
    if (done || timeout) {
        cr::appLog(std::wstring(L"selftest hotkey ") + st.name + L" (" + W(format(*hkSlot(st.slot))) + L"): " + (done ? L"ok" : L"FAILED"));
        (done ? g_hkOk : g_hkFail)++;
        ++g_hkStep; g_hkSent = false; g_hkT0 = now;
    }
}

// Headless render for docs/CI images (PNG or BMP). States: idle | source | settings | recording | micpop
int encoderClsid(const wchar_t* mime, CLSID* out) {
    UINT n = 0, size = 0; gp::GetImageEncodersSize(&n, &size);
    if (!size) return -1;
    std::vector<BYTE> buf(size); auto* info = (gp::ImageCodecInfo*)buf.data();
    gp::GetImageEncoders(n, size, info);
    for (UINT i = 0; i < n; ++i) if (wcscmp(info[i].MimeType, mime) == 0) { *out = info[i].Clsid; return (int)i; }
    return -1;
}
void fakeRecordingsLocked();
void fakeRecordings() { std::lock_guard<std::mutex> l(g_itemsM); fakeRecordingsLocked(); }
void fakeRecordingsLocked() {
    struct F { const wchar_t* n; const wchar_t* e; double d; unsigned long long sz; int w, h, fps; int daysAgo, hh, mm; bool rep; };
    static const F fs[] = {{L"cast_2026-10-14_22-12", L"mp4", 134, 134637158ull, 2560, 1440, 60, 0, 22, 12, false},
                           {L"replay_22-10-05", L"mp4", 30, 25270681ull, 0, 0, 0, 0, 22, 10, true},
                           {L"cast_2026-10-14_19-40", L"mkv", 662, 642567372ull, 1920, 1080, 60, 0, 19, 40, false},
                           {L"cast_2026-10-13_23-05", L"mp4", 2718, 2040109465ull, 2560, 1440, 30, 1, 23, 5, false},
                           {L"cast_2026-10-13_18-22", L"mkv", 407, 406847488ull, 2560, 1440, 60, 1, 18, 22, false},
                           {L"cast_2026-10-12_14-02", L"mp4", 115, 64277299ull, 1920, 1080, 30, 2, 14, 2, false},
                           {L"replay_14-01-30", L"mp4", 30, 23802675ull, 0, 0, 0, 2, 14, 1, true}};
    g_items.clear();
    SYSTEMTIME now; GetLocalTime(&now);
    for (auto& f : fs) {
        RecItem it; it.name = f.n; it.ext = f.e; it.path = outputDir() + L"\\" + f.n + L"." + f.e; it.dur = f.d; it.size = f.sz; it.w = f.w; it.h = f.h; it.fps = f.fps;
        it.replay = f.rep; it.metaDone = true;
        SYSTEMTIME t = now; t.wHour = (WORD)f.hh; t.wMinute = (WORD)f.mm; t.wSecond = 0; t.wMilliseconds = 0;
        FILETIME lf, uf; SystemTimeToFileTime(&t, &lf);
        ULARGE_INTEGER u; u.LowPart = lf.dwLowDateTime; u.HighPart = lf.dwHighDateTime; u.QuadPart -= (ULONGLONG)f.daysAgo * 864000000000ull;
        lf.dwLowDateTime = u.LowPart; lf.dwHighDateTime = u.HighPart;
        LocalFileTimeToFileTime(&lf, &uf); it.mtime = uf;
        // stand-in thumbnail: a dark gradient like a desktop frame
        it.tw = 128; it.th = 72; it.thumbPx.resize(128 * 72);
        for (int y = 0; y < 72; ++y) for (int x = 0; x < 128; ++x) {
            int r = 0x1C + x / 10 + (f.rep ? 0 : y / 8), gg = 0x20 + y / 12, b = 0x2E + (127 - x) / 8;
            it.thumbPx[(size_t)y * 128 + x] = 0xFF000000u | (r << 16) | (gg << 8) | b;
        }
        g_items.push_back(std::move(it));
    }
    if (g_renderLong) {  // clipping audit: long and Cyrillic file names
        g_items[0].name = L"\u0417\u0430\u043f\u0438\u0441\u044c_\u0434\u043b\u044f_\u043e\u0447\u0435\u043d\u044c_\u0434\u043b\u0438\u043d\u043d\u043e\u0433\u043e_\u043f\u0440\u043e\u0435\u043a\u0442\u0430_2026-10-14_22-12-05";
        g_items[2].name = L"Quarterly planning review (final, really final v7) - team sync recording 2026-10-14";
    }
}
// Headless render: idle | recording | settings | recordings | replaytoast | savedtoast | source | dropdown | micpop | ctxmenu | tooltip | replaycap
// --ui-long: worst-case strings for the clipping audit (long device / GPU names, Cyrillic folder, long title and file names)
void applyLongValues() {
    g.mics.insert(g.mics.begin(), AudioDevice{L"{long-mic}", L"Microphone Array (Realtek(R) High Definition Audio with SST and Very Long Vendor Suffix)"});
    g.outs.insert(g.outs.begin(), AudioDevice{L"{long-out}", L"Speakers / Headphones (2- Logitech PRO X Wireless Gaming Headset with Surround Sound)"});
    g.s.micDevice = "{long-mic}"; g.s.systemDevice = "{long-out}";
    g.gpus.push_back(L"NVIDIA GeForce RTX 4090 Laptop GPU with Max-Q Design (Display Adapter 2)");
    g.s.gpuIndex = (int)g.gpus.size() - 1;
    g.s.outputDir = U(L"C:\\Users\\\u0410\u043b\u0435\u043a\u0441\u0430\u043d\u0434\u0440\u0430 \u041a\u043e\u043d\u0441\u0442\u0430\u043d\u0442\u0438\u043d\u043e\u0432\u0430\\Videos\\\u0417\u0430\u043f\u0438\u0441\u0438 \u044d\u043a\u0440\u0430\u043d\u0430 \u0434\u043b\u044f \u043f\u0440\u043e\u0435\u043a\u0442\u0430\\Cast Recorder");
    g.target.kind = CaptureTarget::Window; g.target.hwnd = nullptr; setSrcWindow(kLongTitle, L"PowerPoint", cr::stripAppSuffix(kLongTitle, L"POWERPNT.EXE", L"Microsoft PowerPoint"));
    g.s.toggleRecord = Hotkey{MOD_CTRL_ | MOD_ALT_ | MOD_SHIFT_, 0x91};  // Scroll Lock
    g.s.saveReplay = Hotkey{MOD_CTRL_ | MOD_ALT_ | MOD_SHIFT_ | MOD_WIN_, 0x7B};
    g.s.micMute = Hotkey{MOD_CTRL_ | MOD_ALT_ | MOD_SHIFT_, 0x2D};  // Insert
    g.s.pttKey = Hotkey{MOD_CTRL_ | MOD_SHIFT_, 0x14};  // Caps Lock
    g.s.pushToTalk = true;
    g.status = L"Saved \u0417\u0430\u043f\u0438\u0441\u044c_\u0434\u043b\u044f_\u043e\u0447\u0435\u043d\u044c_\u0434\u043b\u0438\u043d\u043d\u043e\u0433\u043e_\u043f\u0440\u043e\u0435\u043a\u0442\u0430_2026-10-14_22-12-05.mp4 (1.2 GB)";
    g.statusColor = toRef(T_OK);
}
int renderUi(const std::wstring& path, const std::wstring& state, int dpi, const std::wstring& theme, float W = 1100, float H = 700) {
    g_renderOnly = true;
    g.dpi = dpi;
    if (!theme.empty()) g.s.theme = U(theme);
    else if (g.s.theme == "auto") g.s.theme = "dark";  // CI renders are deterministic
    applyTheme();
    makeFonts(); makeUiFonts(); loadUiIcons();
    enumGpus();
    probeEncoders();
    g.mics = listMicrophones(); g.outs = listOutputDevices();
    resolveSource();
    g.status = L"Ready"; g.statusColor = toRef(T_TEXT2);
    if (g_renderLong) applyLongValues();
    for (int i = 0; i < kHkSlots; ++i) g.hkMsgColor[i] = toRef(T_OK);
    g_meter[0] = 0.58f; g_meter[1] = 0.66f;
    g.s.micEnabled = true;  // renders show both chips live
    g.s.replayEnabled = true;
    if (state == L"recording") { g_fakeRec = true; }
    if (state == L"settings") { g_viewIdx = 2; g.capturing = 4; }  // Save replay field shows "Press a key..."
    if (state == L"replaycap") { g_viewIdx = 2; g.s.replayEnabled = true; g.s.replaySeconds = 900; g.s.quality = Quality::Ultra; g.s.fps = 60; g.s.scale = OutputScale::Native; }  // memory-cap warning
    if (state == L"recordings" || state == L"ctxmenu") { g_viewIdx = 1; fakeRecordings(); }
    updatePreview();
    const float s = scale();
    g_renderW = W; g_renderH = H;
    gp::Bitmap bmp((INT)std::lround(W * s), (INT)std::lround(H * s), PixelFormat32bppARGB);
    {
        gp::Graphics gr(&bmp);
        setupGraphics(gr, false);
        gr.ScaleTransform(s, s);
        if (state == L"recordings") { std::lock_guard<std::mutex> l(g_itemsM); g_fakeRemuxPath = g_items[4].path; }
        paintAll(gr, W, H);
        RecLayout L = recLayout(W, H);
        if (state == L"source" || state == L"dropdown" || state == L"micpop" || state == L"ctxmenu") {
            P.kind = state == L"micpop" ? PK_MIC : state == L"ctxmenu" ? PK_LIST : PK_SOURCE;
            P.dd = state == L"micpop" ? H_MICCHIP : state == L"ctxmenu" ? H_CTXMENU : H_SRC;
            P.anchor = state == L"micpop" ? L.mic : state == L"ctxmenu" ? RF(300, 290, 0, 0) : L.src;
            if (state == L"ctxmenu") g_ctxPath = g_items[2].path;
            layoutPopup();
            P.open.set(1);
            P.kb = -1;
            paintAll(gr, W, H);  // field shows its open state
            // the same pixels the layered popup window gets (composePopup), placed on the device pixel grid
            const int mPx = (int)std::lround(kShadow * s);
            const int pw = (int)std::lround(P.w * s) + 2 * mPx, ph = (int)std::lround(P.h * s) + 2 * mPx;
            std::vector<uint32_t> buf((size_t)pw * ph);
            composePopup(buf.data(), pw, ph, mPx, s);
            gp::Bitmap pb(pw, ph, pw * 4, PixelFormat32bppPARGB, (BYTE*)buf.data());
            gr.ResetTransform();
            gr.SetInterpolationMode(gp::InterpolationModeNearestNeighbor);
            gr.DrawImage(&pb, gp::Rect((INT)std::lround(P.x * s) - mPx, (INT)std::lround(P.y * s) - mPx, pw, ph), 0, 0, pw, ph, gp::UnitPixel);
            gr.ScaleTransform(s, s);
        }
        if (state == L"tooltip") {  // hover over the truncated source field: full-text tooltip
            g_mouseX = L.src.X + L.src.Width / 2; g_mouseY = L.src.Y + L.src.Height / 2;
            paintAll(gr, W, H);
        }
        if (state == L"replaytoast" || state == L"savedtoast") {
            Toast t; t.inApp = true;
            if (state == L"replaytoast") { t.kind = TK_REPLAY; t.w = 284; t.h = 64; t.title = lastSavedText(30); t.sub = L"replay_22-10-05.mp4"; }
            else { t.kind = TK_SAVED; t.w = 320; t.h = 116; t.title = L"Recording saved"; t.sub = L"cast_2026-10-14_22-12.mp4 \u00b7 128.4 MB"; }
            if (g_renderLong) { std::lock_guard<std::mutex> l(g_itemsM); fakeRecordingsLocked(); t.sub = g_items[0].name + L".mp4 \u00b7 1.2 GB"; }
            const float tx = L.surf.X + L.surf.Width - 16 - t.w, ty = L.surf.Y + 16 + 30;
            const int mPx = (int)std::lround(kToastM * s);
            const int pw = (int)std::lround(t.w * s) + 2 * mPx, ph = (int)std::lround(t.h * s) + 2 * mPx;
            std::vector<uint32_t> buf((size_t)pw * ph);
            composeToast(buf.data(), pw, ph, mPx, s, t);
            gp::Bitmap tb(pw, ph, pw * 4, PixelFormat32bppPARGB, (BYTE*)buf.data());
            gr.ResetTransform();
            gr.SetInterpolationMode(gp::InterpolationModeNearestNeighbor);
            gr.DrawImage(&tb, gp::Rect((INT)std::lround(tx * s) - mPx, (INT)std::lround(ty * s) - mPx, pw, ph), 0, 0, pw, ph, gp::UnitPixel);
            gr.ScaleTransform(s, s);
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
        "  --moving-pattern     record a moving test pattern (every frame differs; CI replay ring)\n"
        "  --render-pattern FILE.bmp  write the test pattern reference image\n"
        "  --system-audio / --mic     record system audio / the default microphone\n"
        "  --test-tone          add a synthetic 440 Hz audio source (CI)\n"
        "  --pause-at S / --resume-at S  pause and resume at S seconds into the recording\n"
        "  --window TEXT        capture one window (title or exe substring) instead of the screen\n"
        "  --source all|monitor[:N]|window:TEXT   what to capture (default: the saved source)\n"
        "  --test-mic           synthetic noisy microphone through the real mic chain (CI)\n"
        "  --noise-suppression on|off / --noise-gate DB (-80 = off) / --mic-volume PCT / --channels 1|2\n"
        "  --list-windows       list capturable windows\n"
        "  --test-window-picker DIR  CI: picker filtering (main vs popups) and tiny/minimized window capture sizes\n"
        "  --codec auto|h264|hevc|av1   video codec (Auto: H.264 for MP4; AV1 > HEVC > H.264 for MKV)\n"
        "  --auto-remux on|off  MKV: remux to MP4 after saving and keep the MP4\n"
        "  --separate-tracks    three audio tracks: 1 mix, 2 system, 3 mic\n"
        "  --replay SECONDS     keep an instant-replay buffer (5..900 s) while recording\n"
        "  --replay-only        buffer only, write no recording (with --save-replay-at)\n"
        "  --save-replay-at S   save the replay buffer S seconds in; --replay-out FILE.mp4\n"
        "  --test-encoder-stall S  the encoder stops producing output S seconds in (CI: live encoder switch)\n"
        "  --remux IN OUT       stream-copy IN to OUT (no re-encode); --remux-engine mf|ffmpeg\n"
        "  --list-encoders      codec x vendor encoders available on this machine\n"
        "  --test-toast FILE.png / --test-toast-visible FILE.png  show a magenta toast (excluded from capture, or not),\n"
        "                       screenshot the screen to FILE.png and count magenta pixels; with --out also record 2 s\n"
        "  --render-ui FILE.png  paint the window to a PNG/BMP (docs/CI)\n"
        "  --ui-state idle|source|dropdown|settings|recording|recordings|replaytoast|savedtoast|micpop|ctxmenu|tooltip|replaycap\n"
        "                       [--ui-theme dark|light] [--dpi N] [--ui-size WxH] [--ui-long: worst-case long strings]\n"
        "  --help               show this help\n\n"
        "Exit codes: 0 success, 1 recording failed, 2 invalid arguments.\n";
    auto bad = [&](const std::string& m) { emit("error: " + m + "\n\n" + usage, stderr); return 2; };

    double secs = -1;
    std::wstring renderPath, uiState = L"idle", uiTheme; int renderDpi = 96; float uiW = 1100, uiH = 700;
    bool containerGiven = false, replayOnly = false, testToast = false, toastVisible = false;
    int replaySecs = 0, replayMaxMb = 0; double saveReplayAt = -1;
    std::wstring replayOut, remuxIn, remuxOut, remuxEngine, toastShot, pickerDir;
    double pauseAt = -1, resumeAt = -1;
    DWORD latePublishMs = 0;
    RecordOptions o;
    Settings s;
    loadFile(U(settingsPath()), s);
    o.fps = s.fps; o.quality = s.quality; o.encoder = s.encoder; o.container = s.container; o.captureCursor = s.captureCursor;
    o.scale = s.scale; o.gpuIndex = s.gpuIndex; o.bitrateCap = s.bitrateCap;
    o.codec = s.codec; o.autoRemux = false;  // CLI: keep exactly the requested container unless --auto-remux on
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
        if (a == L"--moving-pattern") { o.forcePattern = o.movingPattern = true; continue; }
        if (a == L"--system-audio") { o.systemAudio = true; continue; }
        if (a == L"--mic") { o.mic = true; continue; }
        if (a == L"--test-tone") { o.testTone = true; continue; }
        if (a == L"--test-mic") { o.testMic = true; continue; }
        if (a == L"--warmup") { cliWarmup = true; continue; }
        if (a == L"--separate-tracks") { o.separateTracks = true; continue; }
        if (a == L"--replay-only") { replayOnly = true; continue; }
        if (a == L"--list-encoders") {
            std::string o2;
            for (auto& c : probeCodecSupport(nullptr)) o2 += U(c.codec) + "\t" + U(c.vendor) + "\t" + (c.hardware ? "hardware" : "software") + "\n";
            emit(o2, stdout); return 0;
        }
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
        if (a == L"--ui-long") { g_renderLong = true; continue; }
        if (a != L"--record" && a != L"--fps" && a != L"--encoder" && a != L"--container" && a != L"--quality"
            && a != L"--out" && a != L"--log" && a != L"--scale" && a != L"--gpu" && a != L"--bitrate-cap"
            && a != L"--test-hang-finalize" && a != L"--test-slow-post" && a != L"--test-late-publish" && a != L"--replay-max-mb" && a != L"--pause-at" && a != L"--resume-at" && a != L"--render-ui" && a != L"--ui-state" && a != L"--dpi" && a != L"--ui-size" && a != L"--render-pattern" && a != L"--window"
            && a != L"--source" && a != L"--noise-suppression" && a != L"--noise-gate" && a != L"--mic-volume" && a != L"--channels"
            && a != L"--codec" && a != L"--auto-remux" && a != L"--replay" && a != L"--save-replay-at" && a != L"--replay-out" && a != L"--remux"
            && a != L"--remux-engine" && a != L"--test-toast" && a != L"--test-toast-visible" && a != L"--ui-theme" && a != L"--test-window-picker" && a != L"--test-encoder-stall")
            return bad("unknown option " + U(a));
        if (!value(v)) return bad(U(a) + " needs a value");
        if (a == L"--codec") {
            if (!fromString(v, o.codec)) return bad("--codec must be auto, h264, hevc or av1");
        } else if (a == L"--auto-remux") {
            if (v != "on" && v != "off") return bad("--auto-remux must be on or off");
            o.autoRemux = v == "on";
        } else if (a == L"--replay-max-mb") {
            int mb = 0;
            if (!parseIntStrict(v, 1, 65536, mb)) return bad("--replay-max-mb must be 1..65536");
            replayMaxMb = mb;
        } else if (a == L"--replay") {
            char* end = nullptr; long d = strtol(v.c_str(), &end, 10);
            if (v.empty() || !end || *end || d < 5 || d > 900) return bad("--replay must be 5..900 seconds");
            replaySecs = (int)d;
        } else if (a == L"--save-replay-at") {
            char* end = nullptr; double d = strtod(v.c_str(), &end);
            if (v.empty() || !end || *end || !(d > 0) || d > 86400) return bad("--save-replay-at must be seconds > 0");
            saveReplayAt = d;
        } else if (a == L"--test-encoder-stall") {
            char* end = nullptr; double d = strtod(v.c_str(), &end);
            if (v.empty() || !end || *end || !(d > 0) || d > 86400) return bad("--test-encoder-stall must be seconds > 0");
            o.testStallAfterSec = d;
        } else if (a == L"--replay-out") { replayOut = W(v);
        } else if (a == L"--remux") {
            if (i + 1 >= argc) return bad("--remux needs IN and OUT");
            remuxIn = W(v); remuxOut = argv[++i];
        } else if (a == L"--remux-engine") {
            if (v != "mf" && v != "ffmpeg") return bad("--remux-engine must be mf or ffmpeg");
            remuxEngine = W(v);
        } else if (a == L"--test-toast" || a == L"--test-toast-visible") { testToast = true; toastVisible = a == L"--test-toast-visible"; toastShot = W(v);
        } else if (a == L"--test-window-picker") { pickerDir = W(v);
        } else if (a == L"--ui-theme") {
            if (v != "dark" && v != "light") return bad("--ui-theme must be dark or light");
            uiTheme = W(v);
        } else if (a == L"--window") {
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
            containerGiven = true;
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
        } else if (a == L"--test-late-publish") {
            char* end = nullptr; unsigned long ms = strtoul(v.c_str(), &end, 10);
            if (v.empty() || !end || *end || ms > 60000) return bad("--test-late-publish must be 0..60000 ms");
            setTestLatePublishMs((DWORD)ms); latePublishMs = (DWORD)ms;
        } else if (a == L"--test-slow-post") {
            // handled in wWinMain (UI and CLI)
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
            if (v != "idle" && v != "dropdown" && v != "source" && v != "settings" && v != "recording" && v != "micpop" && v != "recordings" &&
                v != "replaytoast" && v != "savedtoast" && v != "ctxmenu" && v != "replaycap" && v != "tooltip")
                return bad("--ui-state must be idle, source, dropdown, settings, recording, recordings, replaytoast, savedtoast, micpop, ctxmenu, tooltip or replaycap");
            uiState = W(v);
        } else if (a == L"--ui-size") {
            int w = 0, h = 0; char tail = 0;
            if (sscanf(v.c_str(), "%dx%d%c", &w, &h, &tail) != 2 || w < 960 || h < 620 || w > 3840 || h > 2160) return bad("--ui-size must be WxH, 960x620 .. 3840x2160 (logical px)");
            uiW = (float)w; uiH = (float)h;
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
        int rc = renderUi(renderPath, uiState, renderDpi, uiTheme, uiW, uiH);
        emit(rc == 0 ? "ok=1 render-ui\n" : "ok=0 render-ui\n", stdout);
        return rc == 0 ? 0 : 1;
    }
    if (!remuxIn.empty()) {
        std::wstring err;
        ULONGLONG t0 = GetTickCount64();
        bool ok = remuxFile(remuxIn, remuxOut, err, remuxEngine, 120000);
        char b[160]; snprintf(b, sizeof b, "ok=%d remux_ms=%llu engine=%s\n", ok ? 1 : 0, GetTickCount64() - t0, remuxEngine.empty() ? "auto" : U(remuxEngine).c_str());
        emit(std::string(b) + "file=" + U(remuxOut) + "\nmessage=" + U(err) + "\n", stdout);
        return ok ? 0 : 1;
    }
    if (!pickerDir.empty()) {  // CI: real app window vs. popups in the window picker; tiny / minimized window capture sizes
        g.inst = GetModuleHandleW(nullptr);
        SHCreateDirectoryExW(nullptr, pickerDir.c_str(), nullptr);
        auto pump = [](DWORD ms) { ULONGLONG end = GetTickCount64() + ms; while (GetTickCount64() < end) { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m); Sleep(5); } };
        WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = g.inst; wc.lpszClassName = L"CRPickerTest";
        wc.hbrBackground = CreateSolidBrush(RGB(40, 160, 90)); wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        HWND mainW = CreateWindowExW(0, wc.lpszClassName, L"CR Picker Main - Brave", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, 801, 601, nullptr, nullptr, g.inst, nullptr);
        // what Brave/Chrome put on screen next to the real window: a bare topmost popup (media / hover strip) ...
        HWND popup = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName, L"CR Picker Popup", WS_POPUP | WS_VISIBLE, 300, 300, 146, 21, nullptr, nullptr, g.inst, nullptr);
        // ... a tool-window hover card, and an owned dialog
        HWND hover = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName, L"CR Picker Hover", WS_POPUP | WS_VISIBLE, 320, 340, 300, 120, nullptr, nullptr, g.inst, nullptr);
        HWND owned = CreateWindowExW(0, wc.lpszClassName, L"CR Picker Owned", WS_POPUP | WS_CAPTION | WS_VISIBLE, 400, 200, 400, 300, mainW, nullptr, g.inst, nullptr);
        HWND minW = CreateWindowExW(0, wc.lpszClassName, L"CR Picker Minimized", WS_OVERLAPPEDWINDOW, 120, 120, 641, 481, nullptr, nullptr, g.inst, nullptr);
        ShowWindow(minW, SW_SHOWMINNOACTIVE);
        // a real but tiny captioned window (Windows clamps it to its minimum track size, ~146x39)
        HWND tinyW = CreateWindowExW(0, wc.lpszClassName, L"CR Picker Tiny", WS_CAPTION | WS_SYSMENU | WS_VISIBLE, 500, 500, 146, 21, nullptr, nullptr, g.inst, nullptr);
        // a second window of the same app on the same page: both must stay selectable ("... (2)")
        HWND dupW = CreateWindowExW(0, wc.lpszClassName, L"CR Picker Main - Brave", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 160, 160, 701, 501, nullptr, nullptr, g.inst, nullptr);
        // odd-sized window, later grown mid-recording (scaled to fit, never cropped)
        HWND oddW = CreateWindowExW(0, wc.lpszClassName, L"CR Picker Odd", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60, 603, 403, nullptr, nullptr, g.inst, nullptr);
        pump(800);
        includeOwnWindowsForTest(true);
        auto ws = listCaptureWindows();
        std::string out;
        bool ok = true;
        struct Case { const char* name; HWND h; bool expect; } cases[] = {{"main", mainW, true}, {"popup", popup, false}, {"hover", hover, false}, {"owned", owned, false}, {"minimized", minW, true}, {"duplicate", dupW, true}};
        for (auto& c : cases) {
            const CaptureWindow* f = nullptr;
            for (auto& w : ws) if (w.hwnd == c.h) f = &w;
            const char* why = captureRejectReason(c.h);
            char b[200]; snprintf(b, sizeof b, "picker %s listed=%d expect=%d reason=%s\n", c.name, f ? 1 : 0, c.expect ? 1 : 0, why ? why : "-");
            out += b;
            if ((f != nullptr) != c.expect) ok = false;
            if (f && c.h == mainW) out += "label=" + U(f->label()) + "\napp=" + U(f->app) + "\npage=" + U(f->page) + "\n";
            if (f && c.h == dupW) out += "label_duplicate=" + U(f->label()) + "\n";
        }
        // a saved source whose title now matches only the popup must come back as the app's real window
        HWND r = resolveSavedWindow(windowExe(mainW), L"CR Picker Popup");
        out += std::string("resolve_saved=") + (r == mainW ? "main" : r == popup ? "popup" : r == minW ? "minimized" : r ? "other" : "none") + "\n";
        if (r != mainW) ok = false;
        includeOwnWindowsForTest(false);
        // capture: the 146x21 popup and a window minimized at start must give an even, >= 128 px encode
        for (auto& c : {std::make_pair("tiny", tinyW), std::make_pair("minimized", minW), std::make_pair("odd", oddW), std::make_pair("grow", oddW)}) {
            Recorder rec; RecordOptions ro = o;
            ro.window = c.second; ro.monitor = nullptr; ro.allDisplays = false;
            ro.encoder = Encoder::Software; ro.container = Container::MP4; ro.systemAudio = false; ro.mic = false; ro.testTone = false;
            ro.outputPath = pickerDir + L"\\picker-" + W(c.first) + L".mp4";
            rec.start(ro);
            ULONGLONG until = GetTickCount64() + 5000;
            while (rec.running() && !rec.started() && GetTickCount64() < until) pump(5);
            if (std::string(c.first) == "minimized") { pump(1200); ShowWindow(minW, SW_RESTORE); pump(1200); }  // restored mid-recording
            else if (std::string(c.first) == "grow") { pump(1000); SetWindowPos(oddW, nullptr, 0, 0, 1001, 761, SWP_NOMOVE | SWP_NOZORDER); pump(1500); }
            else pump(2000);
            rec.stop(); rec.waitUntilIdle(15000);
            const RecordResult& rr = rec.result();
            std::string el = U(rr.encoderLog); std::string size = "?";
            size_t k = el.find("window capture output ");
            if (k != std::string::npos) size = el.substr(k + 22, el.find('\n', k) - (k + 22));
            out += std::string("capture_") + c.first + " ok=" + (rr.ok ? "1" : "0") + " output=" + size + " file=" + U(rr.file) + " message=" + U(rr.message) + "\n";
            for (size_t q = 0; q < el.size();) {  // capture events (resized / restored / scaling) for the CI log
                size_t e = el.find('\n', q); if (e == std::string::npos) e = el.size();
                std::string line = el.substr(q, e - q);
                if (line.find("resized") != std::string::npos || line.find("restored") != std::string::npos || line.find("scaling") != std::string::npos || line.find("minimized") != std::string::npos)
                    out += std::string("  ") + c.first + " event: " + line + "\n";
                q = e + 1;
            }
            if (!rr.ok) ok = false;
        }
        out += std::string("picker_ok=") + (ok ? "1" : "0") + "\n";
        DestroyWindow(oddW); DestroyWindow(dupW); DestroyWindow(tinyW); DestroyWindow(owned); DestroyWindow(hover); DestroyWindow(popup); DestroyWindow(minW); DestroyWindow(mainW);
        emit(out, stdout);
        return ok ? 0 : 1;
    }
    if (testToast) {  // CI: is a toast window kept out of what we capture?
        g.inst = GetModuleHandleW(nullptr);
        gp::GdiplusStartupInput gsi; ULONG_PTR gpTok = 0;
        gp::GdiplusStartup(&gpTok, &gsi, nullptr);
        Toast* t = showToast(TK_TEST, L"", L"", L"", false, toastVisible);
        auto pump = [](DWORD ms) { ULONGLONG end = GetTickCount64() + ms; while (GetTickCount64() < end) { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m); Sleep(10); } };
        pump(700);
        std::string out = std::string("toast_window=") + (t ? "1" : "0") + " excluded=" + (t && t->excluded ? "1" : "0") + "\n";
        if (t) { RECT wr{}; GetWindowRect(t->hwnd, &wr); char b[96]; snprintf(b, sizeof b, "toast_rect=%ld,%ld,%ld,%ld\n", wr.left, wr.top, wr.right, wr.bottom); out += b; }
        std::wstring err; int w = 0, h = 0;
        bool ok = captureForTest(nullptr, nullptr, true, toastShot, false, err, &w, &h);
        long long magenta = -1;
        if (ok) {
            gp::Bitmap* b = gp::Bitmap::FromFile(toastShot.c_str());
            if (b && b->GetLastStatus() == gp::Ok) {
                magenta = 0;
                gp::BitmapData bd; gp::Rect lr(0, 0, (INT)b->GetWidth(), (INT)b->GetHeight());
                if (b->LockBits(&lr, gp::ImageLockModeRead, PixelFormat32bppARGB, &bd) == gp::Ok) {
                    for (UINT y = 0; y < bd.Height; ++y) {
                        const uint32_t* row = (const uint32_t*)((BYTE*)bd.Scan0 + (size_t)y * bd.Stride);
                        for (UINT x = 0; x < bd.Width; ++x) { uint32_t p = row[x]; if (((p >> 16) & 255) > 230 && ((p >> 8) & 255) < 30 && (p & 255) > 230) ++magenta; }
                    }
                    b->UnlockBits(&bd);
                }
            }
            delete b;
        }
        char b2[160]; snprintf(b2, sizeof b2, "screenshot_ok=%d %dx%d magenta_px=%lld\n", ok ? 1 : 0, w, h, magenta);
        out += b2;
        if (!o.outputPath.empty()) {  // also record 2 s through the real capture path with the toast up
            Recorder r; o.allDisplays = true; o.window = nullptr; o.monitor = nullptr;
            o.container = Container::MP4;
            r.start(o);
            while (r.running() && !r.started()) pump(5);
            pump(2000);
            r.stop(); r.waitUntilIdle(15000);
            out += std::string("record_ok=") + (r.result().ok ? "1" : "0") + " capture=" + U(r.result().captureUsed) + "\nfile=" + U(r.result().file) + "\n";
        }
        emit(out, stdout);
        return 0;
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
        if (!containerGiven) {  // the saved default is MKV now: --out x.mp4 means MP4
            std::wstring ext = lowerW(o.outputPath.substr(o.outputPath.find_last_of(L'.') == std::wstring::npos ? o.outputPath.size() : o.outputPath.find_last_of(L'.')));
            if (ext == L".mp4") o.container = Container::MP4; else if (ext == L".mkv") o.container = Container::MKV; else if (ext == L".mov") o.container = Container::MOV;
        }
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
    std::shared_ptr<ReplayRing> ring;
    if (replaySecs > 0 || replayOnly || saveReplayAt > 0) {
        ring = std::make_shared<ReplayRing>(replaySecs > 0 ? replaySecs : 30); o.ring = ring; o.bufferOnly = replayOnly;
        ring->setMaxBytes(replayMaxMb > 0 ? (size_t)replayMaxMb << 20 : ReplayRing::defaultMaxBytes());
    }
    if (replayOut.empty()) replayOut = o.outputPath.substr(0, o.outputPath.find_last_of(L'.')) + L"-replay.mp4";
    std::string replayInfo;
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
        if (ring && saveReplayAt > 0 && replayInfo.empty() && el >= (ULONGLONG)(saveReplayAt * 1000)) {
            double got = 0; std::wstring err;
            ULONGLONG ts = GetTickCount64();
            bool ok = ring->save(replayOut, got, err);
            char b[256]; snprintf(b, sizeof b, "replay_ok=%d replay_seconds=%.2f replay_buffered=%.2f replay_ram=%zu replay_cap=%zu replay_capped=%d replay_save_ms=%llu\n", ok ? 1 : 0, got, ring->bufferedSeconds(), ring->bytes(), ring->maxBytes(), ring->cappedByMemory() ? 1 : 0, GetTickCount64() - ts);
            replayInfo = std::string(b) + "replay_file=" + U(replayOut) + "\nreplay_message=" + U(err) + "\n";
        }
        if (resumeAt >= 0 && pausedOnce && !resumedOnce && el >= (ULONGLONG)(resumeAt * 1000)) {
            if (r.resume() && pauseStart) pausedWall = GetTickCount64() - pauseStart;
            resumedOnce = true;
        }
        Sleep(5);
    }
    if (pausedOnce && !resumedOnce && pauseStart) pausedWall = GetTickCount64() - pauseStart;
    r.stop();
    // Hang-finalize tests use a short finalize budget; give them headroom then abort.
    DWORD waitMs = latePublishMs ? 300 : 15000;
    if (!r.waitUntilIdle(waitMs)) r.abort();
    r.waitUntilIdle(2000);
    if (latePublishMs) {  // abort() published first: wait for the worker to lose the claim (it must keep the file)
        for (DWORD w = 0; w < latePublishMs + 8000 && testLatePublishLost() == 0; w += 50) Sleep(50);
        Sleep(200);
        char b[64]; snprintf(b, sizeof b, "late_publish_lost=%d\n", testLatePublishLost()); warmInfo += b;
    }
    for (int i = 0; i < 100 && abandonedFinalizeLive() > 0; ++i) Sleep(50);
    RecordResult res = r.result();
    if (res.ok && res.post.needed()) {  // MKV / track names: after the file is saved, outside the recorder
        std::wstring fin, plog;
        ULONGLONG t0 = GetTickCount64();
        bool pok = postProcessRecording(res.post, fin, plog);
        res.file = fin;
        res.encoderLog += plog;
        if (!pok && !res.post.mkvPath.empty()) res.message.clear();  // still saved (as MP4)
        char b[96]; snprintf(b, sizeof b, "post_ok=%d post_ms=%llu\n", pok ? 1 : 0, GetTickCount64() - t0);
        warmInfo += b;
    }
    std::string extra = warmInfo + replayInfo + "state=" + std::string(toString(r.state())) + "\n";
    extra += "codec=" + U(res.codec) + " audio_tracks=" + std::to_string(res.audioTracks) + " fragmented=" + (res.fragmented ? "1" : "0") + " encoder_switches=" + std::to_string(res.encoderSwitches) + "\n";
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
    if (replayOnly) return replayInfo.find("replay_ok=1") != std::string::npos ? 0 : 1;
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
    for (int i = 1; i + 1 < argc; ++i) if (wcscmp(argv[i], L"--test-slow-post") == 0) setTestSlowPostMs((DWORD)std::min(600000ul, wcstoul(argv[i + 1], nullptr, 10)));
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--selftest-record") == 0) g_selftest = g_selftestRecord = true;
    for (int i = 1; i < argc; ++i) if (wcscmp(argv[i], L"--selftest-hotkeys") == 0) g_selftest = g_selftestHotkeys = true;
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
    sweepPartFiles();
    audioThreadErrorHandler() = [](bool mic) { if (g.hwnd) PostMessageW(g.hwnd, WM_APP_AUDIOERR, mic ? 1 : 0, 0); };
    prewarmCaptureAsync();
    applyTheme();
    if (g_selftestHotkeys) { g.s.replayEnabled = true; g.s.replaySeconds = 10; g_hkTestHook = [](int e) { g_hkEvents.push_back(e); }; }
    gp::GdiplusStartupInput gsi; ULONG_PTR gpTok = 0;
    gp::GdiplusStartup(&gpTok, &gsi, nullptr);
    loadIcons();
    loadUiIcons();
    enumGpus();
    probeEncoders();
    g.mics = listMicrophones();  // so a saved microphone shows (and is used) right away
    g.outs = listOutputDevices();
    g.micMuted = g.s.micMuted;
    applyAudioFx();
    syncPtt();
    startPttThread();

    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
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
    applyTheme();  // DWM dark/light frame
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
    if (g_selftest && !g_selftestRecord && !g_selftestHotkeys) SetTimer(g.hwnd, TIMER_SELFTEST, 4000, nullptr);
    if (g_selftestHotkeys) SetTimer(g.hwnd, TIMER_HKTEST, 250, nullptr);
    resolveSource();  // before the preview starts, so it opens on the saved source
    g.gprev.start(g.hwnd, previewRectNow(), g.s.captureCursor);
    if (!IsWindowVisible(g.hwnd)) g.gprev.setVisible(false);
    restartMeter();
    startWatcher();
    ensurePopupWindow();  // dropdown + toast windows exist before they are needed (no creation on the save path)
    prewarmToasts();
    if (!g.warm.running()) startReplayBuffer();  // otherwise when the warm-up finishes

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    g.replay.stop();
    if (!g.replay.waitUntilIdle(3000)) g.replay.abort();
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
