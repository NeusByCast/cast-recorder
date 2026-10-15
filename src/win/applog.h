#pragma once
// Single diagnostics file: %APPDATA%\Cast Recorder\CastRecorder.log (trimmed to the newest ~1 MB at 2 MB).
#include <string>
namespace cr {
std::wstring appDataDir();            // %APPDATA%\Cast Recorder (created)
std::wstring appLogPath();
void setAppLogPath(const std::wstring& p);  // tests
void appLog(const std::wstring& line);       // timestamped, thread-safe; multi-line ok
void installCrashLogger();
void addLogSecret(const std::wstring& s);   // text (e.g. a window title in a file name) logged as "(N chars)"                   // unhandled exceptions -> log
}
