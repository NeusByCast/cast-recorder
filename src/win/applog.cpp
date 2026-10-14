#include "applog.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
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
LONG WINAPI crashFilter(EXCEPTION_POINTERS* e) {
    wchar_t b[200];
    swprintf(b, 200, L"CRASH: exception 0x%08lX at %p", e && e->ExceptionRecord ? (unsigned long)e->ExceptionRecord->ExceptionCode : 0ul,
             e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionAddress : nullptr);
    appLog(b);
    return EXCEPTION_CONTINUE_SEARCH;
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
    trimIfBig(p);
    FILE* f = _wfopen(p.c_str(), L"ab");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char ts[64];
    snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, st.wMilliseconds, (unsigned long)GetCurrentProcessId());
    std::string body = utf8(line);
    // indent continuation lines so each entry stays readable
    std::string out = ts;
    for (char c : body) { if (c == '\n') out += "\r\n    "; else if (c != '\r') out += c; }
    while (out.size() >= 6 && out.compare(out.size() - 6, 6, "\r\n    ") == 0) out.resize(out.size() - 6);
    out += "\r\n";
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
}

void installCrashLogger() { SetUnhandledExceptionFilter(crashFilter); }
}  // namespace cr
