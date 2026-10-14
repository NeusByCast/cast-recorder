#include "preview.h"
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>
#include "capture_wgc.h"

using Microsoft::WRL::ComPtr;

namespace cr {
namespace {
const wchar_t* kClass = L"CastRecorderPreview";
LRESULT CALLBACK childProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;  // clicks go to the main window
    return DefWindowProcW(h, m, w, l);
}
}  // namespace

bool GpuPreview::start(HWND parent, const RECT& rc, bool cursor) {
#ifndef CR_HAVE_WGC
    (void)parent; (void)rc; (void)cursor;
    return false;
#else
    if (child_) return true;
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = childProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        reg = RegisterClassW(&wc) != 0;
    }
    child_ = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, rc.left, rc.top, rc.right - rc.left,
                             rc.bottom - rc.top, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!child_) return false;
    quit_ = false; failed_ = false; live_ = false;
    th_ = std::thread(&GpuPreview::run, this, cursor);
    return true;
#endif
}

void GpuPreview::stop() {
    quit_ = true;
    if (th_.joinable()) th_.join();
    if (child_) { DestroyWindow(child_); child_ = nullptr; }
    live_ = false;
}

void GpuPreview::setRect(const RECT& rc) {
    if (child_) SetWindowPos(child_, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void GpuPreview::setVisible(bool v) {
    visible_ = v;
    if (child_) ShowWindow(child_, v && live_ ? SW_SHOWNA : SW_HIDE);
}

void GpuPreview::run(bool cursor) {
#ifdef CR_HAVE_WGC
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    [&] {
        ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
            return;
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
        ComPtr<IDXGIDevice1> dxd; ComPtr<IDXGIAdapter> ad; ComPtr<IDXGIFactory2> fac;
        if (FAILED(dev.As(&dxd)) || FAILED(dxd->GetAdapter(&ad)) || FAILED(ad->GetParent(IID_PPV_ARGS(&fac)))) return;
        dxd->SetMaximumFrameLatency(1);

        HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        WgcCapture cap;  // WGC delivers frames on our device even if another GPU scans out the monitor
        std::wstring err;
        if (!cap.start(dev.Get(), mon, cursor, err)) return;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = cap.width(); td.Height = cap.height(); td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> frame;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &frame))) return;

        RECT cr{}; GetClientRect(child_, &cr);
        UINT bw = cr.right > 16 ? (UINT)cr.right : 16, bh = cr.bottom > 16 ? (UINT)cr.bottom : 16;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = bw; sd.Height = bh; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.Scaling = DXGI_SCALING_STRETCH;
        ComPtr<IDXGISwapChain1> sc;
        if (FAILED(fac->CreateSwapChainForHwnd(dev.Get(), child_, &sd, nullptr, nullptr, &sc))) {
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            if (FAILED(fac->CreateSwapChainForHwnd(dev.Get(), child_, &sd, nullptr, nullptr, &sc))) return;
        }
        fac->MakeWindowAssociation(child_, DXGI_MWA_NO_ALT_ENTER);

        ComPtr<ID3D11VideoDevice> vdev; ComPtr<ID3D11VideoContext> vctx;
        if (FAILED(dev.As(&vdev)) || FAILED(ctx.As(&vctx))) return;
        ComPtr<ID3D11VideoProcessorEnumerator> en; ComPtr<ID3D11VideoProcessor> vp;
        ComPtr<ID3D11VideoProcessorInputView> iv;
        UINT vpW = 0, vpH = 0;
        auto buildVp = [&](UINT ow, UINT oh) -> bool {
            vp.Reset(); en.Reset(); iv.Reset();
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
            cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            cd.InputWidth = td.Width; cd.InputHeight = td.Height; cd.OutputWidth = ow; cd.OutputHeight = oh;
            cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            if (FAILED(vdev->CreateVideoProcessorEnumerator(&cd, &en)) || FAILED(vdev->CreateVideoProcessor(en.Get(), 0, &vp))) return false;
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{}; ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            if (FAILED(vdev->CreateVideoProcessorInputView(frame.Get(), en.Get(), &ivd, &iv))) return false;
            vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
            D3D11_VIDEO_COLOR bg{}; bg.RGBA.R = 0.07f; bg.RGBA.G = 0.07f; bg.RGBA.B = 0.08f; bg.RGBA.A = 1;
            vctx->VideoProcessorSetOutputBackgroundColor(vp.Get(), FALSE, &bg);
            // letterbox
            double sa = (double)td.Width / td.Height, ta = (double)ow / oh;
            RECT dst{0, 0, (LONG)ow, (LONG)oh};
            if (sa > ta) { LONG h2 = (LONG)(ow / sa); dst.top = ((LONG)oh - h2) / 2; dst.bottom = dst.top + h2; }
            else { LONG w2 = (LONG)(oh * sa); dst.left = ((LONG)ow - w2) / 2; dst.right = dst.left + w2; }
            vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dst);
            RECT full{0, 0, (LONG)ow, (LONG)oh};
            vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &full);
            vpW = ow; vpH = oh;
            return true;
        };
        if (!buildVp(bw, bh)) return;

        bool shown = false;
        LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
        LARGE_INTEGER last{};
        bool any = false;
        while (!quit_) {
            const bool rec = recording_.load();
            SetThreadPriority(GetCurrentThread(), rec ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
            if (!visible_.load()) { Sleep(50); continue; }
            bool got = false;
            if (!cap.copyLatest(ctx.Get(), frame.Get(), 16, got)) { failed_ = true; break; }
            RECT c2{}; GetClientRect(child_, &c2);
            UINT nw = c2.right > 16 ? (UINT)c2.right : 16, nh = c2.bottom > 16 ? (UINT)c2.bottom : 16;
            bool resized = nw != vpW || nh != vpH;
            if (resized) {
                ctx->ClearState(); ctx->Flush();
                if (FAILED(sc->ResizeBuffers(0, nw, nh, DXGI_FORMAT_UNKNOWN, 0)) || !buildVp(nw, nh)) { failed_ = true; break; }
            }
            any = any || got;
            if (!any || (!got && !resized)) continue;
            // pacing: 60 fps idle, 30 fps while recording (never competes with the encoder)
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            LONGLONG minTicks = freq.QuadPart / (rec ? 20 : 60);
            if (last.QuadPart && now.QuadPart - last.QuadPart < minTicks && !resized) {
                DWORD ms = (DWORD)((minTicks - (now.QuadPart - last.QuadPart)) * 1000 / freq.QuadPart);
                if (ms) Sleep(ms);
            }
            ComPtr<ID3D11Texture2D> bb;
            if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) { failed_ = true; break; }
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{}; ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            ComPtr<ID3D11VideoProcessorOutputView> ov;
            if (FAILED(vdev->CreateVideoProcessorOutputView(bb.Get(), en.Get(), &ovd, &ov))) { failed_ = true; break; }
            D3D11_VIDEO_PROCESSOR_STREAM st{}; st.Enable = TRUE; st.pInputSurface = iv.Get();
            if (FAILED(vctx->VideoProcessorBlt(vp.Get(), ov.Get(), 0, 1, &st))) continue;
            HRESULT ph = sc->Present(0, 0);
            if (FAILED(ph)) { failed_ = true; break; }
            QueryPerformanceCounter(&last);
            if (!shown) { shown = true; live_ = true; PostMessageW(GetParent(child_), WM_APP + 41, 1, 0); }
        }
    }();
    if (live_.exchange(false) || failed_) PostMessageW(GetParent(child_), WM_APP + 41, 0, 0);
    if (SUCCEEDED(co)) CoUninitialize();
#else
    (void)cursor;
#endif
}

}  // namespace cr
