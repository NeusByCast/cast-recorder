#ifdef CR_HAVE_WGC
#include "capture_wgc.h"
#include <dxgi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
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
    bool anyFrame = false;  // window mode: something (a real frame, or black while minimized at start) was delivered
    int lastCw = 0, lastCh = 0;
    double intervalMs = 0;
    // window mode: content size != output size -> copy to `scaleIn`, GPU-scale (letterbox) into the output
    winrt::com_ptr<ID3D11Texture2D> scaleIn;
    UINT scaleInW = 0, scaleInH = 0;
    winrt::com_ptr<ID3D11VideoDevice> vdev;
    winrt::com_ptr<ID3D11VideoContext> vctx;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> ven;
    winrt::com_ptr<ID3D11VideoProcessor> vp;
    winrt::com_ptr<ID3D11VideoProcessorInputView> viv;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> vov;
    ID3D11Texture2D* vovFor = nullptr;
    bool vpBroken = false;
    bool scaleTo(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, UINT cw, UINT ch, ID3D11Texture2D* dst);
    void applyInterval();
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

void WgcCapture::prewarmAsync() {
    std::thread([] {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            if (wgc::GraphicsCaptureSession::IsSupported()) {
                (void)winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
                (void)winrt::get_activation_factory<wgc::Direct3D11CaptureFramePool>();
            }
            winrt::uninit_apartment();
        } catch (...) {}
    }).detach();
}

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
        if (!p_->begin(dev, cursor, err)) return false;
        // Output size: the window's real size. Minimized -> its restored placement (the item is only the caption then).
        UINT ow = p_->w, oh = p_->h;
        if (p_->wasIconic) {
            WINDOWPLACEMENT wp{sizeof wp};
            if (GetWindowPlacement(hwnd, &wp)) {
                LONG nw = wp.rcNormalPosition.right - wp.rcNormalPosition.left, nh = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
                // The placement includes the invisible resize borders (left/right/bottom) that WGC doesn't capture.
                if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) {
                    UINT dpi = GetDpiForWindow(hwnd); if (!dpi) dpi = 96;
                    LONG b = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    if (nw > 3 * b && nh > 3 * b) { nw -= 2 * b; nh -= b; }
                }
                ow = (UINT)std::max<LONG>(0, nw); oh = (UINT)std::max<LONG>(0, nh);
            }
            p_->event(L"minimized at start: output sized from the restored window " + std::to_wstring(ow) + L"x" + std::to_wstring(oh));
        }
        ow = std::max(ow, kMinWindowOut); oh = std::max(oh, kMinWindowOut);
        ow = (ow + 1) & ~1u; oh = (oh + 1) & ~1u;  // encoders need even sizes
        ow = std::min(ow, 8192u); oh = std::min(oh, 8192u);
        p_->w = ow; p_->h = oh;
        return true;
    } catch (const winrt::hresult_error& e) {
        err = L"WGC window: " + std::wstring(e.message().c_str());
        return false;
    } catch (...) {
        err = L"WGC window capture failed";
        return false;
    }
}
void WgcCapture::setFrameInterval(double ms) { p_->intervalMs = ms; p_->applyInterval(); }
void WgcCapture::Impl::applyInterval() {
    if (!session) return;
    // Windows 11 24H2+: MinUpdateInterval (default throttles; 1 ms = every composed frame)
    if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
        long long hns = intervalMs > 0 ? (long long)(intervalMs * 10000.0) : 10000;
        try { session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{hns}); } catch (...) {}
    }
}
bool WgcCapture::Impl::scaleTo(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, UINT cw, UINT ch, ID3D11Texture2D* dst) {
    if (vpBroken) return false;
    try {
        if (!vdev) { vdev = dev.try_as<ID3D11VideoDevice>(); winrt::com_ptr<ID3D11DeviceContext> c; c.copy_from(ctx); vctx = c.try_as<ID3D11VideoContext>(); }
        if (!vdev || !vctx) { vpBroken = true; return false; }
        D3D11_TEXTURE2D_DESC od{}; dst->GetDesc(&od);
        if (!scaleIn || scaleInW != cw || scaleInH != ch) {
            scaleIn = nullptr; vp = nullptr; ven = nullptr; viv = nullptr; vov = nullptr; vovFor = nullptr;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = cw; td.Height = ch; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, scaleIn.put()))) { vpBroken = true; return false; }
            scaleInW = cw; scaleInH = ch;
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
            cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            cd.InputWidth = cw; cd.InputHeight = ch; cd.OutputWidth = w; cd.OutputHeight = h;
            cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            if (FAILED(vdev->CreateVideoProcessorEnumerator(&cd, ven.put())) || FAILED(vdev->CreateVideoProcessor(ven.get(), 0, vp.put()))) { vpBroken = true; return false; }
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{}; ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            if (FAILED(vdev->CreateVideoProcessorInputView(scaleIn.get(), ven.get(), &ivd, viv.put()))) { vpBroken = true; return false; }
            vctx->VideoProcessorSetStreamAutoProcessingMode(vp.get(), 0, FALSE);
            vctx->VideoProcessorSetStreamFrameFormat(vp.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
            D3D11_VIDEO_COLOR bg{}; bg.RGBA.A = 1;
            vctx->VideoProcessorSetOutputBackgroundColor(vp.get(), FALSE, &bg);
            double sa = (double)cw / ch, ta = (double)w / h;
            RECT d{0, 0, (LONG)w, (LONG)h};
            if (sa > ta) { LONG h2 = (LONG)(w / sa + 0.5) & ~1L; d.top = ((LONG)h - h2) / 2; d.bottom = d.top + h2; }
            else { LONG w2 = (LONG)(h * sa + 0.5) & ~1L; d.left = ((LONG)w - w2) / 2; d.right = d.left + w2; }
            RECT s0{0, 0, (LONG)cw, (LONG)ch};
            vctx->VideoProcessorSetStreamSourceRect(vp.get(), 0, TRUE, &s0);
            vctx->VideoProcessorSetStreamDestRect(vp.get(), 0, TRUE, &d);
            RECT full{0, 0, (LONG)w, (LONG)h};
            vctx->VideoProcessorSetOutputTargetRect(vp.get(), TRUE, &full);
        }
        if (vovFor != dst) {
            vov = nullptr;
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{}; ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            if (FAILED(vdev->CreateVideoProcessorOutputView(dst, ven.get(), &ovd, vov.put()))) { vpBroken = true; return false; }
            vovFor = dst;
        }
        D3D11_BOX box{0, 0, 0, cw, ch, 1};
        ctx->CopySubresourceRegion(scaleIn.get(), 0, 0, 0, 0, src, 0, &box);
        D3D11_VIDEO_PROCESSOR_STREAM st{}; st.Enable = TRUE; st.pInputSurface = viv.get();
        if (FAILED(vctx->VideoProcessorBlt(vp.get(), vov.get(), 0, 1, &st))) { vpBroken = true; return false; }
        (void)od;
        return true;
    } catch (...) { vpBroken = true; return false; }
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
        p_->applyInterval();
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
        if (p_->hwnd && p_->wasIconic) {
            if (frame) frame.Close();
            if (!p_->anyFrame) {  // minimized at start: begin on black at the restored size; content follows on restore
                if (!p_->rtv || p_->rtvFor != dst) { p_->rtv = nullptr; p_->dev->CreateRenderTargetView(dst, nullptr, p_->rtv.put()); p_->rtvFor = dst; }
                const float black[4] = {0, 0, 0, 1};
                if (p_->rtv) { ctx->ClearRenderTargetView(p_->rtv.get(), black); p_->anyFrame = true; gotNew = true; }
            }
            return true;  // minimized: only the caption is rendered; hold the last frame
        }
        if (!frame) return true;  // minimized: only the caption is rendered; hold the last frame
        auto cs = frame.ContentSize();
        bool contentChanged = false;
        if (p_->hwnd && (cs.Width != p_->lastCw || cs.Height != p_->lastCh)) {
            // Window resized: rebuild the pool at the new size so frames aren't cropped by the old buffers.
            p_->lastCw = cs.Width; p_->lastCh = cs.Height; contentChanged = true;
            p_->event(L"resized " + std::to_wstring(cs.Width) + L"x" + std::to_wstring(cs.Height));
            winrt::com_ptr<IDXGIDevice> dxgi = p_->dev.as<IDXGIDevice>();
            winrt::com_ptr<::IInspectable> insp;
            if (SUCCEEDED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put())))
                p_->pool.Recreate(insp.as<wdx::Direct3D11::IDirect3DDevice>(), wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, cs);
        }
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> tex;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(tex.put())));
        D3D11_TEXTURE2D_DESC sd{}; tex->GetDesc(&sd);
        D3D11_TEXTURE2D_DESC dd{}; dst->GetDesc(&dd);
        UINT cw = std::min((UINT)std::max(cs.Width, 1), sd.Width), ch = std::min((UINT)std::max(cs.Height, 1), sd.Height);
        bool scaled = false;
        if (p_->hwnd && dx == 0 && dy == 0 && (cw != p_->w || ch != p_->h) && cw >= 2 && ch >= 2)
            scaled = p_->scaleTo(ctx, tex.get(), cw, ch, dst);  // fit the window into the fixed output (letterbox)
        if (!scaled) {
            if (p_->hwnd && (contentChanged || (cw != p_->w || ch != p_->h))) {  // 1:1 top-left, rest black
                if (!p_->rtv || p_->rtvFor != dst) { p_->rtv = nullptr; p_->dev->CreateRenderTargetView(dst, nullptr, p_->rtv.put()); p_->rtvFor = dst; }
                const float black[4] = {0, 0, 0, 1};
                if (p_->rtv && contentChanged) ctx->ClearRenderTargetView(p_->rtv.get(), black);
            }
            int bw = std::min(std::min((int)cw, (int)p_->w), (int)dd.Width - dx), bh = std::min(std::min((int)ch, (int)p_->h), (int)dd.Height - dy);
            if (dx >= 0 && dy >= 0 && bw > 0 && bh > 0) {
                D3D11_BOX box{0, 0, 0, (UINT)bw, (UINT)bh, 1};
                ctx->CopySubresourceRegion(dst, 0, (UINT)dx, (UINT)dy, 0, tex.get(), 0, &box);
            }
        }
        frame.Close();
        gotNew = true;
        p_->anyFrame = true;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace cr
#endif
