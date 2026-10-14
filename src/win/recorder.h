#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>
#include "../core/settings.h"
#include "../core/fsm.h"
#include "../core/handoff.h"

namespace cr {

struct RecordOptions {
    int fps = 60;
    Quality quality = Quality::High;
    Encoder encoder = Encoder::Auto;
    Container container = Container::MP4;
    OutputScale scale = OutputScale::Native;
    int gpuIndex = -1;               // -1 = Auto (adapter that owns the monitor)
    unsigned bitrateCap = 0;         // 0 = no extra cap beyond quality preset
    std::wstring outputPath;
    bool captureCursor = true;
    bool allowSynthetic = false;
    HWND notify = nullptr;           // WM_APP_RECORDER_DONE when the recording thread exits
    DWORD finalizeTimeoutMs = 8000;  // hard cap on Finalize (and separately on remux)
};

struct RecordResult {
    bool ok = false;
    bool timedOut = false;           // Finalize or remux hit the hard timeout
    bool abandoned = false;          // stuck Finalize/Close; MF poisoned for process lifetime
    std::wstring message;
    std::wstring file;
    std::wstring encoderUsed;
    std::wstring captureUsed;
    std::wstring pathLog;            // adapter / encoder / zero-copy / drops for diagnostics
    unsigned long long frames = 0, duplicated = 0, late = 0, dropped = 0;
};

constexpr UINT WM_APP_RECORDER_DONE = WM_APP + 40;

class Recorder {
public:
    Recorder();
    ~Recorder();
    bool start(const RecordOptions& o);  // false if already busy or MF abandoned
    void stop();                         // async, idempotent
    bool pause();                        // Recording -> Paused (no samples written, clock frozen)
    bool resume();                       // Paused -> Recording
    bool togglePause() { return isPaused() ? resume() : pause(); }
    // Wait up to timeoutMs for Idle. Returns true if idle. Never call from the UI thread for long;
    // use a short timeout and abort() if still busy.
    bool waitUntilIdle(DWORD timeoutMs);
    // Abandon a stuck finalize: signals stop, waits briefly, detaches the worker if still hung.
    // After abandon, MF is poisoned: start() refuses a second session until process exit.
    void abort();
    void join();  // wait forever — prefer waitUntilIdle; kept for CLI
    bool running() const { return fsm_.busy(); }          // Recording / Stopping / Finishing
    bool isRecording() const { return fsm_.isRecording(); }
    bool isPaused() const { return fsm_.isPaused(); }
    bool isActive() const { return fsm_.isRecording() || fsm_.isPaused(); }
    RecState state() const { return fsm_.get(); }
    bool started() const { return started_; }
    bool mfAbandoned() const { return abandon_.poisoned.load(); }
    double elapsedSeconds() const;
    const RecordResult& result() const { return result_; }

private:
    void run();
    RecordOptions opt_;
    RecordResult result_;
    std::thread th_;
    RecFsm fsm_;
    FinishClaim claim_;
    AbandonLatch abandon_;
    std::atomic<bool> stop_{false}, started_{false};
    std::atomic<int> workers_{0};  // live run() count (incl. detached)
    std::atomic<long long> startQpc_{0};
    std::atomic<long long> pausedQpc_{0};    // QPC when the current pause began (0 = not paused)
    std::atomic<long long> pausedTotal_{0};  // accumulated paused QPC ticks
    unsigned sessionGen_ = 0;
    std::wstring sessionTmp_;      // temp path for abort cleanup
    std::wstring sessionOut_;      // final output path for abort cleanup
    HANDLE doneEvent_ = nullptr;   // manual-reset, set when Idle
};

std::wstring describeHardwareEncoders();
std::wstring findFfmpeg();
// Test hook: delay before IMFSinkWriter::Finalize (simulates a stuck Finalize). 0 = off.
void setTestHangFinalizeMs(DWORD ms);
// Abandoned Finalize helper threads still running (they delete their temp file when they finish).
int abandonedFinalizeLive();

}  // namespace cr
