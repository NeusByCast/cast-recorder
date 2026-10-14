// Capture (WGC / DXGI Desktop Duplication / synthetic) -> D3D11 texture -> Media Foundation H.264 encoder -> MP4.
// Frames stay on the GPU on the hardware path: the captured texture is copied into a GDI-compatible
// BGRA texture (cursor is drawn with GDI), then wrapped with MFCreateDXGISurfaceBuffer. The sink writer's
// GPU video processor converts BGRA->NV12 and feeds the vendor's hardware MFT (NVENC/AMF/QuickSync).
#include "recorder.h"
#include <climits>
#include <new>
#include <algorithm>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d10.h>
#include <dxgi1_2.h>
#include <oleauto.h>
#include <strmif.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>
#include "capture_wgc.h"
#include "hwenc.h"
#include "audio.h"
#include "applog.h"
#include "windows_enum.h"
#include <future>
#include <map>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <timeapi.h>
#include "../core/nv12.h"
// Last: instantiate the CODECAPI_* GUIDs in this TU only.
#include <initguid.h>
#include <codecapi.h>

using Microsoft::WRL::ComPtr;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace cr {
namespace {

std::wstring hrMsg(const wchar_t* what, HRESULT hr) {
    wchar_t b[256];
    swprintf(b, 256, L"%ls failed (0x%08lX)", what, (unsigned long)hr);
    return b;
}

const wchar_t* vendorId(Encoder e) {
    switch (e) {
        case Encoder::NVENC: return L"VEN_10DE";
        case Encoder::AMF: return L"VEN_1002";
        case Encoder::QSV: return L"VEN_8086";
        default: return nullptr;
    }
}
UINT vendorPci(Encoder e) {
    switch (e) { case Encoder::NVENC: return 0x10DE; case Encoder::AMF: return 0x1002; case Encoder::QSV: return 0x8086; default: return 0; }
}
const wchar_t* vendorLabel(const std::wstring& v) {
    if (v.find(L"10DE") != std::wstring::npos) return L"NVENC";
    if (v.find(L"1002") != std::wstring::npos || v.find(L"1022") != std::wstring::npos) return L"AMF";
    if (v.find(L"8086") != std::wstring::npos) return L"QuickSync";
    return L"Hardware";
}

// Hardware H.264 encoder MFTs' vendor ids.
std::vector<std::wstring> hardwareVendors() {
    std::vector<std::wstring> out;
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                            nullptr, &outInfo, &acts, &n))) {
        for (UINT32 i = 0; i < n; ++i) {
            wchar_t* v = nullptr;
            UINT32 len = 0;
            if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, &v, &len))) {
                out.emplace_back(v);
                CoTaskMemFree(v);
            }
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
    }
    return out;
}

// ---------------- capture sources ----------------
struct Source {
    virtual ~Source() = default;
    virtual const wchar_t* name() const = 0;
    // Copies the newest frame into dst (if one arrived). Returns false on fatal error.
    virtual bool acquire(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, UINT timeoutMs, bool& gotNew) = 0;
    virtual bool ended() const { return false; }      // window capture: the window closed
    virtual std::wstring events() { return L""; }     // window capture: minimized/restored/resized
    UINT w = 0, h = 0;
};

struct DuplicationSource : Source {
    ComPtr<IDXGIOutputDuplication> dup;
    ComPtr<IDXGIOutput1> out1;
    ComPtr<ID3D11Device> dev;
    const wchar_t* name() const override { return L"DXGI Desktop Duplication"; }
    bool init(ID3D11Device* d, IDXGIOutput* output, std::wstring& err) {
        dev = d;
        HRESULT hr = output->QueryInterface(IID_PPV_ARGS(&out1));
        if (SUCCEEDED(hr)) hr = out1->DuplicateOutput(d, &dup);
        if (FAILED(hr)) { err = hrMsg(L"DuplicateOutput", hr); return false; }
        DXGI_OUTDUPL_DESC dd{};
        dup->GetDesc(&dd);
        w = dd.ModeDesc.Width; h = dd.ModeDesc.Height;
        return w && h;
    }
    bool acquire(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, UINT timeoutMs, bool& gotNew) override {
        gotNew = false;
        if (!dup) {  // re-create after access lost (mode change, UAC desktop, fullscreen switch)
            if (FAILED(out1->DuplicateOutput(dev.Get(), &dup))) { Sleep(timeoutMs); return true; }
        }
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dup->AcquireNextFrame(timeoutMs, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return true;
        if (hr == DXGI_ERROR_ACCESS_LOST) { dup.Reset(); return true; }
        if (FAILED(hr)) return false;
        if (fi.LastPresentTime.QuadPart != 0) {
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(res.As(&tex))) {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                if (td.Width == w && td.Height == h) { ctx->CopyResource(dst, tex.Get()); gotNew = true; }
            }
        }
        dup->ReleaseFrame();
        return true;
    }
};

}  // namespace
// Static high-detail CI pattern (gradients, fine checkers, thin lines, color bars). Shared with --render-pattern.
void testPattern(std::vector<uint32_t>& px, unsigned w, unsigned h) {
    px.resize((size_t)w * h);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            uint32_t r, g, b;
            if (y < h / 4) { unsigned bar = x * 8 / w; r = (bar & 1) ? 235 : 16; g = (bar & 2) ? 235 : 16; b = (bar & 4) ? 235 : 16; }
            else if (y < h / 2) { r = x * 255 / w; g = (y - h / 4) * 255 / (h / 4); b = 255 - r; }
            else if (y < 3 * h / 4) { uint32_t c = (((x >> 2) ^ (y >> 2)) & 1) ? 220 : 40; r = g = b = c; }
            else { uint32_t c = (x % 16 == 0 || y % 16 == 0) ? 230 : 30; r = c; g = c; b = (x * 7 + y * 3) & 255; }
            px[(size_t)y * w + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
}
namespace {
struct SyntheticSource : Source {
    bool still = false;
    ComPtr<ID3D11Texture2D> staging;
    std::vector<uint32_t> px;
    unsigned n = 0;
    const wchar_t* name() const override { return L"Synthetic test pattern"; }
    bool init(ID3D11Device* d) {
        w = 1280; h = 720;
        px.resize((size_t)w * h);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        return SUCCEEDED(d->CreateTexture2D(&td, nullptr, &staging));
    }
    bool acquire(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, UINT timeoutMs, bool& gotNew) override {
        Sleep(timeoutMs ? (timeoutMs < 5 ? timeoutMs : 5) : 0);
        if (still) {  // new frame every call (worst case for the encoder), identical content -> PSNR vs reference
            if (!n++) { testPattern(px, w, h); ctx->UpdateSubresource(staging.Get(), 0, nullptr, px.data(), w * 4, 0); }
            ctx->CopyResource(dst, staging.Get());
            gotNew = true;
            return true;
        }
        ++n;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
                px[(size_t)y * w + x] = 0xFF000000u | (((x + n * 4) & 255) << 16) | (((y + n * 2) & 255) << 8) | ((x ^ y) & 255);
        ctx->UpdateSubresource(staging.Get(), 0, nullptr, px.data(), w * 4, 0);
        ctx->CopyResource(dst, staging.Get());
        gotNew = true;
        return true;
    }
};

#ifdef CR_HAVE_WGC
struct WgcSourceAdapter : Source {
    WgcCapture cap;
    const wchar_t* name() const override { return window ? L"Windows Graphics Capture (window)" : L"Windows Graphics Capture"; }
    bool window = false;
    bool init(ID3D11Device* d, HMONITOR mon, bool cursor, std::wstring& err) {
        if (!cap.start(d, mon, cursor, err)) return false;
        w = cap.width(); h = cap.height();
        return true;
    }
    bool initWindow(ID3D11Device* d, HWND hw, bool cursor, std::wstring& err) {
        if (!cap.startWindow(d, hw, cursor, err)) return false;
        window = true;
        w = cap.width(); h = cap.height();
        return true;
    }
    bool ended() const override { return window && cap.closed(); }
    std::wstring events() override { return cap.takeEvents(); }
    bool acquire(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, UINT timeoutMs, bool& gotNew) override {
        return cap.copyLatest(ctx, dst, timeoutMs, gotNew);
    }
};
// All displays: one WGC capture per monitor composited at its virtual-desktop offset (gaps stay black).
struct WgcAllDisplays : Source {
    struct Mon { std::unique_ptr<WgcCapture> cap; int x, y; };
    std::vector<Mon> mons;
    POINT origin{0, 0};
    bool cleared = false;
    const wchar_t* name() const override { return L"Windows Graphics Capture (all displays)"; }
    bool init(ID3D11Device* d, bool cursor, std::wstring& err) {
        std::vector<std::pair<HMONITOR, RECT>> ms;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT r, LPARAM p) -> BOOL {
            ((std::vector<std::pair<HMONITOR, RECT>>*)p)->push_back({m, *r}); return TRUE; }, (LPARAM)&ms);
        RECT u{INT_MAX, INT_MAX, INT_MIN, INT_MIN};
        for (auto& m : ms) { u.left = std::min(u.left, m.second.left); u.top = std::min(u.top, m.second.top); u.right = std::max(u.right, m.second.right); u.bottom = std::max(u.bottom, m.second.bottom); }
        if (ms.empty()) { err = L"no displays"; return false; }
        origin = {u.left, u.top};
        for (auto& m : ms) {
            Mon x; x.cap = std::make_unique<WgcCapture>();
            if (!x.cap->start(d, m.first, cursor, err)) return false;
            x.x = m.second.left - u.left; x.y = m.second.top - u.top;
            mons.push_back(std::move(x));
        }
        w = (UINT)(u.right - u.left) & ~1u; h = (UINT)(u.bottom - u.top) & ~1u;
        return true;
    }
    bool acquire(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, UINT timeoutMs, bool& gotNew) override {
        if (!cleared) {
            ComPtr<ID3D11Device> d; ctx->GetDevice(&d); ComPtr<ID3D11RenderTargetView> rtv;
            if (SUCCEEDED(d->CreateRenderTargetView(dst, nullptr, &rtv))) { const float k[4] = {0, 0, 0, 1}; ctx->ClearRenderTargetView(rtv.Get(), k); }
            cleared = true;
        }
        gotNew = false;
        bool ok = true;
        for (size_t i = 0; i < mons.size(); ++i) {
            bool g = false;
            ok &= mons[i].cap->copyLatest(ctx, dst, i == 0 ? timeoutMs : 0, g, mons[i].x, mons[i].y);
            gotNew |= g;
        }
        return ok;
    }
};
#endif

void drawCursor(ID3D11Texture2D* frame, POINT origin) {
    ComPtr<IDXGISurface1> s;
    if (FAILED(frame->QueryInterface(IID_PPV_ARGS(&s)))) return;
    HDC dc = nullptr;
    if (FAILED(s->GetDC(FALSE, &dc))) return;
    CURSORINFO ci{sizeof ci};
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
        ICONINFO ii{};
        if (GetIconInfo(ci.hCursor, &ii)) {
            DrawIconEx(dc, ci.ptScreenPos.x - origin.x - (int)ii.xHotspot, ci.ptScreenPos.y - origin.y - (int)ii.yHotspot,
                       ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }
    }
    s->ReleaseDC(nullptr);
}

// BGRA (capture) -> NV12 (encoder) on the GPU via D3D11 Video Processor. Optional downscale.
struct GpuConvert {
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<ID3D11VideoProcessorEnumerator> venum;
    ComPtr<ID3D11VideoProcessor> vp;
    ComPtr<ID3D11Texture2D> nv12;
    ComPtr<ID3D11VideoProcessorOutputView> outView;
    UINT inW = 0, inH = 0, outW = 0, outH = 0;
    bool ok = false;

    std::wstring why;  // last init failure, with HRESULT
    bool fail(const wchar_t* what, HRESULT hr) { wchar_t b[160]; swprintf(b, 160, L"%ls: 0x%08lX", what, (unsigned long)hr); why = b; return false; }
    bool init(ID3D11Device* d, UINT srcW, UINT srcH, UINT dstW, UINT dstH) {
        inW = srcW; inH = srcH; outW = dstW; outH = dstH;
        HRESULT hr;
        if (FAILED(hr = d->QueryInterface(IID_PPV_ARGS(&vdev)))) return fail(L"QI ID3D11VideoDevice", hr);
        ComPtr<ID3D11DeviceContext> ic; d->GetImmediateContext(&ic);
        if (FAILED(hr = ic.As(&vctx))) return fail(L"QI ID3D11VideoContext", hr);
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = srcW; cd.InputHeight = srcH;
        cd.OutputWidth = dstW; cd.OutputHeight = dstH;
        cd.InputFrameRate = {60, 1}; cd.OutputFrameRate = {60, 1};
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        if (FAILED(hr = vdev->CreateVideoProcessorEnumerator(&cd, &venum))) {
            cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            if (FAILED(hr = vdev->CreateVideoProcessorEnumerator(&cd, &venum))) return fail(L"CreateVideoProcessorEnumerator", hr);
        }
        UINT fin = 0, fout = 0;
        venum->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &fin);
        venum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &fout);
        if (!(fin & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) return fail(L"VP: BGRA input unsupported", (HRESULT)fin);
        if (!(fout & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) return fail(L"VP: NV12 output unsupported", (HRESULT)fout);
        if (FAILED(hr = vdev->CreateVideoProcessor(venum.Get(), 0, &vp))) return fail(L"CreateVideoProcessor", hr);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = dstW; td.Height = dstH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(hr = d->CreateTexture2D(&td, nullptr, &nv12))) {
            td.BindFlags = D3D11_BIND_RENDER_TARGET;  // some iGPUs refuse SRV on NV12
            if (FAILED(hr = d->CreateTexture2D(&td, nullptr, &nv12))) return fail(L"CreateTexture2D(NV12 RT)", hr);
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(hr = vdev->CreateVideoProcessorOutputView(nv12.Get(), venum.Get(), &ovd, &outView))) return fail(L"CreateVideoProcessorOutputView(NV12)", hr);
        RECT src{0, 0, (LONG)srcW, (LONG)srcH}, dst{0, 0, (LONG)dstW, (LONG)dstH};
        vctx->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &src);
        vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dst);
        vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dst);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}; in.RGB_Range = 0; in.YCbCr_Matrix = 1;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{}; out.Usage = 0; out.RGB_Range = 0; out.YCbCr_Matrix = 1;
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vctx->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &in);
        vctx->VideoProcessorSetOutputColorSpace(vp.Get(), &out);
        vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
        ok = true;
        return true;
    }
    bool convert(ID3D11Texture2D* bgra) {
        if (!ok) return false;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorInputView> inView;
        if (FAILED(vdev->CreateVideoProcessorInputView(bgra, venum.Get(), &ivd, &inView))) return false;
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE; stream.pInputSurface = inView.Get();
        return SUCCEEDED(vctx->VideoProcessorBlt(vp.Get(), outView.Get(), 0, 1, &stream));
    }
};

void scaledSize(OutputScale scale, UINT inW, UINT inH, UINT& outW, UINT& outH) {
    outW = inW & ~1u; outH = inH & ~1u;
    UINT maxH = 0;
    if (scale == OutputScale::P1440) maxH = 1440;
    else if (scale == OutputScale::P1080) maxH = 1080;
    else if (scale == OutputScale::P720) maxH = 720;
    if (maxH && outH > maxH) {
        double r = (double)maxH / (double)outH;
        outH = maxH & ~1u;
        outW = ((UINT)(outW * r)) & ~1u;
        if (outW < 2) outW = 2;
    }
}

// ---------------- encoder ----------------
struct Writer {
    ComPtr<IMFSinkWriter> sw;
    ComPtr<IMFDXGIDeviceManager> mgr;
    DWORD stream = 0;
    bool gpu = false;
    std::wstring label;
    // Recordings: fragmented MP4 (moof/mdat every ~1 s). A crash, kill or stuck Finalize still leaves a playable
    // file with everything up to the last fragment; nothing has to be copied or remuxed after Stop.
    bool fragmented = false;
    bool fragmentedOk = false;  // the fMP4 sink was accepted (else plain MP4)
    const GUID& container() const { return fragmented ? MFTranscodeContainerType_FMPEG4 : MFTranscodeContainerType_MPEG4; }
    HRESULT createSink(const std::wstring& path, IMFAttributes* a) {
        HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, a, &sw);
        if (FAILED(hr) && fragmented) {  // very old MF without the fMP4 sink: plain MP4
            a->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
            fragmented = false;
            hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, a, &sw);
        }
        fragmentedOk = SUCCEEDED(hr) && fragmented;
        return hr;
    }

    // nv12In=true: samples are DXGI NV12 surfaces (zero-copy HW path). Otherwise RGB32 (CPU or MF color convert).
    bool open(const std::wstring& path, ID3D11Device* dev, UINT w, UINT h, const RecordOptions& o, bool hardware, bool nv12In, std::wstring& err) {
        ComPtr<IMFAttributes> a;
        MFCreateAttributes(&a, 4);
        a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE);
        a->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        a->SetUINT32(MF_LOW_LATENCY, TRUE);
        a->SetGUID(MF_TRANSCODE_CONTAINERTYPE, container());
        gpu = hardware;
        if (hardware) {
            UINT token = 0;
            HRESULT hr = MFCreateDXGIDeviceManager(&token, &mgr);
            if (SUCCEEDED(hr)) hr = mgr->ResetDevice(dev, token);
            if (FAILED(hr)) { err = hrMsg(L"MFCreateDXGIDeviceManager", hr); return false; }
            a->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, mgr.Get());
        }
        HRESULT hr = createSink(path, a.Get());
        if (FAILED(hr)) { err = hrMsg(L"MFCreateSinkWriterFromURL", hr); return false; }

        unsigned br = bitrateFor(o.quality, (int)w, (int)h, o.fps, o.bitrateCap);
        ComPtr<IMFMediaType> outT, inT;
        MFCreateMediaType(&outT);
        outT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outT->SetUINT32(MF_MT_AVG_BITRATE, br);
        outT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outT->SetUINT32(MF_MT_MPEG2_PROFILE, o.quality >= Quality::High ? 100u /* High */ : 77u /* Main */);
        MFSetAttributeSize(outT.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(outT.Get(), MF_MT_FRAME_RATE, (UINT32)o.fps, 1);
        MFSetAttributeRatio(outT.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = sw->AddStream(outT.Get(), &stream);
        if (FAILED(hr)) { err = hrMsg(L"AddStream", hr); return false; }

        MFCreateMediaType(&inT);
        inT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inT->SetGUID(MF_MT_SUBTYPE, nv12In ? MFVideoFormat_NV12 : MFVideoFormat_RGB32);
        inT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (!nv12In) inT->SetUINT32(MF_MT_DEFAULT_STRIDE, w * 4);
        MFSetAttributeSize(inT.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(inT.Get(), MF_MT_FRAME_RATE, (UINT32)o.fps, 1);
        MFSetAttributeRatio(inT.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = sw->SetInputMediaType(stream, inT.Get(), nullptr);
        if (FAILED(hr)) { err = hrMsg(L"SetInputMediaType", hr); return false; }

        // Low-latency encoder knobs (no B-frames, low latency mode). Best-effort — soft fail.
        ComPtr<IUnknown> unk;
        if (SUCCEEDED(sw->GetServiceForStream(stream, GUID_NULL, IID_PPV_ARGS(&unk))) && unk) {
            ComPtr<ICodecAPI> codec;
            static const GUID kIID_ICodecAPI = {0x901db4c7, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};
            if (SUCCEEDED(unk->QueryInterface(kIID_ICodecAPI, (void**)codec.GetAddressOf())) && codec) {
                VARIANT v; VariantInit(&v);
                v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
                codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
                VariantClear(&v);
                VariantInit(&v); v.vt = VT_UI4; v.ulVal = 0;  // no B-frames
                codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
                VariantClear(&v);
                VariantInit(&v); v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_CBR;
                codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
                VariantClear(&v);
                VariantInit(&v); v.vt = VT_UI4; v.ulVal = br;
                codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
                VariantClear(&v);
            }
        }

        if (wantAudio && !addAudio(err)) return false;
        hr = sw->BeginWriting();
        if (FAILED(hr)) { err = hrMsg(L"BeginWriting", hr); return false; }
        return true;
    }
    DWORD astreams[3] = {0, 0, 0};
    int tracks = 1;               // 1, or 3 (mix, system, mic)
    bool audio = false;
    UINT32 achannels = 2;
    // AAC 48 kHz track(s) encoded by the sink writer's AAC MFT from s16 PCM. Call before BeginWriting.
    bool addAudio(std::wstring& err) {
        for (int k = 0; k < tracks; ++k) {
            ComPtr<IMFMediaType> o, i;
            MFCreateMediaType(&o);
            o->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            o->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            o->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            o->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioMixer::kRate);
            o->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, achannels);
            o->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, achannels == 1 ? 16000 : 24000);  // 128 / 192 kbps
            o->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
            HRESULT hr = sw->AddStream(o.Get(), &astreams[k]);
            MFCreateMediaType(&i);
            i->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            i->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            i->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            i->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioMixer::kRate);
            i->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, achannels);
            i->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * achannels);
            i->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AudioMixer::kRate * 2 * achannels);
            if (SUCCEEDED(hr)) hr = sw->SetInputMediaType(astreams[k], i.Get(), nullptr);
            if (FAILED(hr)) { err = hrMsg(L"Audio stream (AAC)", hr); return false; }
        }
        audio = true;
        return true;
    }
    HRESULT writeAudio(const std::vector<short>& pcm, LONGLONG t, int track = 0) {
        if (!audio || pcm.empty() || track >= tracks) return S_OK;
        DWORD bytes = (DWORD)(pcm.size() * sizeof(short));
        ComPtr<IMFMediaBuffer> b; ComPtr<IMFSample> s;
        HRESULT hr = MFCreateMemoryBuffer(bytes, &b);
        BYTE* p = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(hr = b->Lock(&p, nullptr, nullptr))) { memcpy(p, pcm.data(), bytes); b->Unlock(); b->SetCurrentLength(bytes); }
        if (SUCCEEDED(hr)) hr = MFCreateSample(&s);
        if (SUCCEEDED(hr)) { s->AddBuffer(b.Get()); s->SetSampleTime(t); s->SetSampleDuration((LONGLONG)(pcm.size() / achannels) * 10000000LL / AudioMixer::kRate); }
        if (SUCCEEDED(hr)) hr = sw->WriteSample(astreams[track], s.Get());
        return hr;
    }
    bool wantAudio = false;
    // Mux-only sink writer for samples already encoded by our own hardware MFT (no transforms inside MF).
    bool openPassthrough(const std::wstring& path, IMFMediaType* encOut, std::wstring& err) {
        ComPtr<IMFAttributes> a;
        MFCreateAttributes(&a, 3);
        a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
        a->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        a->SetGUID(MF_TRANSCODE_CONTAINERTYPE, container());
        gpu = true;
        HRESULT hr = createSink(path, a.Get());
        if (FAILED(hr)) { err = hrMsg(L"MFCreateSinkWriterFromURL(passthrough)", hr); return false; }
        hr = sw->AddStream(encOut, &stream);
        if (SUCCEEDED(hr)) hr = sw->SetInputMediaType(stream, encOut, nullptr);
        if (FAILED(hr)) { err = hrMsg(L"Passthrough stream", hr); return false; }
        if (wantAudio && !addAudio(err)) return false;
        hr = sw->BeginWriting();
        if (FAILED(hr)) { err = hrMsg(L"BeginWriting(passthrough)", hr); return false; }
        return true;
    }
    HRESULT write(IMFMediaBuffer* buf, LONGLONG t, LONGLONG d) {
        ComPtr<IMFSample> s;
        HRESULT hr = MFCreateSample(&s);
        if (SUCCEEDED(hr)) hr = s->AddBuffer(buf);
        if (SUCCEEDED(hr)) hr = s->SetSampleTime(t);
        if (SUCCEEDED(hr)) hr = s->SetSampleDuration(d);
        if (SUCCEEDED(hr)) hr = sw->WriteSample(stream, s.Get());
        return hr;
    }
};

std::wstring exeDir() {
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p, n);
    size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}



// ---- abandon bookkeeping (process lifetime) ----
struct FinState {
    enum { Running = 0, Done = 1, Abandoned = 2 };
    std::atomic<int> state{Running};
    std::atomic<HRESULT> hr{E_FAIL};
};
std::atomic<int> g_abandonLive{0};
std::atomic<DWORD> g_testHangFinalizeMs{0};
// Finalize gets half of finalizeTimeoutMs (default 8000 -> 4 s); remux gets the other half.
DWORD finalizeBudgetMs(DWORD finalizeTimeoutMs) { return (finalizeTimeoutMs ? finalizeTimeoutMs : 8000) / 2; }
}  // namespace

void setTestHangFinalizeMs(DWORD ms) { g_testHangFinalizeMs = ms; }
std::atomic<DWORD> g_testSlowPostMs{0};
void setTestSlowPostMs(DWORD ms) { g_testSlowPostMs = ms; }
std::atomic<DWORD> g_testLatePublishMs{0};
void prewarmCaptureAsync() {
#ifdef CR_HAVE_WGC
    WgcCapture::prewarmAsync();
#endif
}
void setTestLatePublishMs(DWORD ms) { g_testLatePublishMs = ms; }
std::atomic<int> g_testLateLost{0};
int testLatePublishLost() { return g_testLateLost.load(); }
static unsigned long long fileSizeOf(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    return GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) ? (((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) : 0;
}
int abandonedFinalizeLive() { return g_abandonLive.load(); }

std::wstring findFfmpeg() {
    std::wstring local = exeDir() + L"\\ffmpeg.exe";
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) return local;
    // Walk PATH ourselves (SearchPathW checks the current directory first). Only absolute entries
    // (drive-letter or UNC) are used; empty, ".", and relative entries are skipped.
    std::vector<wchar_t> env(32768);
    DWORD n = GetEnvironmentVariableW(L"PATH", env.data(), (DWORD)env.size());
    if (!n || n >= env.size()) return {};
    std::wstring path(env.data(), n);
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t sep = path.find(L';', pos);
        if (sep == std::wstring::npos) sep = path.size();
        std::wstring dir = path.substr(pos, sep - pos);
        pos = sep + 1;
        while (!dir.empty() && (dir.front() == L' ' || dir.front() == L'"')) dir.erase(0, 1);
        while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'"')) dir.pop_back();
        bool drive = dir.size() >= 3 && iswalpha(dir[0]) && dir[1] == L':' && (dir[2] == L'\\' || dir[2] == L'/');
        bool unc = dir.size() >= 3 && dir[0] == L'\\' && dir[1] == L'\\';
        if (!drive && !unc) continue;
        if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
        std::wstring cand = dir + L"ffmpeg.exe";
        DWORD a = GetFileAttributesW(cand.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return cand;
    }
    return {};
}

std::wstring describeHardwareEncoders() {
    std::wstring s;
    for (auto& v : hardwareVendors()) {
        std::wstring l = vendorLabel(v);
        if (s.find(l) != std::wstring::npos) continue;
        if (!s.empty()) s += L", ";
        s += l;
    }
    return s;
}

// Process-wide encoder probe cache: (adapter LUID, encoder ordinal, input format) -> worked?
// Filled by the startup warm-up so pressing Record never waits on probes (3 AMD MFTs x ~1.5-2.5 s each).
static std::atomic<bool> g_writerWarm{false};
static std::mutex g_probeM;
static std::map<std::wstring, int> g_probe;  // 1 ok, 0 failed, 2 being probed (by the warm-up)
static std::condition_variable g_probeCv;
static std::wstring probeKey(ID3D11Device* dev, const HwEncoderInfo& e) {
    ComPtr<IDXGIDevice> d; ComPtr<IDXGIAdapter> a; DXGI_ADAPTER_DESC ad{};
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d))) && SUCCEEDED(d->GetAdapter(&a))) a->GetDesc(&ad);
    wchar_t b[128];
    swprintf(b, 128, L"%08lX:%08lX/%d/%ls/%d/%ls", (unsigned long)ad.AdapterLuid.HighPart, (unsigned long)ad.AdapterLuid.LowPart, e.index, e.vendor.c_str(), e.argb ? 1 : 0, codecName(e.subtype));
    return b;
}

static AbandonLatch& processLatch() { static AbandonLatch* l = new AbandonLatch; return *l; }  // never destroyed

Recorder::Recorder() : abandon_(processLatch()) {
    doneEvent_ = CreateEventW(nullptr, TRUE, TRUE, nullptr);  // starts signaled (Idle)
}
Recorder::~Recorder() {
    stop();
    if (!waitUntilIdle(3000)) abort();
    waitUntilIdle(2000);
    if (th_.joinable()) { if (workers_.load() == 0) th_.join(); else th_.detach(); }  // never std::terminate
    if (doneEvent_) CloseHandle(doneEvent_);
}

// x.mkv -> x.mp4 (or "x (2).mp4" if that is taken); x.mp4 -> x.mp4
static std::wstring recordingMp4Path(const std::wstring& out) {
    size_t slash = out.find_last_of(L"\\/"), dot = out.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) ? L"" : out.substr(dot);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    if (ext == L".mp4") return out;
    std::wstring base = ext.empty() ? out : out.substr(0, dot), p = base + L".mp4";
    for (int k = 2; GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES && k < 1000; ++k) p = base + L" (" + std::to_wstring(k) + L").mp4";
    return p;
}

bool Recorder::start(const RecordOptions& o) {
    // After an abandoned Finalize/Close, Media Foundation is poisoned for this process.
    if (!abandon_.canStart()) return false;
    for (int i = 0; i < 40 && workers_.load() > 0; ++i) Sleep(50);
    if (workers_.load() > 0) return false;
    if (!fsm_.tryStart()) return false;
    if (th_.joinable()) th_.join();
    opt_ = o;
    opt_.fps = normalizeFps(o.fps);
    if (!opt_.finalizeTimeoutMs) opt_.finalizeTimeoutMs = 8000;
    result_ = RecordResult{};
    pausedQpc_ = 0;
    pausedTotal_ = 0;
    stop_ = false;
    started_ = false;
    sessionGen_ = claim_.arm();
    sessionOut_ = opt_.outputPath;
    // Every recording is written once, straight to its final .mp4 (fragmented). MKV is a stream copy made afterwards.
    sessionTmp_ = opt_.bufferOnly ? std::wstring() : recordingMp4Path(opt_.outputPath);
    { std::lock_guard<std::mutex> g(fileM_); curFile_ = sessionTmp_; }
    if (doneEvent_) ResetEvent(doneEvent_);
    th_ = std::thread([this] { run(); });
    return true;
}

void Recorder::stop() {
    stop_ = true;
    fsm_.requestStop();  // Recording -> Stopping (idempotent if already Stopping/Finishing)
}

bool Recorder::waitUntilIdle(DWORD timeoutMs) {
    if (!doneEvent_) return !fsm_.busy();
    return WaitForSingleObject(doneEvent_, timeoutMs) == WAIT_OBJECT_0;
}

void Recorder::abort() {
    stop_ = true;
    fsm_.requestStop();
    // Give Finalize / remux their remaining budget, then detach so Exit always works.
    waitUntilIdle(2000);
    if (th_.joinable() && workers_.load() > 0) {
        th_.detach();
        // Prefer leak over UAF: poison MF so we never MFShutdown while Finalize may still run.
        abandon_.abandon();
    }
    // Only the first of {abort, worker} publishes. Files are never deleted here: recordings are fragmented MP4, so
    // whatever reached the disk is playable (only an empty file is removed).
    const std::wstring tmpP = sessionTmp_;
    claim_.claim(sessionGen_, [&] {
        RecordResult pub;
        if (!tmpP.empty()) { if (fileSizeOf(tmpP) > 0) pub.file = tmpP; else DeleteFileW(tmpP.c_str()); }
        pub.timedOut = true;
        pub.abandoned = abandon_.poisoned.load();
        pub.message = pub.file.empty() ? L"Recording cancelled" : L"Recording stopped before it finished saving - kept what was written";
        result_ = std::move(pub);
        fsm_.toIdle();
        started_ = true;
        if (doneEvent_) SetEvent(doneEvent_);
        if (opt_.notify) PostMessageW(opt_.notify, opt_.warmupOnly ? WM_APP_WARMUP_DONE : opt_.doneMsg ? opt_.doneMsg : WM_APP_RECORDER_DONE, 0, 0);
    });
}

bool Recorder::pause() {
    if (!fsm_.pause()) return false;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    pausedQpc_ = q.QuadPart;
    return true;
}

bool Recorder::resume() {
    if (!fsm_.isPaused()) return false;
    long long ps = pausedQpc_.load();
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    // UI clock only: count pause time after the recording clock started (not warm-up), and publish it
    // before the state flips so elapsedSeconds() never sees Recording with a stale total.
    long long from = ps;
    if (!started_ || startQpc_.load() == 0) from = q.QuadPart;      // whole pause was during warm-up
    else if (from < startQpc_.load()) from = startQpc_.load();
    long long add = ps ? q.QuadPart - from : 0;
    if (add < 0) add = 0;
    pausedTotal_ += add;
    if (!fsm_.resume()) { pausedTotal_ -= add; return false; }
    pausedQpc_ = 0;
    return true;
}

void Recorder::join() {
    if (th_.joinable()) th_.join();
}

double Recorder::elapsedSeconds() const {
    if (!isActive() || !started_) return 0;
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    long long end = isPaused() && pausedQpc_.load() ? pausedQpc_.load() : q.QuadPart;
    if (end < startQpc_.load()) end = startQpc_.load();  // paused during warm-up
    double v = double(end - startQpc_ - pausedTotal_) / double(f.QuadPart);
    return v > 0 ? v : 0;
}

void Recorder::run() {
    workers_.fetch_add(1);
    struct WorkerGuard {
        std::atomic<int>& w;
        explicit WorkerGuard(std::atomic<int>& x) : w(x) {}
        ~WorkerGuard() { w.fetch_sub(1); }
    } workerGuard{workers_};
    const unsigned gen = sessionGen_;
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool mfStarted = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    // 1 ms timer resolution (Sleep/waits would otherwise round to 15.6 ms and starve 60 fps pacing) + MMCSS "Capture".
    timeBeginPeriod(1);
    HANDLE mmcss = nullptr;
    {
        typedef HANDLE(WINAPI * AvSet)(LPCWSTR, LPDWORD);
        HMODULE av = LoadLibraryExW(L"avrt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        DWORD task = 0;
        if (av) if (auto f = (AvSet)GetProcAddress(av, "AvSetMmThreadCharacteristicsW")) mmcss = f(L"Capture", &task);
        if (!mmcss) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    }
    RecordResult r;
    bool abandonedMf = false;
    std::wstring err;
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    long long lastMark = opt_.pressQpc;
    if (!lastMark) { LARGE_INTEGER q; QueryPerformanceCounter(&q); lastMark = q.QuadPart; }
    const long long pressQ = lastMark;
    auto mark = [&](const wchar_t* stage) {
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        wchar_t b[96]; swprintf(b, 96, L"%ls=%.0fms ", stage, (double)(q.QuadPart - lastMark) * 1000.0 / qf.QuadPart);
        r.latencyLog += b; lastMark = q.QuadPart;
    };
    mark(L"thread_start");

    try { [&] {
        // ---- device on the adapter that owns the primary monitor (or an explicit gpuIndex) ----
        HMONITOR mon = opt_.monitor ? opt_.monitor : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (opt_.window) { HMONITOR wm = MonitorFromWindow(opt_.window, MONITOR_DEFAULTTONULL); if (wm) mon = wm; }
        ComPtr<IDXGIFactory1> fac;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) { r.message = L"DXGI unavailable"; return; }
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput> output;
        POINT origin{0, 0};
        std::wstring adapterName;
        if (opt_.gpuIndex >= 0) {
            ComPtr<IDXGIAdapter1> pick;
            if (SUCCEEDED(fac->EnumAdapters1((UINT)opt_.gpuIndex, &pick))) {
                adapter = pick;
                DXGI_ADAPTER_DESC1 ad{}; pick->GetDesc1(&ad); adapterName = ad.Description;
                ComPtr<IDXGIOutput> o;
                for (UINT j = 0; pick->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j) {
                    DXGI_OUTPUT_DESC od{}; o->GetDesc(&od);
                    if (od.Monitor == mon) { output = o; origin = {od.DesktopCoordinates.left, od.DesktopCoordinates.top}; break; }
                    o.Reset();
                }
                // Primary monitor only: if this GPU doesn't drive it, drop its other displays and capture the
                // primary monitor cross-adapter (WGC), so --gpu changes the encoder/device, never the content.
                if (output) { DXGI_OUTPUT_DESC od{}; output->GetDesc(&od); if (od.Monitor != mon) output.Reset(); }
            }
        }
        if (!adapter) {
            for (UINT i = 0; !output && fac->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                ComPtr<IDXGIOutput> o;
                for (UINT j = 0; adapter->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j) {
                    DXGI_OUTPUT_DESC od{};
                    o->GetDesc(&od);
                    if (od.Monitor == mon) {
                        output = o; origin = {od.DesktopCoordinates.left, od.DesktopCoordinates.top};
                        DXGI_ADAPTER_DESC1 ad{}; adapter->GetDesc1(&ad); adapterName = ad.Description;
                        break;
                    }
                    o.Reset();
                }
                if (!output) adapter.Reset();
            }
        }
        if (!output) {  // selected GPU drives no display (e.g. laptop dGPU): WGC copies across adapters
            MONITORINFO mi{sizeof mi};
            if (GetMonitorInfoW(mon, &mi)) origin = {mi.rcMonitor.left, mi.rcMonitor.top};
        }
        if (adapterName.empty() && adapter) { DXGI_ADAPTER_DESC1 ad{}; adapter->GetDesc1(&ad); adapterName = ad.Description; }
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       flags, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        bool videoSupport = SUCCEEDED(hr);
        if (FAILED(hr)) {  // e.g. Microsoft Basic Render Driver (VMs, CI): no video support, software encode only
            flags &= ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
            hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        }
        if (FAILED(hr))
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        if (FAILED(hr)) { r.message = hrMsg(L"D3D11CreateDevice", hr); return; }
        mark(L"device");
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
        UINT adapterVendor = 0;
        {
            ComPtr<IDXGIDevice> dxgiDev; ComPtr<IDXGIAdapter> ad;
            if (SUCCEEDED(dev.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&ad))) { DXGI_ADAPTER_DESC d{}; ad->GetDesc(&d); adapterVendor = d.VendorId; }
        }

        // Audio devices open in parallel with capture/encoder setup (WASAPI init is 50-150 ms per device).
        // The start tone is muted out of system audio: it plays when the first frame arrives (t0), so 350 ms
        // after t0 covers its 160 ms + output latency. Nothing waits on the tone.
        AudioMixer mixer;
        std::wstring alog;
        mixer.systemMuteUntil = 3000000;  // start tone (only where it could be captured: mic / no self-exclusion)
        auto audioReady = std::async(std::launch::async, [&] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            mixer.channels = opt_.channels == 1 ? 1 : 2;
            if (!opt_.warmupOnly) mixer.start(opt_.systemAudio, opt_.mic, opt_.micId, opt_.testTone, alog, opt_.systemId, opt_.testMic, false);
            CoUninitialize();
        });
        struct AudioJoin { std::future<void>& f; ~AudioJoin() { if (f.valid()) f.wait(); } } audioJoin{audioReady};

        // ---- capture source: WGC -> Desktop Duplication -> synthetic (tests only) ----
        std::unique_ptr<Source> src;
        bool cursorByGdi = false;
        std::wstring capErr;
        if (opt_.window) {  // single window: WGC only, no silent fallback to the whole screen
#ifdef CR_HAVE_WGC
            auto w = std::make_unique<WgcSourceAdapter>();
            if (w->initWindow(dev.Get(), opt_.window, opt_.captureCursor, capErr)) src = std::move(w);
#else
            capErr = L"window capture needs Windows Graphics Capture (MSVC build)";
#endif
            if (!src) { r.message = L"Window capture unavailable: " + capErr; return; }
        }
#ifdef CR_HAVE_WGC
        if (!src && !opt_.forcePattern && opt_.allDisplays && listDisplays().size() > 1) {
            auto w = std::make_unique<WgcAllDisplays>();
            if (w->init(dev.Get(), opt_.captureCursor, capErr)) src = std::move(w);
            else r.encoderLog += L"all displays: " + capErr + L" (falling back to one monitor)\n";
        }
        if (!src && !opt_.forcePattern) {
            auto w = std::make_unique<WgcSourceAdapter>();
            if (w->init(dev.Get(), mon, opt_.captureCursor, capErr)) src = std::move(w);
        }
#endif
        if (!src && output && !opt_.forcePattern) {
            auto d = std::make_unique<DuplicationSource>();
            if (d->init(dev.Get(), output.Get(), capErr)) { src = std::move(d); cursorByGdi = opt_.captureCursor; }
        }
        if (!src && (opt_.allowSynthetic || opt_.forcePattern)) {
            auto s = std::make_unique<SyntheticSource>();
            s->still = opt_.forcePattern;
            if (s->init(dev.Get())) src = std::move(s);
        }
        if (!src) { r.message = L"Screen capture unavailable: " + (capErr.empty() ? std::wstring(L"no output") : capErr); return; }
        r.captureUsed = src->name();
        mark(L"capture_init");
        UINT W = 0, H = 0;  // encoded size (even; optionally downscaled)
        scaledSize(opt_.scale, src->w, src->h, W, H);
        if (W > 4096 || H > 4096) {  // H.264 level 5.2 limit (wide multi-display desktops): fit inside 4096
            double k = 4096.0 / (double)std::max(W, H);
            W = (UINT)(W * k) & ~1u; H = (UINT)(H * k) & ~1u;
            r.encoderLog += L"capture larger than 4096: encoding " + std::to_wstring(W) + L"x" + std::to_wstring(H) + L"\n";
        }

        D3D11_TEXTURE2D_DESC td{};
        td.Width = src->w; td.Height = src->h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
        ComPtr<ID3D11Texture2D> latest;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &latest))) { r.message = L"CreateTexture2D failed"; return; }
        // ---- audio (system loopback + mic), mixed on the video media clock ----
        Writer wr;
        wr.fragmented = !opt_.warmupOnly && !opt_.bufferOnly;
        wr.tracks = opt_.separateTracks ? 3 : 1;
        struct AChunk { long long t; std::vector<short> pcm[3]; };
        std::deque<AChunk> audioPending;  // until the lazy writer opens
        HRESULT audioHr = S_OK;
        const bool toFile = !opt_.bufferOnly;
        ReplayRing* replay = opt_.ring.get();
        bool ringAttached = false;
        auto writeChunk = [&](const AChunk& c) -> HRESULT {
            HRESULT h = S_OK;
            for (int k = 0; k < wr.tracks && SUCCEEDED(h); ++k) h = wr.writeAudio(c.pcm[k], c.t, k);
            return h;
        };
        auto pushAudio = [&](long long mediaTime, bool final) {
            if (!wr.wantAudio) return;
            mixer.micMuted = micMuted_.load();
            AChunk c; c.t = 0;
            const bool split = wr.tracks == 3;
            mixer.produce(mediaTime, final, c.pcm[0], c.t, split ? &c.pcm[1] : nullptr, split ? &c.pcm[2] : nullptr);
            if (c.pcm[0].empty()) return;
            if (replay && ringAttached) { const std::vector<short>* tr[3] = {&c.pcm[0], split ? &c.pcm[1] : nullptr, split ? &c.pcm[2] : nullptr}; replay->addAudio(c.t, tr); }
            if (!toFile) return;
            if (!wr.sw) { audioPending.push_back(std::move(c)); return; }
            while (!audioPending.empty()) { writeChunk(audioPending.front()); audioPending.pop_front(); }
            HRESULT ah = writeChunk(c);
            if (FAILED(ah)) audioHr = ah;
        };

        // ---- encoder selection ----
        // Hardware: only encoders registered for THIS device's adapter (MFT_ENUM_ADAPTER_LUID), each one
        // pre-flight probed with a short real encode; a failing/stalling one is skipped (leaked, not waited on).
        // Order: requested vendor first (if any), then the other HW encoders on this adapter, then software.
        const std::wstring tmp = sessionTmp_;
        GpuConvert conv;
        bool zeroCopy = false;
        bool bgraDirect = false;  // HW encoder fed BGRA textures (no VideoProcessor)
        std::unique_ptr<HwEncoder> hw;
        bool opened = false;
        std::wstring& elog = r.encoderLog;
        // Encoder/writer state, filled by setup(). For a real recording setup() runs on its own thread while
        // capture has already started (frames are buffered), so Record is not held up by encoder init.
        bool swNv12 = false;
        D3D11_TEXTURE2D_DESC ed{};
        const int RING = 16;  // > encoder queue + in-flight, so the async MFT never reads a surface we overwrite
        ComPtr<ID3D11Texture2D> ring[RING];
        ComPtr<ID3D11Texture2D> staging, lastGood, cpuStaging;
        bool haveGood = false, cpuFallback = false;
        long long failSince = 0;
        std::vector<uint8_t> nv12Cpu;
        D3D11_BOX box{};
        // Encoded-sample sink for the hardware path: the MP4 writer is opened on the first output so it gets the
        // encoder's final output type (sequence header).
        HRESULT muxHr = S_OK;
        auto resetWriter = [&] { bool wa = wr.wantAudio, fr = wr.fragmented; int tr = wr.tracks; UINT32 ch = wr.achannels; wr = Writer{}; wr.wantAudio = wa; wr.tracks = tr; wr.achannels = ch; wr.fragmented = fr; };
        auto muxOut = [&](IMFSample* es) -> HRESULT {
            if (replay) {
                if (!ringAttached) { ComPtr<IMFMediaType> ot = hw->muxType(); replay->attach(ot.Get(), opt_.fps, (int)wr.achannels, wr.tracks, wr.wantAudio); ringAttached = true; }
                replay->addVideo(es);
            }
            if (!toFile) return S_OK;
            if (!wr.sw) {
                ComPtr<IMFMediaType> ot = hw->muxType();
                std::wstring e2;
                if (!wr.openPassthrough(tmp, ot.Get(), e2)) { resetWriter(); muxHr = E_FAIL; err = e2; return E_FAIL; }
            }
            HRESULT h2 = wr.sw->WriteSample(wr.stream, es);
            if (FAILED(h2)) muxHr = h2;
            return h2;
        };
        unsigned long long capArrivals = 0, convN = 0, encN = 0;
        double convMs = 0, encMs = 0;
        LONGLONG lastOutTick = 0;
        unsigned long long lastOutCount = 0;
        auto setup = [&]() -> bool {
            audioReady.wait();
            r.encoderLog += alog;
            mark(L"audio_wait");
            wr.wantAudio = mixer.active() || opt_.warmupOnly;
            wr.achannels = (UINT32)mixer.channels;
            r.hasAudio = wr.wantAudio;
            r.audioTracks = wr.wantAudio ? wr.tracks : 0;
            // Codec order: explicit codec first (H.264 as the last resort); Auto = AV1 > HEVC > H.264 for MKV, H.264 for MP4.
            std::vector<GUID> codecs;
            if (opt_.codec == Codec::AV1) codecs = {MFVideoFormat_AV1};
            else if (opt_.codec == Codec::HEVC) codecs = {MFVideoFormat_HEVC};
            else if (opt_.codec == Codec::Auto && opt_.container == Container::MKV && !opt_.autoRemux) codecs = {MFVideoFormat_AV1, MFVideoFormat_HEVC};
            // Auto + auto-remux ends up as MP4: H.264 for maximum compatibility (HEVC/AV1 need extra codecs to play).
            codecs.push_back(MFVideoFormat_H264);
            // A non-H.264 encoder only counts if the MP4 sink can mux it (pass-through AddStream on a scratch file).
            auto muxable = [&](HwEncoder& enc) {
                ComPtr<IMFMediaType> t = enc.muxType();
                GUID st{}; t->GetGUID(MF_MT_SUBTYPE, &st);
                if (st == MFVideoFormat_H264) return true;
                wchar_t tp[MAX_PATH], tf[MAX_PATH];
                if (!GetTempPathW(MAX_PATH, tp) || !GetTempFileNameW(tp, L"crm", 0, tf)) return false;
                std::wstring scratch = std::wstring(tf) + L".mp4", e3;
                bool ok = false;
                { Writer pw; pw.wantAudio = false; ok = pw.openPassthrough(scratch, t.Get(), e3); pw.sw.Reset(); }
                DeleteFileW(scratch.c_str()); DeleteFileW(tf);
                elog += std::wstring(L"mux check ") + codecName(st) + (ok ? L": ok\n" : L": MP4 sink refused (" + e3 + L")\n");
                return ok;
            };
            bool vpTried = false, vpOk = false;
            for (const GUID& st : codecs) {
            if (opened) break;
            if (opt_.encoder != Encoder::Software && videoSupport) {
                auto encs = hwEncodersForDevice(dev.Get(), elog, st);
                const UINT want = vendorPci(opt_.encoder);
                std::stable_sort(encs.begin(), encs.end(), [want](const HwEncoderInfo& a, const HwEncoderInfo& b) {
                    return (want && a.pciVendor == want) > (want && b.pciVendor == want);
                });
                if (!encs.empty() && !vpTried) { vpTried = true; vpOk = conv.init(dev.Get(), src->w, src->h, W, H); }
                if (!encs.empty() && !vpOk) {
                    if (!bgraDirect) elog += L"GPU NV12 conversion unavailable (" + conv.why + L"): feeding the encoder BGRA surfaces directly\n";
                    conv = GpuConvert{};
                    W = src->w & ~1u; H = src->h & ~1u;  // no GPU scaler on this path: native size
                    for (auto& e : encs) e.argb = true;
                    bgraDirect = true;
                }
                if (!encs.empty()) {
                    for (auto& e : encs) {
                        elog += L"probe " + e.name + L"\n";
                        const std::wstring key = probeKey(dev.Get(), e);
                        int cached = -1;
                        {
                        // Another session (the launch warm-up) is probing this encoder: wait for its verdict, don't
                        // run a second concurrent probe (AMD/NVENC session limits). Capture is already running.
                        std::unique_lock<std::mutex> g(g_probeM);
                        g_probeCv.wait_for(g, std::chrono::seconds(6), [&] { auto it = g_probe.find(key); return it == g_probe.end() || it->second != 2; });
                        auto it = g_probe.find(key);
                        if (it != g_probe.end() && it->second != 2) cached = it->second;
                        else g_probe[key] = 2;
                    }
                        if (cached == 0) { elog += L"probe " + e.name + L": cached FAIL, skipped\n"; r.encoderFallbacks.push_back(e.name); continue; }
                        if (cached == 1) elog += L"probe " + e.name + L": cached OK\n";
                        else {
                            bool okp = probeHwEncoder(dev.Get(), e, W, H, opt_.fps, 2500, elog);
                            { std::lock_guard<std::mutex> g(g_probeM); g_probe[key] = okp ? 1 : 0; }
                        g_probeCv.notify_all();
                            mark(L"probe");
                            if (!okp) { r.encoderFallbacks.push_back(e.name); continue; }
                        }
                        auto enc = std::make_unique<HwEncoder>();
                        unsigned br = bitrateFor(opt_.quality, (int)W, (int)H, opt_.fps, opt_.bitrateCap);
                        if (enc->open(dev.Get(), e, W, H, opt_.fps, br, opt_.quality >= Quality::High, elog) && muxable(*enc)) {
                            hw = std::move(enc); zeroCopy = true; opened = true;
                            r.encoderUsed = std::wstring(vendorLabel(e.vendor)) + L" (hardware)";
                            break;
                        }
                        enc->release();
                        r.encoderFallbacks.push_back(e.name);
                    }
                }
                if (!opened) bgraDirect = false;
            }
            swNv12 = false;  // software MFT fed CPU NV12, muxed by the same pass-through writer as hardware
            if (!opened) {
                conv = GpuConvert{};
                W = src->w & ~1u; H = src->h & ~1u; scaledSize(opt_.scale, src->w, src->h, W, H);
                if (W > 4096 || H > 4096) { double k = 4096.0 / (double)std::max(W, H); W = (UINT)(W * k) & ~1u; H = (UINT)(H * k) & ~1u; }
                auto enc = std::make_unique<HwEncoder>();
                unsigned br = bitrateFor(opt_.quality, (int)W, (int)H, opt_.fps, opt_.bitrateCap);
                if (enc->openSoftware(W, H, opt_.fps, br, opt_.quality >= Quality::High, elog, st) && muxable(*enc)) {
                    hw = std::move(enc); swNv12 = true; opened = true;
                    r.encoderUsed = L"Software (Media Foundation " + std::wstring(codecName(st)) + L")";
                    if (opt_.encoder != Encoder::Software) r.encoderUsed += L", hardware encoder unavailable";
                } else enc->release();
            }
            if (opened) r.codec = codecName(st);
            }
            if (!opened && !toFile) { r.message = L"Replay buffer needs an encoder MFT (none could be opened)"; return false; }
            if (!opened) {
                opened = wr.open(tmp, dev.Get(), W, H, opt_, false, false, err);
                r.encoderUsed = L"Software (Media Foundation H.264)";
                if (opt_.encoder != Encoder::Software) r.encoderUsed += L", hardware encoder unavailable";
                r.codec = L"H.264";
            }
            if (!opened) { resetWriter(); DeleteFileW(tmp.c_str()); r.message = L"Encoder: " + err; return false; }
            const bool scaling = W != (src->w & ~1u) || H != (src->h & ~1u);
            if (!zeroCopy && !swNv12 && scaling) {  // CPU / RGB path can't scale: encode native instead
                resetWriter(); DeleteFileW(tmp.c_str());
                W = src->w & ~1u; H = src->h & ~1u;
                opened = wr.open(tmp, dev.Get(), W, H, opt_, false, false, err);
                if (!opened) { resetWriter(); DeleteFileW(tmp.c_str()); r.message = L"Encoder: " + err; return false; }
            }

            mark(L"encoder");
            // ---- textures ----
            // Ring of encode surfaces so the async encoder never reads a texture we're overwriting.
            ed = td; ed.Width = W; ed.Height = H; ed.MiscFlags = 0;
            if (zeroCopy && !bgraDirect) ed.Format = DXGI_FORMAT_NV12;
            for (auto& t : ring) if (FAILED(dev->CreateTexture2D(&ed, nullptr, &t))) { r.message = L"CreateTexture2D(ring) failed"; return false; }
            // Zero-copy safety net: last good NV12 frame (separate texture, survives processor rebuilds) and a
            // BGRA staging texture for the CPU fallback if the GPU video processor keeps failing.
            if (swNv12) {
                D3D11_TEXTURE2D_DESC sd = td; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(dev->CreateTexture2D(&sd, nullptr, &cpuStaging))) { r.message = L"CreateTexture2D(cpuStaging) failed"; return false; }
                nv12Cpu.resize((size_t)W * H * 3 / 2);
            }
            if (zeroCopy) {
                if (!bgraDirect && FAILED(dev->CreateTexture2D(&ed, nullptr, &lastGood))) { r.message = L"CreateTexture2D(lastGood) failed"; return false; }
            }
            if (!zeroCopy && !swNv12) {
                D3D11_TEXTURE2D_DESC sd = ed; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) { r.message = L"CreateTexture2D(staging) failed"; return false; }
            }
            box = D3D11_BOX{0, 0, 0, W, H, 1};

            // ---- pre-warm: the pass-through MP4 writer opens on the first encoded frame; load the MP4 sink and the
            // AAC encoder once now so that open doesn't stall the paced loop (CI saw ~250 ms -> 8 dropped frames).
            if (hw && !g_writerWarm.exchange(true)) {  // once per process (DLL/MFT load); later starts skip it
                wchar_t tp[MAX_PATH], tf[MAX_PATH];
                if (GetTempPathW(MAX_PATH, tp) && GetTempFileNameW(tp, L"crw", 0, tf)) {
                    std::wstring warm = std::wstring(tf) + L".mp4";
                    {
                        Writer pw; pw.wantAudio = wr.wantAudio; pw.tracks = wr.tracks; pw.achannels = wr.achannels; pw.fragmented = true; std::wstring e3;
                        ComPtr<IMFMediaType> vt = hw->muxType();
                        pw.openPassthrough(warm, vt.Get(), e3);
                        pw.sw.Reset();
                    }
                    DeleteFileW(warm.c_str()); DeleteFileW(tf);
                }
            }

            mark(L"writer_prewarm");
            return true;
            };

        if (opt_.warmupOnly) {
            setup();
            r.message = L"warm-up done"; resetWriter(); DeleteFileW(tmp.c_str()); return;
        }
        auto setupF = std::async(std::launch::async, [&] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            bool ok = setup();
            CoUninitialize();
            return ok;
        });
        struct SetupJoin { std::future<bool>& f; ~SetupJoin() { if (f.valid()) f.wait(); } } setupJoin{setupF};

        // ---- paced loop: high-res waitable timer, duplicate last frame when nothing new ----
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        LARGE_INTEGER freq, t0{};
        QueryPerformanceFrequency(&freq);
        const LONGLONG dur = 10000000LL / opt_.fps;  // 100 ns units
        long long next = 0;  // frame index
        bool haveFrame = false;
        for (int warm = 0; warm < 60 && !stop_ && !haveFrame; ++warm) {
            bool got = false;
            if (!src->acquire(ctx.Get(), latest.Get(), 50, got)) { r.message = L"Capture failed"; break; }
            haveFrame = got;
        }
        if (!haveFrame && r.message.empty()) r.message = L"No frame captured within 3 s";
        QueryPerformanceCounter(&t0);
        startQpc_ = t0.QuadPart;
        started_ = true;
        r.startMs = (double)(t0.QuadPart - pressQ) * 1000.0 / qf.QuadPart;
        mixer.setOrigin(t0.QuadPart, freq.QuadPart);  // audio is placed against the same QPC zero
        if (opt_.notify && haveFrame && !opt_.bufferOnly) PostMessageW(opt_.notify, WM_APP_RECORDER_STARTED, 0, 0);

        // ---- encoder still initialising: keep the timeline running, buffer distinct frames (bounded) ----
        struct Pending { long long idx; int slot; };
        std::vector<Pending> backlog;
        std::vector<ComPtr<ID3D11Texture2D>> slots;
        const size_t kMaxSlots = 30;
        size_t bi = 0;
        {
            bool fresh = true;
            int lastSlot = -1;
            D3D11_TEXTURE2D_DESC bd = td; bd.MiscFlags = 0;
            while (haveFrame && !stop_ && setupF.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                LARGE_INTEGER now; QueryPerformanceCounter(&now);
                long long due = (now.QuadPart - t0.QuadPart) * opt_.fps / freq.QuadPart;
                if (due < next) {
                    long long target = t0.QuadPart + next * freq.QuadPart / opt_.fps;
                    LONGLONG waitMs = (target - now.QuadPart) * 1000 / freq.QuadPart;
                    bool got = false;
                    if (!src->acquire(ctx.Get(), latest.Get(), (UINT)(waitMs > 1 ? waitMs - 1 : 1), got)) { r.message = L"Capture failed"; break; }
                    if (got) { ++capArrivals; fresh = true; }
                    continue;
                }
                if (fresh || lastSlot < 0) {
                    if (slots.size() < kMaxSlots) {
                        ComPtr<ID3D11Texture2D> t;
                        if (SUCCEEDED(dev->CreateTexture2D(&bd, nullptr, &t))) { ctx->CopyResource(t.Get(), latest.Get()); slots.push_back(t); lastSlot = (int)slots.size() - 1; }
                    }
                    fresh = false;
                }
                if (lastSlot >= 0) while (next <= due) backlog.push_back({next++, lastSlot});
            }
        }
        bool setupOk = setupF.get();
        {
            LARGE_INTEGER q; QueryPerformanceCounter(&q);
            wchar_t b[160];
            swprintf(b, 160, L"first_frame=%.0fms(after press) setup_done=%.0fms(after press, overlapped) buffered=%zu/%zu ",
                     r.startMs, (double)(q.QuadPart - pressQ) * 1000.0 / qf.QuadPart, backlog.size(), slots.size());
            r.latencyLog += b;
        }
        if (!setupOk) return;
        if (!backlog.empty()) next = backlog.front().idx;
        HRESULT whr = S_OK;
        long long pauseBegin = 0;  // worker's own clock: QPC when it first observed Paused
        bool windowClosed = false;
        // A stop that lands while the encoder is still initialising still writes what was captured up to it.
        while ((!stop_ || bi < backlog.size()) && haveFrame && r.message.empty()) {
            if (src->ended()) { windowClosed = true; break; }  // captured window closed: finish and keep the file
            if (fsm_.isPaused()) {
                if (stop_) break;
                // Keep capture warm (drain frames) but write nothing; the media clock does not advance.
                if (!pauseBegin) { LARGE_INTEGER pb; QueryPerformanceCounter(&pb); pauseBegin = pb.QuadPart; }
                bool got = false;
                if (!src->acquire(ctx.Get(), latest.Get(), 20, got)) { r.message = L"Capture failed"; break; }
                mixer.setPaused(true);
                continue;
            }
            if (pauseBegin) {  // shift the pacing origin by exactly the pause the worker saw: no "missed" frames
                LARGE_INTEGER pe; QueryPerformanceCounter(&pe);
                t0.QuadPart += pe.QuadPart - pauseBegin;
                pauseBegin = 0;
                mixer.setOrigin(t0.QuadPart, freq.QuadPart);
                mixer.setPaused(false);
                if (opt_.pauseTones) mixer.addMute(next * dur, next * dur + 3500000, true);  // resume tone
                ++r.pauses;
            }
            if (bi < backlog.size()) {  // encode what was captured while the encoder was initialising
                ctx->CopyResource(latest.Get(), slots[backlog[bi].slot].Get());
                next = backlog[bi].idx; ++bi;
                if (bi == backlog.size()) { slots.clear(); }
            } else {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            long long due = (now.QuadPart - t0.QuadPart) * opt_.fps / freq.QuadPart;
            if (due < next) {
                long long target = t0.QuadPart + next * freq.QuadPart / opt_.fps;
                LONGLONG waitMs = (target - now.QuadPart) * 1000 / freq.QuadPart;
                bool got = false;
                if (!src->acquire(ctx.Get(), latest.Get(), (UINT)(waitMs > 1 ? waitMs - 1 : 0), got)) { r.message = L"Capture failed"; break; }
                if (got) ++capArrivals;
                if (hw) { HRESULT ph = hw->pump(muxOut); if (FAILED(ph)) { r.message = hrMsg(L"Encoder", ph); break; } }
                QueryPerformanceCounter(&now);
                if (now.QuadPart < target && timer) {
                    LARGE_INTEGER rel; rel.QuadPart = -((target - now.QuadPart) * 10000000LL / freq.QuadPart);
                    SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(timer, 50);
                }
                continue;
            }
            if (due > next + 1) { unsigned long long skip = (unsigned long long)(due - next - 1); r.late += skip; r.dropped += skip; next = due - 1; }
            }
            if (cursorByGdi) drawCursor(latest.Get(), origin);
            ID3D11Texture2D* t = ring[next % RING].Get();
            LARGE_INTEGER cs0; QueryPerformanceCounter(&cs0);
            ComPtr<IMFMediaBuffer> buf;
            if (bgraDirect) {
                ctx->CopySubresourceRegion(t, 0, 0, 0, 0, latest.Get(), 0, &box);
                whr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), t, 0, FALSE, &buf);
                if (SUCCEEDED(whr)) { ComPtr<IMF2DBuffer> b2; if (SUCCEEDED(buf.As(&b2))) { DWORD len = 0; b2->GetContiguousLength(&len); buf->SetCurrentLength(len); } }
            } else if (zeroCopy && !cpuFallback) {
                // GPU BGRA->NV12 (+scale) into the ring surface; no CPU readback.
                bool okConv = conv.convert(latest.Get());
                LARGE_INTEGER nowc; QueryPerformanceCounter(&nowc);
                if (okConv) {
                    ctx->CopyResource(lastGood.Get(), conv.nv12.Get());
                    haveGood = true;
                    failSince = 0;
                } else {
                    ++r.convertFailures;
                    if (!failSince) failSince = nowc.QuadPart;
                    if (r.convertFailures % 30 == 0) { GpuConvert fresh; if (fresh.init(dev.Get(), src->w, src->h, W, H)) conv = std::move(fresh); }
                    if (nowc.QuadPart - failSince > 2 * freq.QuadPart) {
                        // ~2 s of continuous failure: switch to CPU readback + CPU NV12 (same sink-writer input type).
                        D3D11_TEXTURE2D_DESC sd = td; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &cpuStaging))) { cpuFallback = true; r.cpuFallback = true; nv12Cpu.resize((size_t)W * H * 3 / 2); }
                        else { r.message = L"GPU color conversion failed and the CPU fallback is unavailable"; break; }
                    }
                }
                if (!cpuFallback) {
                    if (haveGood) {  // never send a frame before the first good conversion
                    ctx->CopyResource(t, lastGood.Get());
                    whr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), t, 0, FALSE, &buf);
                    if (SUCCEEDED(whr)) { ComPtr<IMF2DBuffer> b2; if (SUCCEEDED(buf.As(&b2))) { DWORD len = 0; b2->GetContiguousLength(&len); buf->SetCurrentLength(len); } }
                    }
                }
            }
            if (bgraDirect) {
            } else if ((zeroCopy && cpuFallback) || swNv12) {
                ctx->CopyResource(cpuStaging.Get(), latest.Get());
                D3D11_MAPPED_SUBRESOURCE m{};
                whr = ctx->Map(cpuStaging.Get(), 0, D3D11_MAP_READ, 0, &m);
                if (SUCCEEDED(whr)) {
                    bgraToNv12((const uint8_t*)m.pData, src->w, src->h, m.RowPitch, nv12Cpu.data(), W, H);
                    ctx->Unmap(cpuStaging.Get(), 0);
                    whr = MFCreateMemoryBuffer((DWORD)nv12Cpu.size(), &buf);
                    BYTE* bp = nullptr;
                    if (SUCCEEDED(whr) && SUCCEEDED(whr = buf->Lock(&bp, nullptr, nullptr))) {
                        memcpy(bp, nv12Cpu.data(), nv12Cpu.size());
                        buf->Unlock();
                        buf->SetCurrentLength((DWORD)nv12Cpu.size());
                    }
                }
                if (FAILED(whr)) { r.message = hrMsg(L"CPU fallback", whr); break; }
            } else if (zeroCopy) {
                // (GPU path handled above)
            } else {
                ctx->CopySubresourceRegion(t, 0, 0, 0, 0, latest.Get(), 0, &box);
                ctx->CopyResource(staging.Get(), t);
                D3D11_MAPPED_SUBRESOURCE m{};
                whr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
                if (SUCCEEDED(whr)) {
                    whr = MFCreateMemoryBuffer(W * H * 4, &buf);
                    BYTE* p = nullptr;
                    if (SUCCEEDED(whr) && SUCCEEDED(whr = buf->Lock(&p, nullptr, nullptr))) {
                        MFCopyImage(p, (LONG)(W * 4), (const BYTE*)m.pData, (LONG)m.RowPitch, W * 4, H);
                        buf->Unlock();
                        buf->SetCurrentLength(W * H * 4);
                    }
                    ctx->Unmap(staging.Get(), 0);
                }
            }
            bool counted = buf != nullptr;
            LARGE_INTEGER cs1; QueryPerformanceCounter(&cs1);
            convMs += (double)(cs1.QuadPart - cs0.QuadPart) * 1000.0 / freq.QuadPart; ++convN;
            if (SUCCEEDED(whr) && buf && hw) {
                ComPtr<IMFSample> smp;
                whr = MFCreateSample(&smp);
                if (SUCCEEDED(whr)) { smp->AddBuffer(buf.Get()); smp->SetSampleTime(next * dur); smp->SetSampleDuration(dur); whr = hw->encode(smp.Get(), muxOut); }
                if (whr == S_FALSE) { whr = S_OK; counted = false; ++r.dropped; }  // encoder backpressure: drop, never block
                if (FAILED(muxHr)) whr = muxHr;
                // Runtime stall guard: no encoded output for 2 s while we keep feeding -> end cleanly (no hang).
                LARGE_INTEGER nw; QueryPerformanceCounter(&nw);
                if (hw->outputs != lastOutCount || !lastOutTick) { lastOutCount = hw->outputs; lastOutTick = nw.QuadPart; }
                else if (nw.QuadPart - lastOutTick > 2 * freq.QuadPart) { r.message = L"Hardware encoder stopped responding - recording ended early"; r.encoderStalled = true; break; }
            } else if (SUCCEEDED(whr) && buf) whr = wr.write(buf.Get(), next * dur, dur);
            { LARGE_INTEGER cs2; QueryPerformanceCounter(&cs2); encMs += (double)(cs2.QuadPart - cs1.QuadPart) * 1000.0 / freq.QuadPart; ++encN; }
            if (FAILED(whr)) { r.message = hrMsg(L"WriteSample", whr); break; }
            if (counted) ++r.frames;
            pushAudio((next + 1) * dur, false);
            if (FAILED(audioHr)) { r.message = hrMsg(L"Audio", audioHr); break; }
            ++next;
            bool got = false;
            if (!src->acquire(ctx.Get(), latest.Get(), 0, got)) { r.message = L"Capture failed"; break; }
            if (got) ++capArrivals;
            if (!got) ++r.duplicated;  // reuse last frame
        }
        if (timer) CloseHandle(timer);
        {
            std::wstring ev = src->events();
            if (windowClosed) ev += L"captured window closed: recording ended and saved; ";
            if (!ev.empty()) r.encoderLog += L"window: " + ev + L"\n";
            r.windowClosed = windowClosed;
        }
        fsm_.beginFinish();
        LARGE_INTEGER tEnd; QueryPerformanceCounter(&tEnd);
        {
            double secs = (double)(tEnd.QuadPart - t0.QuadPart) / freq.QuadPart;
            if (secs > 0.1) { r.captureFps = capArrivals / secs; }
            r.convertMsAvg = convN ? convMs / convN : 0; r.encodeMsAvg = encN ? encMs / encN : 0;
        }
        if (hw) {
            if (!hw->drain(muxOut, 1500)) { elog += L"drain: timed out, encoder leaked\n"; hw->leak(); }
            else hw->release();
            hw.reset();
            if (!wr.sw && toFile) r.frames = 0;  // nothing ever came out of the encoder
        }
        {
            double secs = (double)next * dur / 1e7;
            r.encodeFps = secs > 0.1 ? r.frames / secs : 0;
        }
        if (wr.sw || !toFile) {
            pushAudio(next * dur, true);
            while (!audioPending.empty() && wr.sw) { writeChunk(audioPending.front()); audioPending.pop_front(); }
        }
        if (replay && ringAttached) replay->detach();
        r.encoderLog += L"audio stats: " + mixer.stats() + L"\n";
        mixer.stop();

        {   // diagnostics: effective path
            wchar_t b[512];
            swprintf(b, 512, L"adapter=%ls; output=%ls; encoder=%ls; zero_copy=%ls%ls; cpu_fallback=%ls; size=%ux%u@%d; dropped=%llu; duplicated=%llu; convert_failures=%llu",
                     adapterName.c_str(), output ? L"local" : L"cross-adapter", r.encoderUsed.c_str(), zeroCopy ? L"yes" : L"no", bgraDirect ? L" (bgra)" : L"", r.cpuFallback ? L"yes" : L"no", W, H, opt_.fps,
                     r.dropped, r.duplicated, r.convertFailures);
            r.pathLog = b;
            swprintf(b, 512, L"; capture_fps=%.1f; encode_fps=%.1f; convert_ms=%.2f; encode_ms=%.2f; audio=%ls; audio_underrun_frames=%llu",
                     r.captureFps, r.encodeFps, r.convertMsAvg, r.encodeMsAvg, r.hasAudio ? L"aac" : L"none", mixer.underrunFrames);
            r.pathLog += b;
            swprintf(b, 512, L"; start_ms=%.0f; latency=[%ls]", r.startMs, r.latencyLog.c_str());
            r.pathLog += b;
        }

        // ---- capture teardown with budget; on timeout the helper owns the Source (leak > UAF) ----
        {
            Source* raw = src.release();
            if (raw) {
                auto closed = std::make_shared<std::atomic<bool>>(false);
                // On timeout the helper owns `raw` forever (prefer leak over UAF if Close hangs).
                std::thread closer([raw, closed] { delete raw; closed->store(true); });
                for (int i = 0; i < 40 && !closed->load(); ++i) Sleep(50);  // ~2 s
                if (closed->load()) closer.join();
                else closer.detach();  // capture objects only (not MF): leak them, don't poison the encoder
            }
        }

        if (!toFile) {  // replay buffer session: nothing to finalize
            resetWriter();
            r.ok = r.frames > 0 && r.message.empty();
            if (r.message.empty() && !r.frames) r.message = L"No frames encoded";
            return;
        }
        // ---- Finalize: budget = finalizeTimeoutMs/2 (default 4 s); remux gets the other half ----
        HRESULT fh = E_FAIL;
        const bool fragOk = wr.fragmentedOk;
        r.fragmented = fragOk;
        if (r.frames && wr.sw) {
            auto st = std::make_shared<FinState>();
            ComPtr<IMFSinkWriter> swRef = wr.sw;     // by value: helper holds its own refs
            ComPtr<IMFDXGIDeviceManager> mgrRef = wr.mgr;
            std::wstring tmpCopy = tmp;
            DWORD hang = g_testHangFinalizeMs.load();
            std::thread fin([swRef, mgrRef, st, tmpCopy, hang]() mutable {
                if (hang) Sleep(hang);  // test hook: simulate a stuck Finalize
                st->hr = swRef->Finalize();
                int expect = FinState::Running;
                if (!st->state.compare_exchange_strong(expect, FinState::Done)) {
                    // Main abandoned us: do not Release the writer (prefer leak over UAF / MF re-entry).
                    swRef.Detach();
                    mgrRef.Detach();
                    (void)tmpCopy;  // the file is kept: fragmented MP4 is playable up to its last fragment
                    g_abandonLive.fetch_sub(1);
                }
            });
            const DWORD budget = finalizeBudgetMs(opt_.finalizeTimeoutMs);
            DWORD waited = 0;
            while (st->state.load() == FinState::Running && waited < budget) { Sleep(50); waited += 50; }
            g_abandonLive.fetch_add(1);
            int expect = FinState::Running;
            if (st->state.compare_exchange_strong(expect, FinState::Abandoned)) {
                // Abandon: do NOT Release the writer/device manager while Finalize may be in flight.
                wr.sw.Detach();   // intentional process-lifetime leak (prefer leak over UAF)
                wr.mgr.Detach();
                fin.detach();
                abandon_.abandon();
                abandonedMf = true;
                r.timedOut = true;
                r.abandoned = true;
                if (fragOk && fileSizeOf(tmp) > 0) { r.file = tmp; r.message = L"Finalize timed out - kept the file (the last second may be missing)"; }
                else { r.message = L"Finalize timed out - nothing was written"; DeleteFileW(tmp.c_str()); }
            } else {
                g_abandonLive.fetch_sub(1);
                fin.join();
                fh = st->hr.load();
                resetWriter();
            }
        } else {
            resetWriter();
            if (!r.frames) { DeleteFileW(tmp.c_str()); if (r.message.empty()) r.message = L"No frames recorded"; }
        }

        if (!r.timedOut) {
            if (FAILED(fh) && r.frames) {
                if (r.message.empty()) r.message = hrMsg(L"Finalize", fh);
                if (fragOk && fileSizeOf(tmp) > 0) { r.file = tmp; r.message += L" - kept the file"; }  // still playable
                else DeleteFileW(tmp.c_str());
            } else if (SUCCEEDED(fh) && r.frames) {
                // Saved. Anything else (MKV, track names) is a separate stream copy the caller runs afterwards, so a
                // long recording can never be lost to a timeout here.
                r.file = tmp;
                r.post.file = tmp;
                if (wr.tracks == 3 && r.hasAudio) r.post.trackNames = {L"Mix", L"System audio", L"Microphone"};
                if (opt_.container != Container::MP4 && !opt_.autoRemux) {  // MKV / MOV: stream copy afterwards
                    size_t dot = opt_.outputPath.find_last_of(L'.');
                    r.post.mkvPath = (dot == std::wstring::npos ? opt_.outputPath : opt_.outputPath.substr(0, dot)) + (opt_.container == Container::MOV ? L".mov" : L".mkv");
                }
                if (opt_.container != Container::MP4 && opt_.autoRemux) r.encoderLog += L"auto-remux on: recorded straight to MP4 (no remux needed)\n";
            }
        }
        r.ok = r.frames > 0 && !r.timedOut && r.message.empty();
    }(); } catch (const std::bad_alloc&) {  // out of memory: stop cleanly, keep what reached the disk
        r.ok = false; r.message = L"Out of memory - recording stopped (the file keeps what was written)";
        r.encoderLog += L"worker: std::bad_alloc caught\n";
    }

    // Skip MF/COM teardown if anything was abandoned (a detached Finalize/Close may still be inside MF).
    timeEndPeriod(1);
    if (mmcss) {
        typedef BOOL(WINAPI * AvRev)(HANDLE);
        if (auto f = (AvRev)GetProcAddress(GetModuleHandleW(L"avrt.dll"), "AvRevertMmThreadCharacteristics")) f(mmcss);
    }
    if (!abandonedMf && !abandon_.poisoned.load()) {
        if (mfStarted) MFShutdown();
        if (SUCCEEDED(hrCo)) CoUninitialize();
    }
    if (DWORD late = g_testLatePublishMs.load()) Sleep(late);  // test hook: abort() publishes first
    const bool won = claim_.claim(gen, [&] {
        result_ = r;
        fsm_.toIdle();
        started_ = true;
        if (doneEvent_) SetEvent(doneEvent_);
        if (opt_.notify) PostMessageW(opt_.notify, opt_.warmupOnly ? WM_APP_WARMUP_DONE : opt_.doneMsg ? opt_.doneMsg : WM_APP_RECORDER_DONE, 0, 0);
    });
    // abort() already published for this session. Never delete what we wrote (fragmented MP4 = playable);
    // like abort(), only an empty file is removed.
    if (!won) {
        if (!r.file.empty() && fileSizeOf(r.file) == 0) DeleteFileW(r.file.c_str());
        g_testLateLost.fetch_add(1);
    }
}


// ======================= remux (stream copy) =======================
namespace {
bool ffmpegCopy(const std::wstring& ffmpeg, const std::wstring& in, const std::wstring& out, DWORD timeoutMs,
                const std::vector<std::wstring>& names, std::wstring& err) {
    // The absolute path goes in lpApplicationName so CreateProcess can't resolve a different ffmpeg.exe.
    std::wstring cmd = L"\"" + ffmpeg + L"\" -hide_banner -loglevel error -y -i \"" + in + L"\" -map 0 -c copy";
    for (size_t k = 0; k < names.size(); ++k) cmd += L" -metadata:s:a:" + std::to_wstring(k) + L" title=\"" + names[k] + L"\" -metadata:s:a:" + std::to_wstring(k) + L" handler_name=\"" + names[k] + L"\"";
    size_t dot = out.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : out.substr(dot);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    if (ext == L".mp4" || ext == L".mov") cmd += L" -movflags +faststart";
    if (ext == L".mp4") cmd += L" -f mp4";
    cmd += L" \"" + out + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(ffmpeg.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) { err = L"ffmpeg did not start"; return false; }
    {   // ffmpeg dies with us (no orphan finishing a copy nobody will rename)
        static HANDLE job = [] {
            HANDLE j = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (j) SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof li);
            return j;
        }();
        if (job) AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
    }
    DWORD wait = timeoutMs ? timeoutMs : 8000;
    bool timedOut = false;
    if (WaitForSingleObject(pi.hProcess, wait) == WAIT_TIMEOUT) {
        timedOut = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    bool ok = code == 0 && GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!ok) { err = timedOut ? L"ffmpeg timed out" : L"ffmpeg failed (exit " + std::to_wstring(code) + L")"; DeleteFileW(out.c_str()); }
    return ok;
}

// Media Foundation stream copy: the source reader hands out compressed samples (no output types set), the sink
// writer muxes them unchanged. Works for the H.264/HEVC/AV1 + AAC streams we write.
bool mfCopy(const std::wstring& in, const std::wstring& out, std::wstring& err) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    bool ok = false;
    [&] {
        ComPtr<IMFSourceReader> rd;
        HRESULT hr = MFCreateSourceReaderFromURL(in.c_str(), nullptr, &rd);
        if (FAILED(hr)) { err = hrMsg(L"open source", hr); return; }
        ComPtr<IMFAttributes> a;
        MFCreateAttributes(&a, 2);
        a->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        a->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        ComPtr<IMFSinkWriter> sw;
        hr = MFCreateSinkWriterFromURL(out.c_str(), nullptr, a.Get(), &sw);
        if (FAILED(hr)) { err = hrMsg(L"create MP4", hr); return; }
        std::vector<std::pair<DWORD, DWORD>> map;  // reader stream -> writer stream
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> t;
            hr = rd->GetNativeMediaType(i, 0, &t);
            if (hr == MF_E_INVALIDSTREAMNUMBER) break;
            if (FAILED(hr)) { err = hrMsg(L"native type", hr); return; }
            GUID major{};
            t->GetGUID(MF_MT_MAJOR_TYPE, &major);
            if (major != MFMediaType_Video && major != MFMediaType_Audio) { rd->SetStreamSelection(i, FALSE); continue; }
            rd->SetStreamSelection(i, TRUE);
            DWORD w = 0;
            hr = sw->AddStream(t.Get(), &w);
            if (SUCCEEDED(hr)) hr = sw->SetInputMediaType(w, t.Get(), nullptr);
            if (FAILED(hr)) { err = hrMsg(L"stream copy setup", hr); return; }
            map.push_back({i, w});
        }
        if (map.empty()) { err = L"no audio/video streams"; return; }
        hr = sw->BeginWriting();
        if (FAILED(hr)) { err = hrMsg(L"BeginWriting", hr); return; }
        unsigned long long n = 0;
        std::vector<DWORD> ended;
        for (;;) {
            DWORD si = 0, flags = 0; LONGLONG ts = 0;
            ComPtr<IMFSample> smp;
            hr = rd->ReadSample(MF_SOURCE_READER_ANY_STREAM, 0, &si, &flags, &ts, &smp);
            if (FAILED(hr)) { err = hrMsg(L"ReadSample", hr); return; }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                bool all = true;  // keep reading until every selected stream ended
                ended.push_back(si);
                for (auto& m2 : map) if (std::find(ended.begin(), ended.end(), m2.first) == ended.end()) all = false;
                if (all) break;
                if (!smp) continue;
            }
            if (!smp) continue;
            for (auto& m2 : map) if (m2.first == si) { hr = sw->WriteSample(m2.second, smp.Get()); break; }
            if (FAILED(hr)) { err = hrMsg(L"WriteSample", hr); return; }
            ++n;
        }
        hr = sw->Finalize();
        if (FAILED(hr)) { err = hrMsg(L"Finalize", hr); return; }
        ok = n > 0;
        if (!ok) err = L"no samples";
    }();
    if (mf) MFShutdown();
    if (SUCCEEDED(hrCo)) CoUninitialize();
    if (!ok) DeleteFileW(out.c_str());
    return ok;
}
}  // namespace

static bool remuxFileDirect(const std::wstring& in, const std::wstring& out, std::wstring& err, const std::wstring& engine, DWORD timeoutMs,
                            const std::vector<std::wstring>& trackNames);
// Writes to "<name>.part<ext>" and renames over `out` only when complete: a crash/kill never leaves a half file at `out`.
bool remuxFile(const std::wstring& in, const std::wstring& out, std::wstring& err, const std::wstring& engine, DWORD timeoutMs,
               const std::vector<std::wstring>& trackNames) {
    size_t dot = out.find_last_of(L'.'), sl = out.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (sl != std::wstring::npos && dot < sl)) dot = out.size();
    const std::wstring part = out.substr(0, dot) + L".part" + out.substr(dot);
    DeleteFileW(part.c_str());
    bool ok = remuxFileDirect(in, part, err, engine, timeoutMs, trackNames) && fileSizeOf(part) > 0;
    if (ok && !MoveFileExW(part.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = L"rename failed (" + std::to_wstring(GetLastError()) + L")"; ok = false;
    }
    if (!ok) DeleteFileW(part.c_str());
    return ok;
}
static bool remuxFileDirect(const std::wstring& in, const std::wstring& out, std::wstring& err, const std::wstring& engine, DWORD timeoutMs,
                            const std::vector<std::wstring>& trackNames) {
    if (GetFileAttributesW(in.c_str()) == INVALID_FILE_ATTRIBUTES) { err = L"source file missing"; return false; }
    size_t odot = out.find_last_of(L'.');
    std::wstring oext = odot == std::wstring::npos ? L"" : out.substr(odot);
    for (auto& c : oext) c = (wchar_t)towlower(c);
    // Default: Media Foundation (built into Windows, no ffmpeg needed) for MP4 output; ffmpeg is the fallback.
    if (engine.empty() && oext == L".mp4" && trackNames.empty()) {
        std::wstring e1;
        if (mfCopy(in, out, e1)) { err.clear(); return true; }
        std::wstring ff0 = findFfmpeg();
        if (ff0.empty()) { err = e1; return false; }
        if (ffmpegCopy(ff0, in, out, timeoutMs, trackNames, err)) return true;
        err = L"MF: " + e1 + L"; " + err;
        return false;
    }
    std::wstring ff = engine == L"mf" ? std::wstring() : findFfmpeg();
    if (!ff.empty()) {
        if (ffmpegCopy(ff, in, out, timeoutMs, trackNames, err)) return true;
        if (engine == L"ffmpeg") return false;
    } else if (engine == L"ffmpeg") { err = L"ffmpeg.exe not found"; return false; }
    size_t dot = out.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : out.substr(dot);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    if (ext != L".mp4") { if (err.empty()) err = L"MKV output needs ffmpeg.exe"; return false; }
    std::wstring e2;
    if (mfCopy(in, out, e2)) { err.clear(); return true; }
    err = err.empty() ? e2 : err + L"; MF: " + e2;
    return false;
}

bool postProcessRecording(const PostJob& j, std::wstring& finalFile, std::wstring& log) {
    finalFile = j.file;
    if (!j.needed()) return true;
    if (DWORD slow = g_testSlowPostMs.load()) { log += L"post: test delay " + std::to_wstring(slow) + L" ms\n"; Sleep(slow); }
    const unsigned long long sz = fileSizeOf(j.file);
    if (!sz) { log += L"post: source missing\n"; return false; }
    std::wstring ff = findFfmpeg();
    if (ff.empty()) {
        if (!j.mkvPath.empty()) log += L"post: MKV needs ffmpeg.exe - kept the MP4\n";
        if (!j.trackNames.empty()) log += L"post: track names need ffmpeg.exe - tracks are unnamed (order: mix, system, mic)\n";
        return false;
    }
    // Disk-bound stream copy: 60 s + 1 s per 20 MB. On timeout only the partial copy is removed.
    const DWORD budget = (DWORD)std::min<unsigned long long>(60000ull + sz / (20ull << 20) * 1000ull, 3600000ull);
    const bool mkv = !j.mkvPath.empty();
    size_t dot = mkv ? j.mkvPath.find_last_of(L'.') : j.file.find_last_of(L'.');
    const std::wstring& dstp = mkv ? j.mkvPath : j.file;
    std::wstring part = dstp.substr(0, dot) + L".part" + dstp.substr(dot);
    std::wstring err;
    bool ok = ffmpegCopy(ff, j.file, part, budget, j.trackNames, err) && fileSizeOf(part) > 0;
    if (ok) {
        const std::wstring& dst = mkv ? j.mkvPath : j.file;
        ok = MoveFileExW(part.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        if (ok && mkv) DeleteFileW(j.file.c_str());
        if (ok) finalFile = dst;
        else err = L"rename failed (" + std::to_wstring(GetLastError()) + L")";
    }
    if (!ok) { DeleteFileW(part.c_str()); log += L"post: " + std::wstring(mkv ? L"MKV conversion" : L"track naming") + L" failed (" + err + L") - kept " + j.file + L"\n"; }
    else log += std::wstring(L"post: ") + (mkv ? L"converted to MKV" : L"audio tracks named (mix / system / mic)") + L"\n";
    return ok;
}

// ======================= instant replay ring =======================
struct ReplayRing::Impl {
    mutable std::mutex m;
    int seconds = 30;
    ComPtr<IMFMediaType> vtype;
    GUID sub{}; UINT32 w = 0, h = 0; std::vector<BYTE> hdr;
    int fps = 30, channels = 1, tracks = 1; bool audio = false;
    struct V { ComPtr<IMFSample> s; LONGLONG t = 0, d = 0; bool key = false; size_t bytes = 0; };
    std::deque<V> v;
    // PCM chunks are immutable once queued: save() takes refs under the lock and copies nothing there.
    struct A { LONGLONG t = 0; std::shared_ptr<const std::vector<short>> pcm[3]; size_t bytes = 0; };
    std::deque<A> a;
    LONGLONG offset = 0, lastEnd = 0;
    bool attached = false;
    size_t bytes = 0;
    size_t maxBytes = (size_t)2 << 30;  // hard RAM cap (UI sets min(25 % of RAM, 2 GB)); oldest GOPs go first
    bool capped = false;                 // the cap (not the length) last trimmed the ring (sticky)
    // Key frame closest to (end - secs): with 1 s GOPs the saved length is within +-0.5 s of what was asked.
    LONGLONG startFor(int secs) const {
        LONGLONG want = lastEnd - (LONGLONG)secs * 10000000LL;
        LONGLONG best = -1, bestD = 0;
        for (auto& x : v) {
            if (!x.key) continue;
            LONGLONG d = x.t > want ? x.t - want : want - x.t;
            if (best < 0 || d < bestD) { best = x.t; bestD = d; }
            if (x.t > want) break;
        }
        return best;
    }
    void dropFirstGop() {
        size_t k = 1;
        while (k < v.size() && !v[k].key) ++k;
        for (size_t i = 0; i < k && !v.empty(); ++i) { bytes -= v.front().bytes; v.pop_front(); }
    }
    // Out of memory: halve the cap (shorter replay instead of a crash); the UI reads cappedByMemory().
    void oom() {
        maxBytes = std::max<size_t>((size_t)1 << 20, bytes / 2);
        trim();
        capped = true;
    }
    void trim() {
        // keep one GOP more than asked so the nearest key frame can be the one before (end - seconds)
        const LONGLONG keep = (LONGLONG)seconds * 10000000LL;
        for (;;) {
            size_t k = 1;
            while (k < v.size() && !v[k].key) ++k;
            if (k >= v.size() || v[k].t > lastEnd - keep) break;
            dropFirstGop();
            capped = false;  // the length (not the cap) is trimming now
        }
        while (bytes > maxBytes && v.size() > 1) {
            size_t k = 1;
            while (k < v.size() && !v[k].key) ++k;
            if (k >= v.size()) break;  // never drop the only GOP
            dropFirstGop();
            capped = true;
        }
        LONGLONG first = v.empty() ? lastEnd : v.front().t;
        while (!a.empty() && a.front().t + 2000000 < first) { bytes -= a.front().bytes; a.pop_front(); }
    }
};

ReplayRing::ReplayRing(int seconds) : p_(new Impl) { p_->seconds = seconds < 5 ? 5 : seconds > 900 ? 900 : seconds; }
// Bytes a replay of `seconds` needs at this bitrate: video + PCM (48 kHz s16 x channels x tracks) + 10 % headroom.
unsigned long long ReplayRing::estimateBytes(int seconds, unsigned videoBitrate, int channels, int tracks, bool audio) {
    double perSec = videoBitrate / 8.0 + (audio ? 48000.0 * 2 * channels * tracks : 0.0);
    return (unsigned long long)(perSec * seconds * 1.1);
}
size_t ReplayRing::defaultMaxBytes() {
    MEMORYSTATUSEX ms{sizeof ms};
    unsigned long long quarter = GlobalMemoryStatusEx(&ms) ? ms.ullTotalPhys / 4 : (1ull << 30);
    unsigned long long cap = std::min<unsigned long long>(quarter, 2ull << 30);
    return (size_t)std::min<unsigned long long>(cap, (unsigned long long)SIZE_MAX / 2);
}
ReplayRing::~ReplayRing() = default;
void ReplayRing::setSeconds(int s) { std::lock_guard<std::mutex> g(p_->m); p_->seconds = s < 5 ? 5 : s > 900 ? 900 : s; p_->trim(); }
int ReplayRing::seconds() const { std::lock_guard<std::mutex> g(p_->m); return p_->seconds; }
size_t ReplayRing::bytes() const { std::lock_guard<std::mutex> g(p_->m); return p_->bytes; }
void ReplayRing::setMaxBytes(size_t b) { std::lock_guard<std::mutex> g(p_->m); p_->maxBytes = b < ((size_t)1 << 20) ? ((size_t)1 << 20) : b; p_->trim(); }
size_t ReplayRing::maxBytes() const { std::lock_guard<std::mutex> g(p_->m); return p_->maxBytes; }
bool ReplayRing::cappedByMemory() const { std::lock_guard<std::mutex> g(p_->m); return p_->capped; }
double ReplayRing::bufferedSeconds() const {
    std::lock_guard<std::mutex> g(p_->m);
    LONGLONG st = p_->startFor(p_->seconds);
    return st < 0 ? 0.0 : (double)(p_->lastEnd - st) / 1e7;
}

void ReplayRing::attach(IMFMediaType* vt, int fps, int channels, int tracks, bool audio) {
    std::lock_guard<std::mutex> g(p_->m);
    GUID sub{}; UINT32 w = 0, h = 0; std::vector<BYTE> hdr;
    vt->GetGUID(MF_MT_SUBTYPE, &sub);
    MFGetAttributeSize(vt, MF_MT_FRAME_SIZE, &w, &h);
    UINT32 sz = 0;
    if (SUCCEEDED(vt->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sz)) && sz) { hdr.resize(sz); vt->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, hdr.data(), sz, nullptr); }
    const bool same = p_->vtype && sub == p_->sub && w == p_->w && h == p_->h && hdr == p_->hdr && fps == p_->fps &&
                      channels == p_->channels && tracks == p_->tracks && audio == p_->audio;
    if (!same) {  // different encoder configuration: older packets could not be muxed with this one
        p_->v.clear(); p_->a.clear(); p_->bytes = 0; p_->lastEnd = 0;
        ComPtr<IMFMediaType> c;
        MFCreateMediaType(&c);
        vt->CopyAllItems(c.Get());
        p_->vtype = c; p_->sub = sub; p_->w = w; p_->h = h; p_->hdr = hdr; p_->fps = fps; p_->channels = channels; p_->tracks = tracks; p_->audio = audio;
    }
    p_->offset = p_->lastEnd;  // continue the ring's timeline (session time starts at 0 again)
    p_->attached = true;
}
void ReplayRing::detach() { std::lock_guard<std::mutex> g(p_->m); p_->attached = false; }

void ReplayRing::addVideo(IMFSample* s) {
    LONGLONG t = 0, d = 0;
    if (FAILED(s->GetSampleTime(&t))) return;
    if (FAILED(s->GetSampleDuration(&d)) || d <= 0) d = 10000000LL / 30;
    UINT32 clean = 0;
    if (FAILED(s->GetUINT32(MFSampleExtension_CleanPoint, &clean))) clean = 0;
    // Own copy of the bytes: encoder MFTs may recycle their output samples from a small pool.
    ComPtr<IMFMediaBuffer> src, dst;
    if (FAILED(s->ConvertToContiguousBuffer(&src))) return;
    BYTE* p = nullptr; DWORD len = 0;
    if (FAILED(src->Lock(&p, nullptr, &len))) return;
    HRESULT hr = MFCreateMemoryBuffer(len ? len : 1, &dst);
    BYTE* q = nullptr;
    if (SUCCEEDED(hr) && SUCCEEDED(hr = dst->Lock(&q, nullptr, nullptr))) { memcpy(q, p, len); dst->Unlock(); dst->SetCurrentLength(len); }
    src->Unlock();
    if (FAILED(hr)) return;
    ComPtr<IMFSample> c;
    if (FAILED(MFCreateSample(&c))) return;
    c->AddBuffer(dst.Get());
    std::lock_guard<std::mutex> g(p_->m);
    if (!p_->attached) return;
    try {
    Impl::V x; x.s = c; x.t = t + p_->offset; x.d = d; x.key = clean != 0; x.bytes = len;
    if (p_->v.empty() && !x.key) return;  // the ring always starts on a key frame
    p_->v.push_back(x);
    p_->bytes += len;
    if (x.t + d > p_->lastEnd) p_->lastEnd = x.t + d;
    p_->trim();
    } catch (const std::bad_alloc&) { p_->oom(); }
}

void ReplayRing::addAudio(long long t, const std::vector<short>* const pcm[3]) {
    Impl::A c;  // copy the PCM before taking the lock
    try {
        for (int k = 0; k < 3; ++k) if (pcm[k] && !pcm[k]->empty()) { c.pcm[k] = std::make_shared<const std::vector<short>>(*pcm[k]); c.bytes += pcm[k]->size() * sizeof(short); }
    } catch (const std::bad_alloc&) { std::lock_guard<std::mutex> g(p_->m); p_->oom(); return; }
    std::lock_guard<std::mutex> g(p_->m);
    if (!p_->attached || !p_->audio || p_->v.empty()) return;
    c.t = t + p_->offset;
    for (int k = p_->tracks; k < 3; ++k) { if (c.pcm[k]) c.bytes -= c.pcm[k]->size() * sizeof(short); c.pcm[k].reset(); }
    const size_t nb = c.bytes;
    try { p_->a.push_back(std::move(c)); p_->bytes += nb; } catch (const std::bad_alloc&) { p_->oom(); }
}

bool ReplayRing::save(const std::wstring& path, double& savedSeconds, std::wstring& err) {
    try { return saveImpl(path, savedSeconds, err); }
    catch (const std::bad_alloc&) { savedSeconds = 0; err = L"Out of memory while saving the replay"; DeleteFileW(path.c_str()); return false; }
}
bool ReplayRing::saveImpl(const std::wstring& path, double& savedSeconds, std::wstring& err) {
    savedSeconds = 0;
    std::vector<Impl::V> vs; std::vector<Impl::A> as;
    ComPtr<IMFMediaType> vt; int fps = 30, channels = 1, tracks = 1; bool audio = false;
    LONGLONG start = 0, end = 0;
    {
        std::lock_guard<std::mutex> g(p_->m);
        start = p_->startFor(p_->seconds);
        if (start < 0 || !p_->vtype) { err = L"Replay buffer is empty"; return false; }
        end = p_->lastEnd;
        // refs only (ComPtr / shared_ptr): the copy of the media itself happens in the writer, outside the lock
        size_t nv = 0, na = 0;
        for (auto& x : p_->v) if (x.t >= start) ++nv;
        for (auto& x : p_->a) if (x.t + 2000000 >= start) ++na;
        vs.reserve(nv); as.reserve(na);
        for (auto& x : p_->v) if (x.t >= start) vs.push_back(x);
        for (auto& x : p_->a) if (x.t + 2000000 >= start) as.push_back(x);
        vt = p_->vtype; fps = p_->fps; channels = p_->channels; tracks = p_->tracks; audio = p_->audio;
    }
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    bool ok = false;
    [&] {
        Writer w;
        w.wantAudio = audio && !as.empty();
        w.tracks = tracks; w.achannels = (UINT32)channels;
        if (!w.openPassthrough(path, vt.Get(), err)) return;
        size_t ai = 0;
        const LONGLONG perFrame = 10000000LL / AudioMixer::kRate;
        auto writeAudioUpTo = [&](LONGLONG until) -> HRESULT {
            for (; ai < as.size() && as[ai].t <= until; ++ai) {
                const Impl::A& c = as[ai];
                for (int k = 0; k < tracks; ++k) {
                    if (!c.pcm[k]) continue;
                    const std::vector<short>& pc = *c.pcm[k];
                    if (pc.empty()) continue;
                    LONGLONG t = c.t - start;
                    size_t skip = 0;
                    if (t < 0) { skip = (size_t)((-t) / perFrame) * (size_t)channels; t = 0; }
                    if (skip >= pc.size()) continue;
                    std::vector<short> part(pc.begin() + (ptrdiff_t)skip, pc.end());
                    HRESULT h = w.writeAudio(part, t, k);
                    if (FAILED(h)) return h;
                }
            }
            return S_OK;
        };
        for (auto& x : vs) {
            HRESULT h = w.wantAudio ? writeAudioUpTo(x.t) : S_OK;
            ComPtr<IMFSample> c; ComPtr<IMFMediaBuffer> b;
            if (SUCCEEDED(h)) h = MFCreateSample(&c);
            if (SUCCEEDED(h)) h = x.s->GetBufferByIndex(0, &b);
            if (SUCCEEDED(h)) h = c->AddBuffer(b.Get());
            if (SUCCEEDED(h)) { c->SetSampleTime(x.t - start); c->SetSampleDuration(x.d); if (x.key) c->SetUINT32(MFSampleExtension_CleanPoint, TRUE); h = w.sw->WriteSample(w.stream, c.Get()); }
            if (FAILED(h)) { err = hrMsg(L"WriteSample", h); return; }
        }
        if (w.wantAudio) writeAudioUpTo(end);
        HRESULT h = w.sw->Finalize();
        if (FAILED(h)) { err = hrMsg(L"Finalize", h); return; }
        ok = true;
    }();
    (void)fps;
    if (mf) MFShutdown();
    if (SUCCEEDED(hrCo)) CoUninitialize();
    if (ok) savedSeconds = (double)(end - start) / 1e7;
    else DeleteFileW(path.c_str());
    return ok;
}

}  // namespace cr
