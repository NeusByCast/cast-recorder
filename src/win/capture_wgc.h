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
    WgcCapture();
    ~WgcCapture();
    bool start(ID3D11Device* dev, HMONITOR mon, bool cursor, std::wstring& err);
    // Single window (CreateForWindow). Output size is fixed at start; if the window is resized the content is
    // copied at 1:1 into the top-left and the rest cleared. Minimized: no new frames (last frame is held).
    bool startWindow(ID3D11Device* dev, HWND hwnd, bool cursor, std::wstring& err);
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
