#ifdef CR_HAVE_WGC
#include "capture_wgc.h"
#include <dxgi.h>
#include <algorithm>
#include <chrono>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;

namespace cr {

struct WgcCapture::Impl {
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::com_ptr<ID3D11Device> dev;
    UINT w = 0, h = 0;
    HANDLE arrived = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset; set from FrameArrived
    winrt::event_token token{};
    ~Impl() { if (arrived) CloseHandle(arrived); }
};

WgcCapture::WgcCapture() : p_(std::make_unique<Impl>()) {}
WgcCapture::~WgcCapture() {
    try {
        if (p_->pool && p_->token.value) p_->pool.FrameArrived(p_->token);
        if (p_->session) p_->session.Close();
        if (p_->pool) p_->pool.Close();
    } catch (...) {}
}
UINT WgcCapture::width() const { return p_->w; }
UINT WgcCapture::height() const { return p_->h; }

bool WgcCapture::start(ID3D11Device* dev, HMONITOR mon, bool cursor, std::wstring& err) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {}  // already initialised by the caller
    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) { err = L"WGC not supported"; return false; }
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForMonitor(mon, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(p_->item)));
        p_->dev.copy_from(dev);
        winrt::com_ptr<IDXGIDevice> dxgi = p_->dev.as<IDXGIDevice>();
        winrt::com_ptr<::IInspectable> insp;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put()));
        auto d3d = insp.as<wdx::Direct3D11::IDirect3DDevice>();
        auto size = p_->item.Size();
        p_->w = (UINT)size.Width; p_->h = (UINT)size.Height;
        p_->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(d3d, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);
        // Never block in the handler: just signal the recording thread (no 1 ms polling / 15.6 ms Sleep rounding).
        HANDLE ev = p_->arrived;
        p_->token = p_->pool.FrameArrived([ev](auto&&, auto&&) { SetEvent(ev); });
        p_->session = p_->pool.CreateCaptureSession(p_->item);
        p_->session.IsCursorCaptureEnabled(cursor);
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
            try { p_->session.IsBorderRequired(false); } catch (...) {}
        }
        // Windows 11 24H2+: default MinUpdateInterval throttles delivery; ask for every composed frame.
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
            try { p_->session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{std::chrono::milliseconds(1)}); } catch (...) {}
        }
        p_->session.StartCapture();
        return p_->w && p_->h;
    } catch (const winrt::hresult_error& e) {
        err = L"WGC: " + std::wstring(e.message().c_str());
        return false;
    } catch (...) {
        err = L"WGC failed";
        return false;
    }
}

bool WgcCapture::copyLatest(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, UINT timeoutMs, bool& gotNew) {
    gotNew = false;
    try {
        DWORD start = GetTickCount();
        wgc::Direct3D11CaptureFrame frame{nullptr}, f{nullptr};
        for (;;) {
            while ((f = p_->pool.TryGetNextFrame())) { if (frame) frame.Close(); frame = f; }  // keep newest
            DWORD el = GetTickCount() - start;
            if (frame || el >= timeoutMs) break;
            WaitForSingleObject(p_->arrived, timeoutMs - el);
        }
        if (!frame) return true;
        auto cs = frame.ContentSize();
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> tex;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(tex.put())));
        D3D11_BOX box{0, 0, 0, (UINT)std::min(cs.Width, (int)p_->w), (UINT)std::min(cs.Height, (int)p_->h), 1};
        ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, tex.get(), 0, &box);
        frame.Close();
        gotNew = true;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace cr
#endif
