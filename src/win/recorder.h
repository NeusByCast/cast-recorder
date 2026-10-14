#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>
#include "../core/settings.h"
#include "../core/fsm.h"

namespace cr {

struct RecordOptions {
    int fps = 60;
    Quality quality = Quality::High;
    Encoder encoder = Encoder::Auto;
    Container container = Container::MP4;
    std::wstring outputPath;
    bool captureCursor = true;
    bool allowSynthetic = false;
    HWND notify = nullptr;       // WM_APP_RECORDER_DONE when the recording thread exits
    DWORD finalizeTimeoutMs = 8000;  // hard cap on Finalize + remux
};

struct RecordResult {
    bool ok = false;
    bool timedOut = false;       // Finalize or remux hit the hard timeout
    std::wstring message;
    std::wstring file;
    std::wstring encoderUsed;
    std::wstring captureUsed;
    unsigned long long frames = 0, duplicated = 0, late = 0;
};

constexpr UINT WM_APP_RECORDER_DONE = WM_APP + 40;

class Recorder {
public:
    Recorder();
    ~Recorder();
    bool start(const RecordOptions& o);  // false if already busy
    void stop();                         // async, idempotent
    // Wait up to timeoutMs for Idle. Returns true if idle. Never call from the UI thread for long;
    // use a short timeout and abort() if still busy.
    bool waitUntilIdle(DWORD timeoutMs);
    // Abandon a stuck finalize: signals stop, waits briefly, detaches the worker if still hung.
    // After abort the object can start() again once waitUntilIdle succeeds or the worker exits.
    void abort();
    void join();  // wait forever — prefer waitUntilIdle; kept for CLI
    bool running() const { return fsm_.busy(); }          // Recording / Stopping / Finishing
    bool isRecording() const { return fsm_.isRecording(); }
    RecState state() const { return fsm_.get(); }
    bool started() const { return started_; }
    double elapsedSeconds() const;
    const RecordResult& result() const { return result_; }

private:
    void run();
    void signalDone();
    RecordOptions opt_;
    RecordResult result_;
    std::thread th_;
    RecFsm fsm_;
    std::atomic<bool> stop_{false}, started_{false};
    std::atomic<int> workers_{0};  // live run() count (incl. detached)
    std::atomic<long long> startQpc_{0};
    HANDLE doneEvent_ = nullptr;  // manual-reset, set when Idle
};

std::wstring describeHardwareEncoders();
std::wstring findFfmpeg();

}  // namespace cr
