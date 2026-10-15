#pragma once
#include <exception>
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>
#include <memory>
#include <mutex>
#include "../core/settings.h"
#include "../core/fsm.h"
#include "../core/handoff.h"

struct IMFSample;
struct IMFMediaType;

namespace cr {

// Instant replay: encoded video packets + PCM audio of the last N seconds, in RAM, trimmed to start on a key frame.
// One ring outlives recorder sessions (buffer-only session -> recording -> buffer-only) as long as the encoder
// configuration is unchanged, so a replay can be saved at any time, also while recording.
class ReplayRing {
public:
    explicit ReplayRing(int seconds);
    ~ReplayRing();
    void setSeconds(int s);
    int seconds() const;
    double bufferedSeconds() const;   // what a save right now would contain (from the first usable key frame)
    size_t bytes() const;             // RAM held
    // RAM cap: when hit, the oldest GOPs are dropped (the replay gets shorter, memory never grows past it).
    void setMaxBytes(size_t b);
    size_t maxBytes() const;
    bool cappedByMemory() const;      // the last trim was because of the cap
    static unsigned long long estimateBytes(int seconds, unsigned videoBitrate, int channels, int tracks, bool audio);
    static size_t defaultMaxBytes();  // min(25 % of physical RAM, 2 GB)
    // Muxes the last `seconds()` (from a key frame) into an MP4. Blocking: call off the UI thread.
    bool save(const std::wstring& path, double& savedSeconds, std::wstring& err);
    bool saveImpl(const std::wstring& path, double& savedSeconds, std::wstring& err);
    // recorder side
    void attach(IMFMediaType* videoType, int fps, int channels, int tracks, bool audio);
    void addVideo(IMFSample* s);
    void addAudio(long long t100ns, const std::vector<short>* const pcm[3]);
    void detach();
    std::wstring stats() const;  // arena / packet counters (perf log)
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

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
    bool forcePattern = false;       // CI: static test pattern instead of the screen (quality / fps checks)
    bool systemAudio = true;         // WASAPI loopback of the default output device
    bool mic = false;
    std::wstring micId;              // empty = default microphone
    bool testTone = false;           // CI: synthetic 440 Hz audio source
    HWND window = nullptr;           // capture this top-level window instead of the monitor (WGC)
    HMONITOR monitor = nullptr;      // capture this monitor (nullptr = primary)
    bool allDisplays = false;        // capture every monitor composited into one virtual-desktop frame (WGC)
    std::wstring systemId;           // system-audio output device (empty = default, excluding our own sounds)
    int channels = 2;                // 1 = mono, 2 = stereo AAC (default)
    bool testMic = false;            // CI: synthetic noisy microphone through the real mic chain
    bool pauseTones = false;         // UI plays pause/resume tones: keep them out of the audio
    bool warmupOnly = false;         // run setup (device, capture, probes, writer/AAC load) and exit before recording
    long long pressQpc = 0;          // QPC when the user asked to record (latency breakdown)
    HWND notify = nullptr;           // WM_APP_RECORDER_DONE when the recording thread exits
    DWORD finalizeTimeoutMs = 8000;  // Finalize budget is half of this; nothing after Finalize runs in the worker
    // v1.1
    Codec codec = Codec::Auto;       // Auto: AV1 > HEVC > H.264 for MKV, H.264 for MP4 (falls back to H.264)
    bool autoRemux = false;          // MKV: keep the (crash-safe) MP4 the recorder wrote instead of converting to MKV
    bool separateTracks = false;     // audio tracks: 1 mix, 2 system, 3 mic
    std::shared_ptr<ReplayRing> ring;  // feed encoded packets + audio into this replay buffer
    bool bufferOnly = false;         // replay buffer session: no file is written
    UINT doneMsg = 0;                // notify with this instead of WM_APP_RECORDER_DONE (replay buffer session)
};

// Work after a recording is saved (never inside the recorder / Finalize watchdog): MKV conversion and audio track
// names, both ffmpeg stream copies. The saved MP4 is only replaced once the new file is complete.
struct PostJob {
    std::wstring file;                     // the saved (fragmented) MP4
    std::wstring mkvPath;                  // non-empty: convert to this MKV, then delete the MP4
    std::vector<std::wstring> trackNames;  // non-empty: title the audio tracks (Mix / System audio / Microphone)
    bool needed() const { return !file.empty() && (!mkvPath.empty() || !trackNames.empty()); }
};
// Runs a PostJob (blocking; call off the UI thread). finalFile = the file to show afterwards (the MP4 if anything failed).
bool postProcessRecording(const PostJob& j, std::wstring& finalFile, std::wstring& log);
void setTestSlowPostMs(DWORD ms);  // test hook: post-processing takes this long first (simulates a ~5 GB copy)
void prewarmCaptureAsync();
bool isAppTempName(const std::wstring& fileName);         // "*.castrec-tmp*.*"
std::wstring makeTempFor(const std::wstring& out);        // fresh journaled temp next to `out`
void releaseTemp(const std::wstring& tmp);
std::wstring activeTempFor(const std::wstring& out);      // temp currently being written for `out` (progress)
int sweepTempJournal(const std::wstring& outputDir, std::wstring& log);                  // startup: recycle journaled leftovers of a killed run
void logThreadException(const wchar_t* name, const char* what);
// Runs f, logging and swallowing any exception (worker threads must never terminate the process).
template <class F> void guardedRun(const wchar_t* name, F&& f) {
    try { f(); }
    catch (const std::exception& e) { logThreadException(name, e.what()); }
    catch (...) { logThreadException(name, "unknown exception"); }
}  // cold-start mitigation: load the WGC runtime off the UI thread
void setTestLatePublishMs(DWORD ms);  // test hook: the worker publishes this late so abort() wins the claim
int testLatePublishLost();           // how many workers lost the publish claim

struct RecordResult {
    PostJob post;                    // to run after the result is published (UI: background queue; CLI: inline)
    bool fragmented = false;         // written as fragmented MP4 (playable even without Finalize)
    bool ok = false;
    bool timedOut = false;           // Finalize or remux hit the hard timeout
    bool cpuFallback = false;        // GPU color conversion failed; switched to CPU NV12
    bool abandoned = false;          // stuck Finalize/Close; MF poisoned for process lifetime
    std::wstring message;
    std::wstring file;
    std::wstring encoderUsed;
    std::wstring captureUsed;
    std::wstring encoderLog;         // per-step HRESULTs: enumeration, probe, MFT setup, drain
    std::vector<std::wstring> encoderFallbacks;  // hardware encoders that failed the probe / setup
    bool encoderStalled = false;
    std::wstring latencyLog;         // press -> first frame breakdown
    bool windowClosed = false;       // window capture ended because the window closed
    double startMs = 0;              // press -> first captured frame
    std::wstring pathLog;            // adapter / encoder / zero-copy / drops for diagnostics
    bool hasAudio = false;
    int audioTracks = 0;
    std::wstring codec;              // H.264 / HEVC / AV1
    double captureFps = 0, encodeFps = 0, convertMsAvg = 0, encodeMsAvg = 0;
    unsigned long long frames = 0, duplicated = 0, late = 0, dropped = 0, pauses = 0, convertFailures = 0;
};

constexpr UINT WM_APP_RECORDER_DONE = WM_APP + 40;
constexpr UINT WM_APP_RECORDER_STARTED = WM_APP + 42;  // first frame captured: play the start tone now
constexpr UINT WM_APP_WARMUP_DONE = WM_APP + 43;

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
    void setMicMuted(bool m) { micMuted_ = m; }
    bool micMuted() const { return micMuted_.load(); }
    bool mfAbandoned() const { return abandon_.poisoned.load(); }
    double elapsedSeconds() const;
    std::wstring currentFile() const { std::lock_guard<std::mutex> g(fileM_); return curFile_; }  // file being written
    const RecordResult& result() const { return result_; }

private:
    void run();
    RecordOptions opt_;
    RecordResult result_;
    std::thread th_;
    RecFsm fsm_;
    FinishClaim claim_;
    AbandonLatch& abandon_;        // process-wide: MF is per-process, so one poisoned session poisons all
    std::atomic<bool> stop_{false}, started_{false}, micMuted_{false};
    std::atomic<int> workers_{0};  // live run() count (incl. detached)
    std::atomic<long long> startQpc_{0};
    std::atomic<long long> pausedQpc_{0};    // QPC when the current pause began (0 = not paused)
    std::atomic<long long> pausedTotal_{0};  // accumulated paused QPC ticks
    unsigned sessionGen_ = 0;
    std::wstring sessionTmp_;      // temp path for abort cleanup
    std::wstring sessionOut_;      // final output path for abort cleanup
    HANDLE doneEvent_ = nullptr;   // manual-reset, set when Idle
    mutable std::mutex fileM_;
    std::wstring curFile_;
};

std::wstring describeHardwareEncoders();
// Static CI test pattern (also written by --render-pattern as the PSNR/SSIM reference).
void testPattern(std::vector<uint32_t>& px, unsigned w, unsigned h);
std::wstring findFfmpeg();
// MKV/MOV -> MP4 without re-encoding (stream copy): ffmpeg when available, else Media Foundation (source reader ->
// pass-through sink writer). engine: L"" auto, L"ffmpeg", L"mf". trackNames: title metadata for the audio tracks.
bool remuxFile(const std::wstring& in, const std::wstring& out, std::wstring& err, const std::wstring& engine = L"",
               DWORD timeoutMs = 0, const std::vector<std::wstring>& trackNames = {});
// Test-only (CLI --test-toast): GDI grab of the desktop to a PNG. Windows excluded from capture
// (WDA_EXCLUDEFROMCAPTURE, our toasts) never appear.
bool captureForTest(HWND window, HMONITOR monitor, bool allDisplays, const std::wstring& path, bool jpg, std::wstring& err, int* outW = nullptr, int* outH = nullptr);
// Test hook: delay before IMFSinkWriter::Finalize (simulates a stuck Finalize). 0 = off.
void setTestHangFinalizeMs(DWORD ms);
// Abandoned Finalize helper threads still running.
int abandonedFinalizeLive();

}  // namespace cr
