#pragma once
// GPU live preview: own D3D11 device + own WGC session -> D3D11 video processor (scale/letterbox) -> flip-model
// swapchain in a child window. Runs on its own thread, so it never touches the recorder's device or encoder.
#include <windows.h>
#include <atomic>
#include <memory>
#include <thread>

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
    HWND hwnd() const { return child_; }
private:
    void run(bool cursor);
    HWND child_ = nullptr;
    std::thread th_;
    std::atomic<bool> quit_{false}, live_{false}, recording_{false}, visible_{true};
    std::atomic<bool> failed_{false};
};

}  // namespace cr
