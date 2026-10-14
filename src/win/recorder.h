#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>
#include "../core/settings.h"

namespace cr {

struct RecordOptions {
    int fps = 60;
    Quality quality = Quality::High;
    Encoder encoder = Encoder::Auto;
    Container container = Container::MP4;
    std::wstring outputPath;     // final path incl. extension
    bool captureCursor = true;
    bool allowSynthetic = false; // smoke tests: use a generated test pattern if screen capture is unavailable
    HWND notify = nullptr;       // receives WM_APP_RECORDER_DONE when the recording thread exits
};

struct RecordResult {
    bool ok = false;
    std::wstring message;        // error or summary
    std::wstring file;           // final file
    std::wstring encoderUsed;    // "NVIDIA hardware (NVENC)", "Software (Media Foundation H.264)" ...
    std::wstring captureUsed;    // "Windows Graphics Capture", "DXGI Desktop Duplication", "Synthetic"
    unsigned long long frames = 0, duplicated = 0, late = 0;
};

constexpr UINT WM_APP_RECORDER_DONE = WM_APP + 40;

class Recorder {
public:
    ~Recorder();
    bool start(const RecordOptions& o);  // returns immediately; false if already running
    void stop();                         // asynchronous request
    void join();
    bool running() const { return running_; }
    double elapsedSeconds() const;
    const RecordResult& result() const { return result_; }  // valid after join / DONE message

private:
    void run();
    RecordOptions opt_;
    RecordResult result_;
    std::thread th_;
    std::atomic<bool> running_{false}, stop_{false};
    std::atomic<long long> startQpc_{0};
};

// Lists hardware H.264 encoders present (for UI hints), e.g. L"NVENC, QuickSync".
std::wstring describeHardwareEncoders();
// Finds ffmpeg.exe next to the app or on PATH (used to remux MP4 into MKV/MOV). Empty if missing.
std::wstring findFfmpeg();

}  // namespace cr
