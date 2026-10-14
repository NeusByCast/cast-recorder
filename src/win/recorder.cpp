// Capture (WGC / DXGI Desktop Duplication / synthetic) -> D3D11 texture -> Media Foundation H.264 encoder -> MP4.
// Frames stay on the GPU on the hardware path: the captured texture is copied into a GDI-compatible
// BGRA texture (cursor is drawn with GDI), then wrapped with MFCreateDXGISurfaceBuffer. The sink writer's
// GPU video processor converts BGRA->NV12 and feeds the vendor's hardware MFT (NVENC/AMF/QuickSync).
#include "recorder.h"
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_2.h>
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

// ---------------- encoder ----------------
struct Writer {
    ComPtr<IMFSinkWriter> sw;
    ComPtr<IMFDXGIDeviceManager> mgr;
    DWORD stream = 0;
    bool gpu = false;
    std::wstring label;

    bool open(const std::wstring& path, ID3D11Device* dev, UINT w, UINT h, const RecordOptions& o, bool hardware, std::wstring& err) {
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

        ComPtr<IMFMediaType> outT, inT;
        MFCreateMediaType(&outT);
        outT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outT->SetUINT32(MF_MT_AVG_BITRATE, bitrateFor(o.quality, (int)w, (int)h, o.fps));
        outT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outT->SetUINT32(MF_MT_MPEG2_PROFILE, o.quality >= Quality::High ? 100u /* High */ : 77u /* Main */);
        MFSetAttributeSize(outT.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(outT.Get(), MF_MT_FRAME_RATE, (UINT32)o.fps, 1);
        MFSetAttributeRatio(outT.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = sw->AddStream(outT.Get(), &stream);
        if (FAILED(hr)) { err = hrMsg(L"AddStream", hr); return false; }

        MFCreateMediaType(&inT);
        inT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        inT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        inT->SetUINT32(MF_MT_DEFAULT_STRIDE, w * 4);
        MFSetAttributeSize(inT.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(inT.Get(), MF_MT_FRAME_RATE, (UINT32)o.fps, 1);
        MFSetAttributeRatio(inT.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = sw->SetInputMediaType(stream, inT.Get(), nullptr);
        if (FAILED(hr)) { err = hrMsg(L"SetInputMediaType", hr); return false; }
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

bool remux(const std::wstring& ffmpeg, const std::wstring& in, const std::wstring& out) {
    std::wstring cmd = L"\"" + ffmpeg + L"\" -hide_banner -loglevel error -y -i \"" + in + L"\" -c copy \"" + out + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0 && GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

std::wstring findFfmpeg() {
    std::wstring local = exeDir() + L"\\ffmpeg.exe";
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) return local;
    wchar_t p[MAX_PATH];
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, p, nullptr)) return p;
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

Recorder::~Recorder() { stop(); join(); }

bool Recorder::start(const RecordOptions& o) {
    if (running_) return false;
    join();
    opt_ = o;
    opt_.fps = normalizeFps(o.fps);
    result_ = RecordResult{};
    stop_ = false;
    running_ = true;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    startQpc_ = q.QuadPart;
    th_ = std::thread([this] { run(); });
    return true;
}
void Recorder::stop() { stop_ = true; }
void Recorder::join() { if (th_.joinable()) th_.join(); }
double Recorder::elapsedSeconds() const {
    if (!running_) return 0;
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    return double(q.QuadPart - startQpc_) / double(f.QuadPart);
}

void Recorder::run() {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    RecordResult& r = result_;
    std::wstring err;

    [&] {
        // ---- device on the adapter that owns the primary monitor ----
        HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        ComPtr<IDXGIFactory1> fac;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) { r.message = L"DXGI unavailable"; return; }
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput> output;
        POINT origin{0, 0};
        for (UINT i = 0; !output && fac->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            ComPtr<IDXGIOutput> o;
            for (UINT j = 0; adapter->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j) {
                DXGI_OUTPUT_DESC od{};
                o->GetDesc(&od);
                if (od.Monitor == mon) { output = o; origin = {od.DesktopCoordinates.left, od.DesktopCoordinates.top}; break; }
                o.Reset();
            }
            if (!output) adapter.Reset();
        }
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
        const UINT W = src->w & ~1u, H = src->h & ~1u;  // H.264 needs even dimensions

        // ---- encoder selection ----
        auto vendors = hardwareVendors();
        bool wantHw = opt_.encoder != Encoder::Software && videoSupport;
        std::wstring hwVendor;
        if (wantHw) {
            const wchar_t* want = vendorId(opt_.encoder);
            for (auto& v : vendors)
                if (!want || v.find(want) != std::wstring::npos) { hwVendor = v; break; }
            if (hwVendor.empty()) wantHw = false;
            else if (want && adapterVendor && adapterVendor != vendorPci(opt_.encoder)) {
                // The MF hardware encoder runs on the capture device's adapter; a different vendor's GPU can't take it.
                wantHw = false;
            }
        }
        const std::wstring tmp = opt_.container == Container::MP4 ? opt_.outputPath : opt_.outputPath + L".part.mp4";
        Writer wr;
        bool opened = false;
        if (wantHw) {
            opened = wr.open(tmp, dev.Get(), W, H, opt_, true, err);
            if (opened) r.encoderUsed = std::wstring(vendorLabel(hwVendor)) + L" (hardware)";
            else { wr = Writer{}; DeleteFileW(tmp.c_str()); }
        }
        if (!opened) {
            opened = wr.open(tmp, dev.Get(), W, H, opt_, false, err);
            r.encoderUsed = L"Software (Media Foundation H.264)";
            if (opt_.encoder != Encoder::Software && opt_.encoder != Encoder::Auto) r.encoderUsed += L", requested encoder unavailable";
        }
        if (!opened) { r.message = L"Encoder: " + err; return; }

        // ---- textures ----
        D3D11_TEXTURE2D_DESC td{};
        td.Width = src->w; td.Height = src->h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
        ComPtr<ID3D11Texture2D> latest;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &latest))) { r.message = L"CreateTexture2D failed"; return; }
        // Ring of encode surfaces so the async encoder never reads a texture we're overwriting.
        D3D11_TEXTURE2D_DESC ed = td; ed.Width = W; ed.Height = H; ed.MiscFlags = 0; ed.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        const int RING = 8;
        ComPtr<ID3D11Texture2D> ring[RING];
        ComPtr<ID3D11Texture2D> staging;
        for (auto& t : ring) if (FAILED(dev->CreateTexture2D(&ed, nullptr, &t))) { r.message = L"CreateTexture2D(ring) failed"; return; }
        if (!wr.gpu) {
            D3D11_TEXTURE2D_DESC sd = ed; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) { r.message = L"CreateTexture2D(staging) failed"; return; }
        }
        D3D11_BOX box{0, 0, 0, W, H, 1};

        // ---- paced loop ----
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        LARGE_INTEGER freq, t0;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        startQpc_ = t0.QuadPart;
        const LONGLONG dur = 10000000LL / opt_.fps;  // 100 ns units
        long long next = 0;  // frame index
        bool haveFrame = false;
        HRESULT whr = S_OK;
        while (!stop_) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            long long due = (now.QuadPart - t0.QuadPart) * opt_.fps / freq.QuadPart;  // frames that should exist by now
            if (due < next) {
                long long target = t0.QuadPart + next * freq.QuadPart / opt_.fps;
                LONGLONG waitMs = (target - now.QuadPart) * 1000 / freq.QuadPart;
                bool got = false;
                // Pull capture frames while waiting for the next tick.
                if (!src->acquire(ctx.Get(), latest.Get(), (UINT)(waitMs > 0 ? waitMs : 0), got)) { r.message = L"Capture failed"; break; }
                if (got) haveFrame = true;
                QueryPerformanceCounter(&now);
                if (now.QuadPart < target && timer) {
                    LARGE_INTEGER rel; rel.QuadPart = -((target - now.QuadPart) * 10000000LL / freq.QuadPart);
                    SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(timer, 50);
                }
                continue;
            }
            if (due > next + 1) { r.late += (unsigned long long)(due - next - 1); next = due - 1; }  // fell behind: skip ticks
            if (!haveFrame) {  // nothing captured yet (static screen at start): do a short blocking acquire
                bool got = false;
                if (!src->acquire(ctx.Get(), latest.Get(), 0, got)) { r.message = L"Capture failed"; break; }
                if (got) haveFrame = true; else { ++next; continue; }
            }
            if (cursorByGdi) drawCursor(latest.Get(), origin);
            ID3D11Texture2D* t = ring[next % RING].Get();
            ctx->CopySubresourceRegion(t, 0, 0, 0, 0, latest.Get(), 0, &box);
            ComPtr<IMFMediaBuffer> buf;
            if (wr.gpu) {
                whr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), t, 0, FALSE, &buf);
                if (SUCCEEDED(whr)) { ComPtr<IMF2DBuffer> b2; if (SUCCEEDED(buf.As(&b2))) { DWORD len = 0; b2->GetContiguousLength(&len); buf->SetCurrentLength(len); } }
            } else {
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
            // Drain new capture frames without blocking so "latest" is fresh next tick.
            bool got = false;
            if (!src->acquire(ctx.Get(), latest.Get(), 0, got)) { r.message = L"Capture failed"; break; }
            if (!got) ++r.duplicated;
        }
        if (timer) CloseHandle(timer);
        src.reset();
        HRESULT fh = r.frames ? wr.sw->Finalize() : E_FAIL;
        wr = Writer{};
        if (FAILED(fh)) { if (r.message.empty()) r.message = r.frames ? hrMsg(L"Finalize", fh) : L"No frames recorded"; DeleteFileW(tmp.c_str()); return; }
        r.file = tmp;
        if (opt_.container != Container::MP4) {
            std::wstring ff = findFfmpeg();
            if (!ff.empty() && remux(ff, tmp, opt_.outputPath)) { DeleteFileW(tmp.c_str()); r.file = opt_.outputPath; }
            else {
                std::wstring mp4 = opt_.outputPath.substr(0, opt_.outputPath.find_last_of(L'.')) + L".mp4";
                MoveFileExW(tmp.c_str(), mp4.c_str(), MOVEFILE_REPLACE_EXISTING);
                r.file = mp4;
                r.message = L"ffmpeg.exe not found, saved as MP4";
            }
        }
        r.ok = r.message.empty() || r.message.rfind(L"ffmpeg", 0) == 0;
    }();

    MFShutdown();
    if (SUCCEEDED(hrCo)) CoUninitialize();
    running_ = false;
    if (opt_.notify) PostMessageW(opt_.notify, WM_APP_RECORDER_DONE, 0, 0);
}

}  // namespace cr
