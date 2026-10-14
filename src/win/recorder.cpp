// Capture (WGC / DXGI Desktop Duplication / synthetic) -> D3D11 texture -> Media Foundation H.264 encoder -> MP4.
// Frames stay on the GPU on the hardware path: the captured texture is copied into a GDI-compatible
// BGRA texture (cursor is drawn with GDI), then wrapped with MFCreateDXGISurfaceBuffer. The sink writer's
// GPU video processor converts BGRA->NV12 and feeds the vendor's hardware MFT (NVENC/AMF/QuickSync).
#include "recorder.h"
#include <climits>
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
    if (scale == OutputScale::P1080) maxH = 1080;
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

    // nv12In=true: samples are DXGI NV12 surfaces (zero-copy HW path). Otherwise RGB32 (CPU or MF color convert).
    bool open(const std::wstring& path, ID3D11Device* dev, UINT w, UINT h, const RecordOptions& o, bool hardware, bool nv12In, std::wstring& err) {
        ComPtr<IMFAttributes> a;
        MFCreateAttributes(&a, 4);
        a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE);
        a->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        a->SetUINT32(MF_LOW_LATENCY, TRUE);
        a->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        gpu = hardware;
        if (hardware) {
            UINT token = 0;
            HRESULT hr = MFCreateDXGIDeviceManager(&token, &mgr);
            if (SUCCEEDED(hr)) hr = mgr->ResetDevice(dev, token);
            if (FAILED(hr)) { err = hrMsg(L"MFCreateDXGIDeviceManager", hr); return false; }
            a->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, mgr.Get());
        }
        HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, a.Get(), &sw);
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
    DWORD astream = 0;
    bool audio = false;
    UINT32 achannels = 2;
    // AAC 48 kHz stereo track encoded by the sink writer's AAC MFT from s16 PCM. Call before BeginWriting.
    bool addAudio(std::wstring& err) {
        ComPtr<IMFMediaType> o, i;
        MFCreateMediaType(&o);
        o->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        o->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        o->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        o->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioMixer::kRate);
        o->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, achannels);
        o->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, achannels == 1 ? 16000 : 24000);  // 128 / 192 kbps
        o->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        HRESULT hr = sw->AddStream(o.Get(), &astream);
        MFCreateMediaType(&i);
        i->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        i->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        i->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        i->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioMixer::kRate);
        i->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, achannels);
        i->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * achannels);
        i->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AudioMixer::kRate * 2 * achannels);
        if (SUCCEEDED(hr)) hr = sw->SetInputMediaType(astream, i.Get(), nullptr);
        if (FAILED(hr)) { err = hrMsg(L"Audio stream (AAC)", hr); return false; }
        audio = true;
        return true;
    }
    HRESULT writeAudio(const std::vector<short>& pcm, LONGLONG t) {
        if (!audio || pcm.empty()) return S_OK;
        DWORD bytes = (DWORD)(pcm.size() * sizeof(short));
        ComPtr<IMFMediaBuffer> b; ComPtr<IMFSample> s;
        HRESULT hr = MFCreateMemoryBuffer(bytes, &b);
        BYTE* p = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(hr = b->Lock(&p, nullptr, nullptr))) { memcpy(p, pcm.data(), bytes); b->Unlock(); b->SetCurrentLength(bytes); }
        if (SUCCEEDED(hr)) hr = MFCreateSample(&s);
        if (SUCCEEDED(hr)) { s->AddBuffer(b.Get()); s->SetSampleTime(t); s->SetSampleDuration((LONGLONG)(pcm.size() / achannels) * 10000000LL / AudioMixer::kRate); }
        if (SUCCEEDED(hr)) hr = sw->WriteSample(astream, s.Get());
        return hr;
    }
    bool wantAudio = false;
    // Mux-only sink writer for samples already encoded by our own hardware MFT (no transforms inside MF).
    bool openPassthrough(const std::wstring& path, IMFMediaType* encOut, std::wstring& err) {
        ComPtr<IMFAttributes> a;
        MFCreateAttributes(&a, 3);
        a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
        a->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        a->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        gpu = true;
        HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, a.Get(), &sw);
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

bool remux(const std::wstring& ffmpeg, const std::wstring& in, const std::wstring& out, DWORD timeoutMs) {
    // The absolute path goes in lpApplicationName so CreateProcess can't resolve a different ffmpeg.exe.
    std::wstring cmd = L"\"" + ffmpeg + L"\" -hide_banner -loglevel error -y -i \"" + in + L"\" -c copy \"" + out + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(ffmpeg.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    DWORD wait = timeoutMs ? timeoutMs : 8000;
    if (WaitForSingleObject(pi.hProcess, wait) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0 && GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES;
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
    swprintf(b, 128, L"%08lX:%08lX/%d/%ls/%d", (unsigned long)ad.AdapterLuid.HighPart, (unsigned long)ad.AdapterLuid.LowPart, e.index, e.vendor.c_str(), e.argb ? 1 : 0);
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
    sessionTmp_ = opt_.container == Container::MP4 ? opt_.outputPath : opt_.outputPath + L".part.mp4";
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
    // Only the first of {abort, worker} publishes; the winner here also deletes half-written files.
    const std::wstring tmpP = sessionTmp_, outP = sessionOut_;
    claim_.claim(sessionGen_, [&] {
        if (!tmpP.empty()) DeleteFileW(tmpP.c_str());
        if (!outP.empty() && outP != tmpP) DeleteFileW(outP.c_str());
        RecordResult pub;
        pub.timedOut = true;
        pub.abandoned = abandon_.poisoned.load();
        pub.message = L"Recording cancelled";
        result_ = std::move(pub);
        fsm_.toIdle();
        started_ = true;
        if (doneEvent_) SetEvent(doneEvent_);
        if (opt_.notify) PostMessageW(opt_.notify, opt_.warmupOnly ? WM_APP_WARMUP_DONE : WM_APP_RECORDER_DONE, 0, 0);
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

    [&] {
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
        std::deque<std::pair<long long, std::vector<short>>> audioPending;  // until the lazy writer opens
        HRESULT audioHr = S_OK;
        auto pushAudio = [&](long long mediaTime, bool final) {
            if (!wr.wantAudio) return;
            mixer.micMuted = micMuted_.load();
            std::vector<short> pcm; long long at = 0;
            mixer.produce(mediaTime, final, pcm, at);
            if (pcm.empty()) return;
            if (!wr.sw) { audioPending.emplace_back(at, std::move(pcm)); return; }
            while (!audioPending.empty()) { wr.writeAudio(audioPending.front().second, audioPending.front().first); audioPending.pop_front(); }
            HRESULT ah = wr.writeAudio(pcm, at);
            if (FAILED(ah)) audioHr = ah;
        };

        // ---- encoder selection ----
        // Hardware: only encoders registered for THIS device's adapter (MFT_ENUM_ADAPTER_LUID), each one
        // pre-flight probed with a short real encode; a failing/stalling one is skipped (leaked, not waited on).
        // Order: requested vendor first (if any), then the other HW encoders on this adapter, then software.
        const std::wstring tmp = opt_.container == Container::MP4 ? opt_.outputPath : opt_.outputPath + L".part.mp4";
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
        auto muxOut = [&](IMFSample* es) -> HRESULT {
            if (!wr.sw) {
                ComPtr<IMFMediaType> ot = hw->muxType();
                std::wstring e2;
                if (!wr.openPassthrough(tmp, ot.Get(), e2)) { { bool wa = wr.wantAudio; wr = Writer{}; wr.wantAudio = wa; } muxHr = E_FAIL; err = e2; return E_FAIL; }
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
            if (opt_.encoder != Encoder::Software && videoSupport) {
                auto encs = hwEncodersForDevice(dev.Get(), elog);
                const UINT want = vendorPci(opt_.encoder);
                std::stable_sort(encs.begin(), encs.end(), [want](const HwEncoderInfo& a, const HwEncoderInfo& b) {
                    return (want && a.pciVendor == want) > (want && b.pciVendor == want);
                });
                bool vpOk = !encs.empty() && conv.init(dev.Get(), src->w, src->h, W, H);
                if (!encs.empty() && !vpOk) {
                    elog += L"GPU NV12 conversion unavailable (" + conv.why + L"): feeding the encoder BGRA surfaces directly\n";
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
                        if (enc->open(dev.Get(), e, W, H, opt_.fps, br, opt_.quality >= Quality::High, elog)) {
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
                auto enc = std::make_unique<HwEncoder>();
                unsigned br = bitrateFor(opt_.quality, (int)W, (int)H, opt_.fps, opt_.bitrateCap);
                if (enc->openSoftware(W, H, opt_.fps, br, opt_.quality >= Quality::High, elog)) { hw = std::move(enc); swNv12 = true; opened = true; }
                else { enc->release(); opened = wr.open(tmp, dev.Get(), W, H, opt_, false, false, err); }
                r.encoderUsed = L"Software (Media Foundation H.264)";
                if (opt_.encoder != Encoder::Software) r.encoderUsed += L", hardware encoder unavailable";
            }
            if (!opened) { { bool wa = wr.wantAudio; wr = Writer{}; wr.wantAudio = wa; } DeleteFileW(tmp.c_str()); r.message = L"Encoder: " + err; return false; }
            const bool scaling = W != (src->w & ~1u) || H != (src->h & ~1u);
            if (!zeroCopy && !swNv12 && scaling) {  // CPU / RGB path can't scale: encode native instead
                { bool wa = wr.wantAudio; wr = Writer{}; wr.wantAudio = wa; } DeleteFileW(tmp.c_str());
                W = src->w & ~1u; H = src->h & ~1u;
                opened = wr.open(tmp, dev.Get(), W, H, opt_, false, false, err);
                if (!opened) { { bool wa = wr.wantAudio; wr = Writer{}; wr.wantAudio = wa; } DeleteFileW(tmp.c_str()); r.message = L"Encoder: " + err; return false; }
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
                        Writer pw; pw.wantAudio = wr.wantAudio; std::wstring e3;
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
            r.message = L"warm-up done"; { bool wa = wr.wantAudio; wr = Writer{}; wr.wantAudio = wa; } DeleteFileW(tmp.c_str()); return;
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
        if (opt_.notify && haveFrame) PostMessageW(opt_.notify, WM_APP_RECORDER_STARTED, 0, 0);

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
        while (!stop_ && haveFrame && r.message.empty()) {
            if (src->ended()) { windowClosed = true; break; }  // captured window closed: finish and keep the file
            if (fsm_.isPaused()) {
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
            if (!wr.sw) r.frames = 0;  // nothing ever came out of the encoder
        }
        {
            double secs = (double)next * dur / 1e7;
            r.encodeFps = secs > 0.1 ? r.frames / secs : 0;
        }
        if (wr.sw) {
            pushAudio(next * dur, true);
            while (!audioPending.empty()) { wr.writeAudio(audioPending.front().second, audioPending.front().first); audioPending.pop_front(); }
        }
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

        // ---- Finalize: budget = finalizeTimeoutMs/2 (default 4 s); remux gets the other half ----
        HRESULT fh = E_FAIL;
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
                    for (int i = 0; i < 40 && !DeleteFileW(tmpCopy.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND; ++i) Sleep(50);
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
                r.message = L"Finalize timed out - file discarded";
                DeleteFileW(tmp.c_str());  // best effort now; helper retries after Finalize returns
            } else {
                g_abandonLive.fetch_sub(1);
                fin.join();
                fh = st->hr.load();
                { bool wa = wr.wantAudio; wr = Writer{}; wr.wantAudio = wa; }
            }
        } else {
            { bool wa = wr.wantAudio; wr = Writer{}; wr.wantAudio = wa; }
            if (!r.frames) { DeleteFileW(tmp.c_str()); if (r.message.empty()) r.message = L"No frames recorded"; }
        }

        if (!r.timedOut) {
            if (FAILED(fh) && r.frames) {
                if (r.message.empty()) r.message = hrMsg(L"Finalize", fh);
                DeleteFileW(tmp.c_str());
            } else if (SUCCEEDED(fh) && r.frames) {
                r.file = tmp;
                if (opt_.container != Container::MP4) {
                    std::wstring ff = findFfmpeg();
                    DWORD remuxBudget = finalizeBudgetMs(opt_.finalizeTimeoutMs);
                    if (!ff.empty() && remux(ff, tmp, opt_.outputPath, remuxBudget)) {
                        DeleteFileW(tmp.c_str());
                        r.file = opt_.outputPath;
                    } else {
                        DeleteFileW(opt_.outputPath.c_str());
                        std::wstring mp4 = opt_.outputPath;
                        size_t slash = mp4.find_last_of(L"\\/"), dot = mp4.find_last_of(L'.');
                        if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) mp4.resize(dot);
                        mp4 += L".mp4";
                        if (MoveFileExW(tmp.c_str(), mp4.c_str(), MOVEFILE_REPLACE_EXISTING)) r.file = mp4;
                        else r.file = tmp;
                        if (ff.empty()) r.message = L"ffmpeg.exe not found, saved as MP4";
                        else { r.message = L"ffmpeg remux failed, saved as MP4"; DeleteFileW(tmp.c_str()); }
                    }
                }
            }
        }
        r.ok = r.frames > 0 && !r.timedOut && (r.message.empty() || r.message.find(L"saved as MP4") != std::wstring::npos);
    }();

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
    const bool won = claim_.claim(gen, [&] {
        result_ = r;
        fsm_.toIdle();
        started_ = true;
        if (doneEvent_) SetEvent(doneEvent_);
        if (opt_.notify) PostMessageW(opt_.notify, opt_.warmupOnly ? WM_APP_WARMUP_DONE : WM_APP_RECORDER_DONE, 0, 0);
    });
    // abort() already published for this session: our output is stale, remove any file we produced.
    if (!won && !r.file.empty()) DeleteFileW(r.file.c_str());
}

}  // namespace cr
