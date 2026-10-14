#ifdef CR_HAVE_WGC
#include "capture_wgc.h"
#include <dxgi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
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
    winrt::event_token token{}, closedTok{};
    HWND hwnd = nullptr;
    std::atomic<bool> isClosed{false};
    bool wasIconic = false;
    int lastCw = 0, lastCh = 0;
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    ID3D11Texture2D* rtvFor = nullptr;
    std::mutex evM;
    std::wstring events;
    void event(const std::wstring& e) { std::lock_guard<std::mutex> g(evM); if (events.size() < 2000) events += e + L"; "; }
    bool begin(ID3D11Device* dev, bool cursor, std::wstring& err);
    ~Impl() { if (arrived) CloseHandle(arrived); }
};

WgcCapture::WgcCapture() : p_(std::make_unique<Impl>()) {}
WgcCapture::~WgcCapture() {
    try {
        if (p_->pool && p_->token.value) p_->pool.FrameArrived(p_->token);
        if (p_->item && p_->closedTok.value) p_->item.Closed(p_->closedTok);
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
        return p_->begin(dev, cursor, err);
    } catch (const winrt::hresult_error& e) {
        err = L"WGC: " + std::wstring(e.message().c_str());
        return false;
    } catch (...) {
        err = L"WGC failed";
        return false;
    }
}

bool WgcCapture::startWindow(ID3D11Device* dev, HWND hwnd, bool cursor, std::wstring& err) {
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) { err = L"WGC not supported"; return false; }
        if (!IsWindow(hwnd)) { err = L"window no longer exists"; return false; }
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(p_->item)));
        p_->hwnd = hwnd;
        p_->wasIconic = IsIconic(hwnd) != 0;
        Impl* ip = p_.get();
        p_->closedTok = p_->item.Closed([ip](auto&&, auto&&) { ip->isClosed = true; ip->event(L"closed"); SetEvent(ip->arrived); });
        return p_->begin(dev, cursor, err);
    } catch (const winrt::hresult_error& e) {
        err = L"WGC window: " + std::wstring(e.message().c_str());
        return false;
    } catch (...) {
        err = L"WGC window capture failed";
        return false;
    }
}
bool WgcCapture::closed() const { return p_->isClosed.load() || (p_->hwnd && !IsWindow(p_->hwnd)); }
std::wstring WgcCapture::takeEvents() { std::lock_guard<std::mutex> g(p_->evM); std::wstring e; e.swap(p_->events); return e; }

bool WgcCapture::Impl::begin(ID3D11Device* devIn, bool cursor, std::wstring& err) {
    Impl* p_ = this;
    try {
        p_->dev.copy_from(devIn);
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
        p_->lastCw = (int)p_->w; p_->lastCh = (int)p_->h;
        return p_->w && p_->h;
    } catch (const winrt::hresult_error& e) {
        err = L"WGC: " + std::wstring(e.message().c_str());
        return false;
    } catch (...) {
        err = L"WGC failed";
        return false;
    }
}

bool WgcCapture::copyLatest(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, UINT timeoutMs, bool& gotNew, int dx, int dy) {
    gotNew = false;
    try {
        DWORD start = GetTickCount();
        wgc::Direct3D11CaptureFrame frame{nullptr}, f{nullptr};
        for (;;) {
            while ((f = p_->pool.TryGetNextFrame())) { if (frame) frame.Close(); frame = f; }  // keep newest
            DWORD el = GetTickCount() - start;
            if (frame || el >= timeoutMs || p_->isClosed) break;
            WaitForSingleObject(p_->arrived, timeoutMs - el);
        }
        if (p_->hwnd) {
            bool ic = IsIconic(p_->hwnd) != 0;
            if (ic != p_->wasIconic) { p_->wasIconic = ic; p_->event(ic ? L"minimized (holding last frame)" : L"restored"); }
        }
        if (!frame) return true;
        auto cs = frame.ContentSize();
        if (p_->hwnd && (cs.Width != p_->lastCw || cs.Height != p_->lastCh)) {
            // Window resized: rebuild the pool at the new size so frames aren't cropped by the old buffers;
            // the output stays at the start size (content top-left, rest cleared).
            p_->lastCw = cs.Width; p_->lastCh = cs.Height;
            p_->event(L"resized " + std::to_wstring(cs.Width) + L"x" + std::to_wstring(cs.Height));
            winrt::com_ptr<IDXGIDevice> dxgi = p_->dev.as<IDXGIDevice>();
            winrt::com_ptr<::IInspectable> insp;
            if (SUCCEEDED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put())))
                p_->pool.Recreate(insp.as<wdx::Direct3D11::IDirect3DDevice>(), wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, cs);
            if (!p_->rtv || p_->rtvFor != dst) { p_->rtv = nullptr; p_->dev->CreateRenderTargetView(dst, nullptr, p_->rtv.put()); p_->rtvFor = dst; }
            const float black[4] = {0, 0, 0, 1};
            if (p_->rtv) ctx->ClearRenderTargetView(p_->rtv.get(), black);
        }
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> tex;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(tex.put())));
        D3D11_TEXTURE2D_DESC dd{}; dst->GetDesc(&dd);
        int bw = std::min(std::min(cs.Width, (int)p_->w), (int)dd.Width - dx), bh = std::min(std::min(cs.Height, (int)p_->h), (int)dd.Height - dy);
        if (dx >= 0 && dy >= 0 && bw > 0 && bh > 0) {
            D3D11_BOX box{0, 0, 0, (UINT)bw, (UINT)bh, 1};
            ctx->CopySubresourceRegion(dst, 0, (UINT)dx, (UINT)dy, 0, tex.get(), 0, &box);
        }
        frame.Close();
        gotNew = true;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace cr
#endif
