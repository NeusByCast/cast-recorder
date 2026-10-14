#include "applog.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <atomic>
#include <cwctype>
#include <vector>

namespace cr {
namespace {
std::mutex g_m;
std::wstring g_path;
std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
void trimIfBig(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return;
    if (a.nFileSizeHigh == 0 && a.nFileSizeLow < 2u * 1024 * 1024) return;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return;
    const long keep = 1024 * 1024;
    fseek(f, -keep, SEEK_END);
    std::vector<char> buf(keep);
    size_t n = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    size_t start = 0;
    while (start < n && buf[start] != '\n') ++start;  // cut at a line boundary
    f = _wfopen(p.c_str(), L"wb");
    if (!f) return;
    const char* hdr = "... (older entries trimmed)\r\n";
    fwrite(hdr, 1, strlen(hdr), f);
    if (start + 1 < n) fwrite(buf.data() + start + 1, 1, n - start - 1, f);
    fclose(f);
}
HANDLE g_crashFile = INVALID_HANDLE_VALUE;  // opened up front: the crash path takes no locks, allocates nothing
std::atomic<bool> g_crashed{false};
LONG WINAPI crashFilter(EXCEPTION_POINTERS* e) {
    if (g_crashed.exchange(true) || g_crashFile == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    char b[160];
    int n = wsprintfA(b, "CRASH [pid %lu tid %lu]: exception 0x%08lX at %p\r\n", GetCurrentProcessId(), GetCurrentThreadId(),
                      e && e->ExceptionRecord ? (unsigned long)e->ExceptionRecord->ExceptionCode : 0ul,
                      e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionAddress : nullptr);
    DWORD w = 0;
    if (n > 0) WriteFile(g_crashFile, b, (DWORD)n, &w, nullptr);  // FILE_APPEND_DATA handle: atomic append
    return EXCEPTION_CONTINUE_SEARCH;
}
// Cross-process lock for append + trim (the app, the CLI and a second launch can all write).
struct XLock {
    HANDLE h;
    XLock() : h(CreateMutexW(nullptr, FALSE, L"Local\\CastRecorder.Log")) { if (h) { DWORD r = WaitForSingleObject(h, 2000); if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) { CloseHandle(h); h = nullptr; } } }
    ~XLock() { if (h) { ReleaseMutex(h); CloseHandle(h); } }
};
// %USERPROFILE%\... -> %USERPROFILE%\... (case-insensitive), so shared logs don't carry the user name.
std::wstring redact(std::wstring s) {
    wchar_t up[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
    if (!n || n >= MAX_PATH) return s;
    std::wstring u(up, n), lo = s, ul = u;
    for (auto& c : lo) c = towlower(c);
    for (auto& c : ul) c = towlower(c);
    for (size_t p = lo.find(ul); p != std::wstring::npos; p = lo.find(ul, p + 13)) {
        s.replace(p, u.size(), L"%USERPROFILE%"); lo.replace(p, u.size(), L"%userprofile%");
    }
    // the bare user name (e.g. in a JSON-escaped path) too
    const wchar_t* bs = wcsrchr(up, L'\\');
    if (bs && bs[1]) {
        std::wstring name = bs + 1, nl = name;
        for (auto& c : nl) c = towlower(c);
        if (nl.size() >= 3) for (size_t p = lo.find(L"\\" + nl); p != std::wstring::npos; p = lo.find(L"\\" + nl, p + 1)) {
            size_t e = p + 1 + nl.size();
            if (e < lo.size() && lo[e] != L'\\' && lo[e] != L'/' && lo[e] != L'"') continue;
            s.replace(p + 1, name.size(), L"<user>"); lo.replace(p + 1, name.size(), L"<user>");
        }
    }
    return s;
}
}  // namespace

std::wstring appDataDir() {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    if (s.empty()) { wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t); s = t; }
    s += L"\\Cast Recorder";
    SHCreateDirectoryExW(nullptr, s.c_str(), nullptr);
    return s;
}
std::wstring appLogPath() {
    std::lock_guard<std::mutex> g(g_m);
    if (g_path.empty()) g_path = appDataDir() + L"\\CastRecorder.log";
    return g_path;
}
void setAppLogPath(const std::wstring& p) { std::lock_guard<std::mutex> g(g_m); g_path = p; }

void appLog(const std::wstring& line) {
    std::wstring p = appLogPath();
    std::lock_guard<std::mutex> g(g_m);
    XLock xl;
    trimIfBig(p);
    FILE* f = _wfopen(p.c_str(), L"ab");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char ts[64];
    snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, st.wMilliseconds, (unsigned long)GetCurrentProcessId());
    std::string body = utf8(redact(line));
    // indent continuation lines so each entry stays readable
    std::string out = ts;
    for (char c : body) { if (c == '\n') out += "\r\n    "; else if (c != '\r') out += c; }
    while (out.size() >= 6 && out.compare(out.size() - 6, 6, "\r\n    ") == 0) out.resize(out.size() - 6);
    out += "\r\n";
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
}

void installCrashLogger() {
    std::wstring p = appLogPath();
    g_crashFile = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    SetUnhandledExceptionFilter(crashFilter);
}
}  // namespace cr
