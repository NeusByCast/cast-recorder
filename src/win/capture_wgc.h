// Windows Graphics Capture (Windows 10 1903+). Built only with MSVC + C++/WinRT (CR_HAVE_WGC);
// MinGW builds fall back to DXGI Desktop Duplication.
#pragma once
#ifdef CR_HAVE_WGC
#include <windows.h>
#include <d3d11.h>
#include <memory>
#include <string>

namespace cr {
class WgcCapture {
public:
    // Cold-start mitigation: loads the WGC runtime + activation factories on a background thread (no capture).
    static void prewarmAsync();
public:
    WgcCapture();
    ~WgcCapture();
    bool start(ID3D11Device* dev, HMONITOR mon, bool cursor, std::wstring& err);
    // Single window (CreateForWindow). Output size is fixed at start from the window's real (restored) frame size,
    // made even and at least kMinWindowOut on each side: a window minimized at start is sized from its restored
    // placement, never from the tiny minimized caption. If the window is later resized, its content is scaled to fit
    // the fixed output (letterboxed, GPU video processor; 1:1 top-left copy where no video processor exists).
    // Minimized: no new frames (the last frame is held).
    static constexpr UINT kMinWindowOut = 128;
    bool startWindow(ID3D11Device* dev, HWND hwnd, bool cursor, std::wstring& err);
    // Deliver at most one frame per `ms` (WGC MinUpdateInterval). 0 = every composed frame. On high-refresh monitors
    // (144/240 Hz) this keeps the compositor and our copies at the recording rate instead of the display rate.
    void setFrameInterval(double ms);
    bool closed() const;       // captured window/item went away
    std::wstring takeEvents(); // "minimized", "restored", "resized WxH", "closed"
    // dx/dy: destination offset (all-displays compositing). The copy is clipped to dst.
    bool copyLatest(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, UINT timeoutMs, bool& gotNew, int dx = 0, int dy = 0);
    UINT width() const;
    UINT height() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}  // namespace cr
#endif
