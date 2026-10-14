#pragma once
// GPU live preview: own D3D11 device + own WGC session -> D3D11 video processor (scale/letterbox) -> flip-model
// swapchain in a child window. Runs on its own thread, so it never touches the recorder's device or encoder.
#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include "windows_enum.h"

namespace cr {

class GpuPreview {
public:
    ~GpuPreview() { stop(); }
    // Creates the child window (UI thread) and starts the render thread. False -> caller keeps its GDI fallback.
    bool start(HWND parent, const RECT& rc, bool cursor);
    void stop();
    void setRect(const RECT& rc);            // UI thread
    void setRecording(bool rec) { recording_ = rec; }  // 30 fps + lower priority while recording
    void setVisible(bool v);
    bool live() const { return live_.load(); }
    // Follow the selected source (any thread). The render thread swaps captures on its next iteration.
    void setTarget(const CaptureTarget& t) { { std::lock_guard<std::mutex> g(tm_); target_ = t; } ++targetGen_; }
    unsigned sourceWidth() const { return srcW_; }
    unsigned sourceHeight() const { return srcH_; }
    double fps() const { return fps_; }  // frames presented per second (last second)
    HWND hwnd() const { return child_; }
private:
    void run(bool cursor);
    HWND child_ = nullptr;
    std::thread th_;
    std::atomic<bool> quit_{false}, live_{false}, recording_{false}, visible_{true};
    std::atomic<bool> failed_{false};
    std::mutex tm_;
    CaptureTarget target_;
    std::atomic<unsigned> targetGen_{1}, srcW_{0}, srcH_{0};
    std::atomic<double> fps_{0};
};

}  // namespace cr
