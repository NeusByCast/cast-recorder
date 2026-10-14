// Capture (WGC / DXGI Desktop Duplication / synthetic) -> D3D11 texture -> Media Foundation H.264 encoder -> MP4.
// Frames stay on the GPU on the hardware path: the captured texture is copied into a GDI-compatible
// BGRA texture (cursor is drawn with GDI), then wrapped with MFCreateDXGISurfaceBuffer. The sink writer's
// GPU video processor converts BGRA->NV12 and feeds the vendor's hardware MFT (NVENC/AMF/QuickSync).
#include "recorder.h"
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
#include <cstdio>
#include <memory>
#include <vector>
#include "capture_wgc.h"
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

struct SyntheticSource : Source {
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
    const wchar_t* name() const override { return L"Windows Graphics Capture"; }
    bool init(ID3D11Device* d, HMONITOR mon, bool cursor, std::wstring& err) {
        if (!cap.start(d, mon, cursor, err)) return false;
        w = cap.width(); h = cap.height();
        return true;
    }
    bool acquire(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, UINT timeoutMs, bool& gotNew) override {
        return cap.copyLatest(ctx, dst, timeoutMs, gotNew);
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

    bool init(ID3D11Device* d, UINT srcW, UINT srcH, UINT dstW, UINT dstH) {
        inW = srcW; inH = srcH; outW = dstW; outH = dstH;
        if (FAILED(d->QueryInterface(IID_PPV_ARGS(&vdev))) || FAILED(d->QueryInterface(IID_PPV_ARGS(&vctx)))) return false;
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = srcW; cd.InputHeight = srcH;
        cd.OutputWidth = dstW; cd.OutputHeight = dstH;
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(vdev->CreateVideoProcessorEnumerator(&cd, &venum))) return false;
        D3D11_VIDEO_PROCESSOR_CAPS caps{};
        if (FAILED(venum->GetVideoProcessorCaps(&caps))) return false;
        if (FAILED(vdev->CreateVideoProcessor(venum.Get(), 0, &vp))) return false;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = dstW; td.Height = dstH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(d->CreateTexture2D(&td, nullptr, &nv12))) return false;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(vdev->CreateVideoProcessorOutputView(nv12.Get(), venum.Get(), &ovd, &outView))) return false;
        RECT src{0, 0, (LONG)srcW, (LONG)srcH}, dst{0, 0, (LONG)dstW, (LONG)dstH};
        vctx->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &src);
        vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dst);
        vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dst);
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

        hr = sw->BeginWriting();
        if (FAILED(hr)) { err = hrMsg(L"BeginWriting", hr); return false; }
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

static AbandonLatch& processLatch() { static AbandonLatch* l = new AbandonLatch; return *l; }  // never destroyed

Recorder::Recorder() : abandon_(processLatch()) {
    doneEvent_ = CreateEventW(nullptr, TRUE, TRUE, nullptr);  // starts signaled (Idle)
}
Recorder::~Recorder() {
    stop();
    if (!waitUntilIdle(3000)) abort();
    waitUntilIdle(2000);
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
        if (opt_.notify) PostMessageW(opt_.notify, WM_APP_RECORDER_DONE, 0, 0);
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
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // leave games / UI the headroom
    RecordResult r;
    bool abandonedMf = false;
    std::wstring err;

    [&] {
        // ---- device on the adapter that owns the primary monitor (or an explicit gpuIndex) ----
        HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
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
                    if (!output) { output = o; origin = {od.DesktopCoordinates.left, od.DesktopCoordinates.top}; }
                    o.Reset();
                }
                // Primary monitor isn't on this GPU: capture the GPU's first display so WGC and DXGI agree.
                if (output) { DXGI_OUTPUT_DESC od{}; output->GetDesc(&od); mon = od.Monitor; }
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
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
        UINT adapterVendor = 0;
        {
            ComPtr<IDXGIDevice> dxgiDev; ComPtr<IDXGIAdapter> ad;
            if (SUCCEEDED(dev.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&ad))) { DXGI_ADAPTER_DESC d{}; ad->GetDesc(&d); adapterVendor = d.VendorId; }
        }

        // ---- capture source: WGC -> Desktop Duplication -> synthetic (tests only) ----
        std::unique_ptr<Source> src;
        bool cursorByGdi = false;
        std::wstring capErr;
#ifdef CR_HAVE_WGC
        {
            auto w = std::make_unique<WgcSourceAdapter>();
            if (w->init(dev.Get(), mon, opt_.captureCursor, capErr)) src = std::move(w);
        }
#endif
        if (!src && output) {
            auto d = std::make_unique<DuplicationSource>();
            if (d->init(dev.Get(), output.Get(), capErr)) { src = std::move(d); cursorByGdi = opt_.captureCursor; }
        }
        if (!src && opt_.allowSynthetic) {
            auto s = std::make_unique<SyntheticSource>();
            if (s->init(dev.Get())) src = std::move(s);
        }
        if (!src) { r.message = L"Screen capture unavailable: " + (capErr.empty() ? std::wstring(L"no output") : capErr); return; }
        r.captureUsed = src->name();
        UINT W = 0, H = 0;  // encoded size (even; optionally downscaled)
        scaledSize(opt_.scale, src->w, src->h, W, H);

        // ---- encoder selection ----
        auto vendors = hardwareVendors();
        bool wantHw = opt_.encoder != Encoder::Software && videoSupport;
        std::wstring hwVendor;
        if (wantHw) {
            const wchar_t* want = vendorId(opt_.encoder);
            for (auto& v : vendors)
                if (!want || v.find(want) != std::wstring::npos) { hwVendor = v; break; }
            if (hwVendor.empty()) wantHw = false;
            else if (want && adapterVendor && adapterVendor != vendorPci(opt_.encoder)) wantHw = false;  // encoder must live on capture GPU
        }
        const std::wstring tmp = opt_.container == Container::MP4 ? opt_.outputPath : opt_.outputPath + L".part.mp4";
        // Zero-copy: GPU video processor BGRA->NV12 (+scale) feeding the HW MFT through the DXGI manager.
        GpuConvert conv;
        bool zeroCopy = wantHw && conv.init(dev.Get(), src->w, src->h, W, H);
        Writer wr;
        bool opened = false;
        if (wantHw) {
            opened = wr.open(tmp, dev.Get(), W, H, opt_, true, zeroCopy, err);
            if (!opened && zeroCopy) { wr = Writer{}; DeleteFileW(tmp.c_str()); zeroCopy = false; opened = wr.open(tmp, dev.Get(), W, H, opt_, true, false, err); }
            if (opened) r.encoderUsed = std::wstring(vendorLabel(hwVendor)) + L" (hardware)";
            else { wr = Writer{}; DeleteFileW(tmp.c_str()); zeroCopy = false; }
        }
        if (!opened) {
            zeroCopy = false;
            opened = wr.open(tmp, dev.Get(), W, H, opt_, false, false, err);
            r.encoderUsed = L"Software (Media Foundation H.264)";
            if (opt_.encoder != Encoder::Software && opt_.encoder != Encoder::Auto) r.encoderUsed += L", requested encoder unavailable";
        }
        if (!opened) { wr = Writer{}; DeleteFileW(tmp.c_str()); r.message = L"Encoder: " + err; return; }
        const bool scaling = W != (src->w & ~1u) || H != (src->h & ~1u);
        if (!zeroCopy && scaling) {  // CPU / RGB path can't scale: encode native instead
            // (re-open at native size keeps output correct; zero-copy path is the fast one)
            wr = Writer{}; DeleteFileW(tmp.c_str());
            W = src->w & ~1u; H = src->h & ~1u;
            opened = wr.open(tmp, dev.Get(), W, H, opt_, wantHw && r.encoderUsed.find(L"hardware") != std::wstring::npos, false, err);
            if (!opened) { wr = Writer{}; DeleteFileW(tmp.c_str()); r.message = L"Encoder: " + err; return; }
        }

        // ---- textures ----
        D3D11_TEXTURE2D_DESC td{};
        td.Width = src->w; td.Height = src->h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
        ComPtr<ID3D11Texture2D> latest;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &latest))) { r.message = L"CreateTexture2D failed"; return; }
        // Ring of encode surfaces so the async encoder never reads a texture we're overwriting.
        D3D11_TEXTURE2D_DESC ed = td; ed.Width = W; ed.Height = H; ed.MiscFlags = 0;
        if (zeroCopy) ed.Format = DXGI_FORMAT_NV12;
        const int RING = 8;
        ComPtr<ID3D11Texture2D> ring[RING];
        ComPtr<ID3D11Texture2D> staging;
        for (auto& t : ring) if (FAILED(dev->CreateTexture2D(&ed, nullptr, &t))) { r.message = L"CreateTexture2D(ring) failed"; return; }
        if (!wr.gpu) {
            D3D11_TEXTURE2D_DESC sd = ed; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) { r.message = L"CreateTexture2D(staging) failed"; return; }
        }
        D3D11_BOX box{0, 0, 0, W, H, 1};

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
        HRESULT whr = S_OK;
        long long pauseBegin = 0;  // worker's own clock: QPC when it first observed Paused
        while (!stop_ && haveFrame && r.message.empty()) {
            if (fsm_.isPaused()) {
                // Keep capture warm (drain frames) but write nothing; the media clock does not advance.
                if (!pauseBegin) { LARGE_INTEGER pb; QueryPerformanceCounter(&pb); pauseBegin = pb.QuadPart; }
                bool got = false;
                if (!src->acquire(ctx.Get(), latest.Get(), 20, got)) { r.message = L"Capture failed"; break; }
                continue;
            }
            if (pauseBegin) {  // shift the pacing origin by exactly the pause the worker saw: no "missed" frames
                LARGE_INTEGER pe; QueryPerformanceCounter(&pe);
                t0.QuadPart += pe.QuadPart - pauseBegin;
                pauseBegin = 0;
                ++r.pauses;
            }
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            long long due = (now.QuadPart - t0.QuadPart) * opt_.fps / freq.QuadPart;
            if (due < next) {
                long long target = t0.QuadPart + next * freq.QuadPart / opt_.fps;
                LONGLONG waitMs = (target - now.QuadPart) * 1000 / freq.QuadPart;
                bool got = false;
                if (!src->acquire(ctx.Get(), latest.Get(), (UINT)(waitMs > 1 ? waitMs - 1 : 0), got)) { r.message = L"Capture failed"; break; }
                QueryPerformanceCounter(&now);
                if (now.QuadPart < target && timer) {
                    LARGE_INTEGER rel; rel.QuadPart = -((target - now.QuadPart) * 10000000LL / freq.QuadPart);
                    SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(timer, 50);
                }
                continue;
            }
            if (due > next + 1) { unsigned long long skip = (unsigned long long)(due - next - 1); r.late += skip; r.dropped += skip; next = due - 1; }
            if (cursorByGdi) drawCursor(latest.Get(), origin);
            ID3D11Texture2D* t = ring[next % RING].Get();
            ComPtr<IMFMediaBuffer> buf;
            if (zeroCopy) {
                // GPU BGRA->NV12 (+scale) into the ring surface; no CPU readback.
                if (!conv.convert(latest.Get())) {
                    // Keep recording: re-send the last good NV12 frame, and rebuild the video processor
                    // every 30 failures (device hiccup, mode change). Never stop the session for this.
                    ++r.convertFailures;
                    if (r.convertFailures % 30 == 0) { GpuConvert fresh; if (fresh.init(dev.Get(), src->w, src->h, W, H)) conv = std::move(fresh); }
                }
                ctx->CopyResource(t, conv.nv12.Get());
                whr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), t, 0, FALSE, &buf);
                if (SUCCEEDED(whr)) { ComPtr<IMF2DBuffer> b2; if (SUCCEEDED(buf.As(&b2))) { DWORD len = 0; b2->GetContiguousLength(&len); buf->SetCurrentLength(len); } }
            } else if (wr.gpu) {
                ctx->CopySubresourceRegion(t, 0, 0, 0, 0, latest.Get(), 0, &box);
                whr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), t, 0, FALSE, &buf);
                if (SUCCEEDED(whr)) { ComPtr<IMF2DBuffer> b2; if (SUCCEEDED(buf.As(&b2))) { DWORD len = 0; b2->GetContiguousLength(&len); buf->SetCurrentLength(len); } }
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
            if (SUCCEEDED(whr)) whr = wr.write(buf.Get(), next * dur, dur);
            if (FAILED(whr)) { r.message = hrMsg(L"WriteSample", whr); break; }
            ++r.frames;
            ++next;
            bool got = false;
            if (!src->acquire(ctx.Get(), latest.Get(), 0, got)) { r.message = L"Capture failed"; break; }
            if (!got) ++r.duplicated;  // reuse last frame
        }
        if (timer) CloseHandle(timer);
        fsm_.beginFinish();

        {   // diagnostics: effective path
            wchar_t b[512];
            swprintf(b, 512, L"adapter=%ls; output=%ls; encoder=%ls; zero_copy=%ls; size=%ux%u@%d; dropped=%llu; duplicated=%llu; convert_failures=%llu",
                     adapterName.c_str(), output ? L"local" : L"cross-adapter", r.encoderUsed.c_str(), zeroCopy ? L"yes" : L"no", W, H, opt_.fps,
                     r.dropped, r.duplicated, r.convertFailures);
            r.pathLog = b;
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
                else { abandon_.abandon(); abandonedMf = true; closer.detach(); }
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
                wr = Writer{};
            }
        } else {
            wr = Writer{};
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
    if (!abandonedMf && !abandon_.poisoned.load()) {
        if (mfStarted) MFShutdown();
        if (SUCCEEDED(hrCo)) CoUninitialize();
    }
    const bool won = claim_.claim(gen, [&] {
        result_ = r;
        fsm_.toIdle();
        started_ = true;
        if (doneEvent_) SetEvent(doneEvent_);
        if (opt_.notify) PostMessageW(opt_.notify, WM_APP_RECORDER_DONE, 0, 0);
    });
    // abort() already published for this session: our output is stale, remove any file we produced.
    if (!won && !r.file.empty()) DeleteFileW(r.file.c_str());
}

}  // namespace cr
