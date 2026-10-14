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
    bool copyLatest(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, UINT timeoutMs, bool& gotNew);
    UINT width() const;
    UINT height() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}  // namespace cr
#endif
