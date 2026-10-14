// Explicit async hardware H.264 MFT driver. Replaces "let the sink writer pick a hardware MFT", which on
// hybrid laptops can bind the encoder of one GPU to the D3D device of another (both AMD on iGPU+dGPU
// systems: same VEN_1002, different LUID) and then stall forever in ProcessInput/Finalize.
#include "hwenc.h"
#include <dxgi.h>
#include <mferror.h>
#include <oleauto.h>
#include <strmif.h>
#include <codecapi.h>
#include <cstdio>
#include <atomic>
#include <memory>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace cr {
namespace {

// Spelled out locally so we don't depend on SDK/mingw header versions.
const GUID kMFT_ENUM_ADAPTER_LUID = {0x1d39518c, 0xe220, 0x4da8, {0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1}};
const GUID kMF_TRANSFORM_ASYNC = {0xf81a699a, 0x649a, 0x497d, {0x8c, 0x73, 0x29, 0xf8, 0xfe, 0xd6, 0xad, 0x7a}};
const GUID kMF_TRANSFORM_ASYNC_UNLOCK = {0xe5666d6b, 0x3422, 0x4eb6, {0xa4, 0x21, 0xda, 0x7d, 0xb1, 0xf8, 0xe2, 0x07}};
const GUID kMFT_ENUM_HARDWARE_VENDOR_ID = {0x3aecb0cc, 0x035b, 0x4bcc, {0x81, 0x85, 0x2b, 0x8d, 0x55, 0x1e, 0xf3, 0xaf}};
const GUID kMFT_FRIENDLY_NAME = {0x314ffbae, 0x5b41, 0x4c95, {0x9c, 0x19, 0x4e, 0x7d, 0x58, 0x6f, 0xac, 0xe3}};
const GUID kIID_ICodecAPI = {0x901db4c7, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};
const GUID kCODECAPI_AVLowLatencyMode = {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};
const GUID kCODECAPI_AVEncCommonRateControlMode = {0x1c0608e9, 0x370c, 0x4710, {0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23}};
const GUID kCODECAPI_AVEncCommonMeanBitRate = {0xf7222374, 0x2144, 0x4815, {0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52}};
const GUID kCODECAPI_AVEncMPVDefaultBPictureCount = {0x8d390aac, 0xdc5c, 0x4200, {0xb5, 0x7f, 0x81, 0x4d, 0x04, 0xba, 0xba, 0xb2}};
const GUID kCODECAPI_AVEncCommonMaxBitRate = {0x9651eae4, 0x39b9, 0x4ebf, {0x85, 0xef, 0xd7, 0xf4, 0x44, 0xec, 0x74, 0x65}};
const GUID kCODECAPI_AVEncCommonQualityVsSpeed = {0x98332df8, 0x03cd, 0x476b, {0x89, 0xfa, 0x3f, 0x9e, 0x44, 0x2d, 0xec, 0x9f}};
const GUID kCODECAPI_AVEncMPVGOPSize = {0x95f31b26, 0x95a4, 0x41aa, {0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1}};

typedef HRESULT(WINAPI* MFTEnum2Fn)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*, const MFT_REGISTER_TYPE_INFO*,
                                     IMFAttributes*, IMFActivate***, UINT32*);

void logHr(std::wstring& log, const wchar_t* what, HRESULT hr) {
    wchar_t b[200];
    swprintf(b, 200, L"%ls: 0x%08lX%ls\n", what, (unsigned long)hr, SUCCEEDED(hr) ? L" ok" : L" FAILED");
    log += b;
}

bool adapterLuid(ID3D11Device* dev, LUID& luid, UINT& vendor) {
    ComPtr<IDXGIDevice> d; ComPtr<IDXGIAdapter> a;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&d))) || FAILED(d->GetAdapter(&a))) return false;
    DXGI_ADAPTER_DESC ad{}; a->GetDesc(&ad);
    luid = ad.AdapterLuid; vendor = ad.VendorId;
    return true;
}

std::vector<ComPtr<IMFActivate>> enumForDevice(ID3D11Device* dev, std::wstring& log) {
    std::vector<ComPtr<IMFActivate>> out;
    LUID luid{}; UINT vendor = 0;
    if (!adapterLuid(dev, luid, vendor)) { log += L"enum: no adapter for device\n"; return out; }
    auto fn = (MFTEnum2Fn)GetProcAddress(GetModuleHandleW(L"mfplat.dll"), "MFTEnum2");
    if (!fn) { log += L"enum: MFTEnum2 unavailable (pre-1703 Windows)\n"; return out; }
    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 1);
    attr->SetBlob(kMFT_ENUM_ADAPTER_LUID, (const UINT8*)&luid, sizeof luid);
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** acts = nullptr; UINT32 n = 0;
    HRESULT hr = fn(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &outInfo,
                    attr.Get(), &acts, &n);
    wchar_t b[160];
    swprintf(b, 160, L"MFTEnum2(adapter LUID %08lX:%08lX, vendor %04X): 0x%08lX, %u encoder(s)\n",
             (unsigned long)luid.HighPart, (unsigned long)luid.LowPart, vendor, (unsigned long)hr, n);
    log += b;
    if (FAILED(hr)) return out;
    for (UINT32 i = 0; i < n; ++i) { out.emplace_back(acts[i]); acts[i]->Release(); }
    CoTaskMemFree(acts);
    return out;
}

std::wstring str(IMFActivate* a, const GUID& k) {
    wchar_t* v = nullptr; UINT32 len = 0;
    if (FAILED(a->GetAllocatedString(k, &v, &len))) return {};
    std::wstring s(v); CoTaskMemFree(v); return s;
}

void setCodec(ICodecAPI* c, const GUID& k, VARTYPE vt, ULONG v, const wchar_t* name, std::wstring& log) {
    VARIANT var; VariantInit(&var); var.vt = vt;
    if (vt == VT_BOOL) var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE; else var.ulVal = v;
    HRESULT hr = c->SetValue(&k, &var);
    logHr(log, name, hr);
}

}  // namespace

std::vector<HwEncoderInfo> hwEncodersForDevice(ID3D11Device* dev, std::wstring& log) {
    std::vector<HwEncoderInfo> r;
    auto acts = enumForDevice(dev, log);
    for (size_t i = 0; i < acts.size(); ++i) {
        HwEncoderInfo e;
        e.vendor = str(acts[i].Get(), kMFT_ENUM_HARDWARE_VENDOR_ID);
        e.name = str(acts[i].Get(), kMFT_FRIENDLY_NAME);
        if (e.vendor.find(L"10DE") != std::wstring::npos) e.pciVendor = 0x10DE;
        else if (e.vendor.find(L"1002") != std::wstring::npos || e.vendor.find(L"1022") != std::wstring::npos) e.pciVendor = 0x1002;
        else if (e.vendor.find(L"8086") != std::wstring::npos) e.pciVendor = 0x8086;
        e.index = (int)i;
        log += L"  [" + std::to_wstring(i) + L"] " + e.name + L" (" + e.vendor + L")\n";
        r.push_back(e);
    }
    return r;
}

HwEncoder::~HwEncoder() { release(); }

void HwEncoder::release() {
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        ComPtr<IMFShutdown> sd;
        if (SUCCEEDED(mft_.As(&sd))) sd->Shutdown();
    }
    if (act_) act_->ShutdownObject();
    ev_.Reset(); mft_.Reset(); outType_.Reset(); mgr_.Reset(); act_.Reset();
}

void HwEncoder::leak() {
    ev_.Detach(); mft_.Detach(); outType_.Detach(); mgr_.Detach(); act_.Detach();
}

bool HwEncoder::open(ID3D11Device* dev, const HwEncoderInfo& info, UINT w, UINT h, int fps, unsigned bitrate, bool high,
                     std::wstring& log) {
    auto acts = enumForDevice(dev, log);
    if (info.index < 0 || (size_t)info.index >= acts.size()) { log += L"open: encoder index gone\n"; return false; }
    act_ = acts[info.index];
    label = info.name;
    HRESULT hr = act_->ActivateObject(IID_PPV_ARGS(&mft_));
    logHr(log, L"ActivateObject", hr);
    if (FAILED(hr)) return false;

    ComPtr<IMFAttributes> a;
    UINT32 isAsync = 0;
    if (SUCCEEDED(mft_->GetAttributes(&a)) && a) {
        a->GetUINT32(kMF_TRANSFORM_ASYNC, &isAsync);
        if (isAsync) { hr = a->SetUINT32(kMF_TRANSFORM_ASYNC_UNLOCK, TRUE); logHr(log, L"MF_TRANSFORM_ASYNC_UNLOCK", hr); if (FAILED(hr)) return false; }
        a->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    if (!isAsync) { log += L"MFT is synchronous: not supported on this path\n"; return false; }
    hr = mft_.As(&ev_);
    logHr(log, L"QI IMFMediaEventGenerator", hr);
    if (FAILED(hr)) return false;

    hr = mft_->GetStreamIDs(1, &inId_, 1, &outId_);
    if (hr == E_NOTIMPL) { inId_ = 0; outId_ = 0; hr = S_OK; }
    logHr(log, L"GetStreamIDs", hr);

    UINT token = 0;
    hr = MFCreateDXGIDeviceManager(&token, &mgr_);
    if (SUCCEEDED(hr)) hr = mgr_->ResetDevice(dev, token);
    logHr(log, L"DXGI device manager (same device as capture)", hr);
    if (FAILED(hr)) return false;
    hr = mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)mgr_.Get());
    logHr(log, L"MFT_MESSAGE_SET_D3D_MANAGER", hr);
    if (FAILED(hr)) return false;

    return configure(w, h, fps, bitrate, high, log);
}

bool HwEncoder::openSoftware(UINT w, UINT h, int fps, unsigned bitrate, bool high, std::wstring& log) {
    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, MFVideoFormat_NV12}, outInfo{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** acts = nullptr; UINT32 n = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &inInfo, &outInfo, &acts, &n);
    logHr(log, L"MFTEnumEx(software H.264)", hr);
    if (FAILED(hr) || !n) { if (acts) CoTaskMemFree(acts); return false; }
    act_ = acts[0];
    for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    label = str(act_.Get(), kMFT_FRIENDLY_NAME);
    hr = act_->ActivateObject(IID_PPV_ARGS(&mft_));
    logHr(log, L"ActivateObject(software)", hr);
    if (FAILED(hr)) return false;
    sync_ = true;
    hr = mft_->GetStreamIDs(1, &inId_, 1, &outId_);
    if (FAILED(hr)) { inId_ = 0; outId_ = 0; }
    return configure(w, h, fps, bitrate, high, log);
}

bool HwEncoder::configure(UINT w, UINT h, int fps, unsigned bitrate, bool high, std::wstring& log) {
    HRESULT hr = S_OK;
    w_ = w; h_ = h; fps_ = fps; br_ = bitrate;
    // Rate control / latency before types (some drivers lock these at SetOutputType).
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(mft_->QueryInterface(kIID_ICodecAPI, (void**)codec.GetAddressOf())) && codec) {
        // Peak-constrained VBR at a resolution/fps-scaled mean, quality-first preset, no low-latency mode
        // (low-latency/CBR on AMF/QSV trades a lot of quality for latency we don't need when recording).
        setCodec(codec.Get(), kCODECAPI_AVEncCommonRateControlMode, VT_UI4, eAVEncCommonRateControlMode_PeakConstrainedVBR, L"CODECAPI RateControl=PeakConstrainedVBR", log);
        setCodec(codec.Get(), kCODECAPI_AVEncCommonMeanBitRate, VT_UI4, bitrate, L"CODECAPI MeanBitRate", log);
        setCodec(codec.Get(), kCODECAPI_AVEncCommonMaxBitRate, VT_UI4, (ULONG)((double)bitrate * 1.5), L"CODECAPI MaxBitRate", log);
        setCodec(codec.Get(), kCODECAPI_AVEncCommonQualityVsSpeed, VT_UI4, high ? 100 : 60, L"CODECAPI QualityVsSpeed", log);
        setCodec(codec.Get(), kCODECAPI_AVLowLatencyMode, VT_BOOL, 0, L"CODECAPI LowLatencyMode=off", log);
        setCodec(codec.Get(), kCODECAPI_AVEncMPVDefaultBPictureCount, VT_UI4, 0, L"CODECAPI BPictureCount=0", log);
        setCodec(codec.Get(), kCODECAPI_AVEncMPVGOPSize, VT_UI4, (ULONG)fps * 2, L"CODECAPI GOPSize", log);
    } else log += L"ICodecAPI not available\n";

    // Output type FIRST (required by AMF/NVENC/QSV encoders), then input.
    MFCreateMediaType(&outType_);
    outType_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType_->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    outType_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType_->SetUINT32(MF_MT_MPEG2_PROFILE, high ? 100u : 77u);
    outType_->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    outType_->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    outType_->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    outType_->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    MFSetAttributeSize(outType_.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(outType_.Get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
    MFSetAttributeRatio(outType_.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = mft_->SetOutputType(outId_, outType_.Get(), 0);
    logHr(log, L"SetOutputType(H264)", hr);
    if (FAILED(hr) && high) {  // some iGPUs reject High at this size: retry Main
        outType_->SetUINT32(MF_MT_MPEG2_PROFILE, 77u);
        hr = mft_->SetOutputType(outId_, outType_.Get(), 0);
        logHr(log, L"SetOutputType(H264 Main)", hr);
    }
    if (FAILED(hr)) return false;
    outType_->GetUINT32(MF_MT_MPEG2_PROFILE, &profile_);

    // Input: pick the MFT's own NV12 type (keeps its private attributes) and set our size/rate.
    ComPtr<IMFMediaType> inT;
    bool found = false;
    for (DWORD i = 0; !found; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(mft_->GetInputAvailableType(inId_, i, &t))) break;
        GUID sub{};
        if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) { inT = t; found = true; }
    }
    if (!inT) { MFCreateMediaType(&inT); inT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); inT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12); }
    inT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inT->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    inT->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    inT->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    inT->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    MFSetAttributeSize(inT.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(inT.Get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
    MFSetAttributeRatio(inT.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = mft_->SetInputType(inId_, inT.Get(), 0);
    logHr(log, L"SetInputType(NV12)", hr);
    if (FAILED(hr)) return false;

    MFT_OUTPUT_STREAM_INFO osi{};
    if (SUCCEEDED(mft_->GetOutputStreamInfo(outId_, &osi))) {
        providesSamples_ = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outBufSize_ = osi.cbSize ? osi.cbSize : w * h * 3 / 2;
    }
    hr = mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    logHr(log, L"NOTIFY_BEGIN_STREAMING", hr);
    if (FAILED(hr)) return false;
    hr = mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    logHr(log, L"NOTIFY_START_OF_STREAM", hr);
    return SUCCEEDED(hr);
}

ComPtr<IMFMediaType> HwEncoder::currentOutputType() {
    ComPtr<IMFMediaType> t;
    if (mft_ && SUCCEEDED(mft_->GetOutputCurrentType(outId_, &t))) return t;
    return outType_;
}

HRESULT HwEncoder::processOutput(const OutputFn& out) {
    MFT_OUTPUT_DATA_BUFFER ob{};
    ob.dwStreamID = outId_;
    ComPtr<IMFSample> mine;
    if (!providesSamples_) {
        ComPtr<IMFMediaBuffer> b;
        MFCreateMemoryBuffer(outBufSize_, &b);
        MFCreateSample(&mine);
        mine->AddBuffer(b.Get());
        ob.pSample = mine.Get();
    }
    DWORD status = 0;
    HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
    if (ob.pEvents) ob.pEvents->Release();
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {  // renegotiate: take the MFT's new output type
        ComPtr<IMFMediaType> t;
        if (SUCCEEDED(mft_->GetOutputAvailableType(outId_, 0, &t))) { mft_->SetOutputType(outId_, t.Get(), 0); outType_ = t; }
        if (ob.pSample && !mine) ob.pSample->Release();
        return S_OK;
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { if (ob.pSample && !mine) ob.pSample->Release(); return sync_ ? hr : S_OK; }
    if (FAILED(hr)) { if (ob.pSample && !mine) ob.pSample->Release(); return hr; }
    HRESULT ohr = S_OK;
    if (ob.pSample) { ohr = deliver(ob.pSample, out); if (!mine) ob.pSample->Release(); }
    return ohr;
}

namespace {
// Annex-B NAL walk: calls f(type, ptr-to-start-code, total length incl. start code).
template <class F> void forNals(const BYTE* p, DWORD n, F f) {
    DWORD i = 0;
    auto sc = [&](DWORD k) -> DWORD { if (k + 3 <= n && p[k] == 0 && p[k + 1] == 0 && p[k + 2] == 1) return 3;
                                      if (k + 4 <= n && p[k] == 0 && p[k + 1] == 0 && p[k + 2] == 0 && p[k + 3] == 1) return 4; return 0; };
    while (i < n && !sc(i)) ++i;
    while (i < n) {
        DWORD l = sc(i), j = i + l;
        while (j < n && !sc(j)) ++j;
        if (i + l < n) f(p[i + l] & 0x1f, p + i, j - i);
        i = j;
    }
}
}  // namespace

HRESULT HwEncoder::deliver(IMFSample* s, const OutputFn& out) {
    ComPtr<IMFMediaBuffer> b;
    if (FAILED(s->ConvertToContiguousBuffer(&b))) return S_OK;
    BYTE* p = nullptr; DWORD len = 0;
    if (FAILED(b->Lock(&p, nullptr, &len))) return S_OK;
    bool idr = false; std::vector<BYTE> sps, pps;
    forNals(p, len, [&](int t, const BYTE* q, DWORD l) {
        if (t == 5) idr = true;
        else if (t == 7 && sps.empty()) sps.assign(q, q + l);
        else if (t == 8 && pps.empty()) pps.assign(q, q + l);
    });
    b->Unlock();
    if (seqHeader_.empty() && !sps.empty() && !pps.empty()) { seqHeader_ = sps; seqHeader_.insert(seqHeader_.end(), pps.begin(), pps.end()); }
    UINT32 clean = 0;
    if (FAILED(s->GetUINT32(MFSampleExtension_CleanPoint, &clean))) clean = 0;
    if (idr && !clean) { s->SetUINT32(MFSampleExtension_CleanPoint, TRUE); clean = 1; }
    if (!gotKey_) {
        if (!clean) return S_OK;  // the MP4/MKV must start on a keyframe
        gotKey_ = true;
    }
    const LONGLONG dur = 10000000LL / fps_;
    LONGLONG t = 0, d = 0;
    if (FAILED(s->GetSampleTime(&t))) { t = nextTime_; s->SetSampleTime(t); }
    if (FAILED(s->GetSampleDuration(&d)) || d <= 0) s->SetSampleDuration(dur);
    nextTime_ = t + dur;
    ++outputs;
    return out(s);
}

ComPtr<IMFMediaType> HwEncoder::muxType() {
    // Clean H.264 type for the pass-through writer, always carrying SPS/PPS (MF_MT_MPEG_SEQUENCE_HEADER).
    ComPtr<IMFMediaType> cur = currentOutputType(), t;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    t->SetUINT32(MF_MT_AVG_BITRATE, br_);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_MPEG2_PROFILE, profile_);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w_, h_);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, (UINT32)fps_, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    UINT32 sz = 0;
    std::vector<BYTE> hdr;
    if (cur && SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sz)) && sz) { hdr.resize(sz); cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, hdr.data(), sz, nullptr); }
    if (hdr.empty()) hdr = seqHeader_;
    if (!hdr.empty()) t->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, hdr.data(), (UINT32)hdr.size());
    return t;
}

HRESULT HwEncoder::pump(const OutputFn& out) {
    if (sync_) {
        for (;;) { HRESULT hr = processOutput(out); if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK; if (FAILED(hr)) return hr; }
    }
    for (;;) {
        ComPtr<IMFMediaEvent> e;
        HRESULT hr = ev_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &e);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) return S_OK;
        if (FAILED(hr)) return hr;
        MediaEventType t = MEUnknown; e->GetType(&t);
        HRESULT st = S_OK; e->GetStatus(&st);
        if (FAILED(st)) return st;
        if (t == METransformNeedInput) { ++needInput_; hr = submitQueued(); if (FAILED(hr)) return hr; }
        else if (t == METransformHaveOutput) { hr = processOutput(out); if (FAILED(hr)) return hr; }
        else if (t == METransformDrainComplete) drained_ = true;
    }
}

HRESULT HwEncoder::encode(IMFSample* s, const OutputFn& out) {
    if (sync_) {
        HRESULT hr = mft_->ProcessInput(inId_, s, 0);
        if (hr == MF_E_NOTACCEPTING) { hr = pump(out); if (SUCCEEDED(hr)) hr = mft_->ProcessInput(inId_, s, 0); }
        if (FAILED(hr)) return hr;
        ++inputs;
        return pump(out);
    }
    // Queue a few frames instead of dropping when the MFT hasn't asked for input yet; only drop past 4 queued.
    HRESULT result = S_OK;
    queue_.emplace_back(s);
    if (queue_.size() > 4) { queue_.erase(queue_.begin()); ++backpressureDrops; result = S_FALSE; }
    HRESULT hr = pump(out);
    if (FAILED(hr)) return hr;
    return result;
}

HRESULT HwEncoder::submitQueued() {
    while (needInput_ > 0 && !queue_.empty()) {
        HRESULT hr = mft_->ProcessInput(inId_, queue_.front().Get(), 0);
        if (hr == MF_E_NOTACCEPTING) return S_OK;
        if (FAILED(hr)) return hr;
        queue_.erase(queue_.begin());
        --needInput_;
        ++inputs;
    }
    return S_OK;
}

bool HwEncoder::drain(const OutputFn& out, DWORD timeoutMs) {
    if (!mft_) return true;
    if (!sync_) {  // feed what's still queued first
        ULONGLONG qEnd = GetTickCount64() + timeoutMs / 2;
        while (!queue_.empty() && GetTickCount64() < qEnd) { if (FAILED(pump(out))) return false; if (!queue_.empty()) Sleep(1); }
    }
    queue_.clear();
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0))) return false;
    if (sync_) return SUCCEEDED(pump(out));
    ULONGLONG end = GetTickCount64() + timeoutMs;
    while (!drained_ && GetTickCount64() < end) {
        if (FAILED(pump(out))) return false;
        if (!drained_) Sleep(2);
    }
    return drained_;
}

bool probeHwEncoder(ID3D11Device* dev, const HwEncoderInfo& info, UINT w, UINT h, int fps, DWORD timeoutMs,
                    std::wstring& log) {
    // Private device on the same adapter so a stalled probe can be leaked without touching the recording device.
    ComPtr<IDXGIDevice> dd; ComPtr<IDXGIAdapter> ad;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dd))) || FAILED(dd->GetAdapter(&ad))) { log += L"probe: no adapter\n"; return false; }
    struct St { std::atomic<int> state{0}; std::wstring log; bool ok = false; };  // 0 running, 1 done, 2 abandoned
    auto st = std::make_shared<St>();
    ComPtr<IDXGIAdapter> adRef = ad;
    std::thread t([st, adRef, info, w, h, fps]() mutable {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<ID3D11Device> pdev; ComPtr<ID3D11DeviceContext> pctx;
        HRESULT hr = D3D11CreateDevice(adRef.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                       D3D11_SDK_VERSION, &pdev, nullptr, &pctx);
        std::wstring lg;
        logHr(lg, L"probe D3D11CreateDevice", hr);
        bool ok = false;
        auto enc = std::make_unique<HwEncoder>();
        if (SUCCEEDED(hr)) {
            ComPtr<ID3D10Multithread> mt;
            if (SUCCEEDED(pdev.As(&mt))) mt->SetMultithreadProtected(TRUE);
            if (enc->open(pdev.Get(), info, w, h, fps, 4000000, false, lg)) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_NV12;
                td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                ComPtr<ID3D11Texture2D> tex;
                hr = pdev->CreateTexture2D(&td, nullptr, &tex);
                if (FAILED(hr)) { td.BindFlags = D3D11_BIND_RENDER_TARGET; hr = pdev->CreateTexture2D(&td, nullptr, &tex); }
                logHr(lg, L"probe CreateTexture2D(NV12)", hr);
                const LONGLONG dur = 10000000LL / fps;
                int sent = 0;
                ULONGLONG end = GetTickCount64() + 1500;
                auto sink = [&](IMFSample*) { return S_OK; };
                while (SUCCEEDED(hr) && enc->outputs == 0 && GetTickCount64() < end && st->state.load() == 0) {
                    if (sent < 30) {
                        ComPtr<IMFMediaBuffer> b; ComPtr<IMFSample> s;
                        hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex.Get(), 0, FALSE, &b);
                        if (SUCCEEDED(hr)) { ComPtr<IMF2DBuffer> b2; DWORD len = 0; if (SUCCEEDED(b.As(&b2))) b2->GetContiguousLength(&len); b->SetCurrentLength(len); }
                        if (SUCCEEDED(hr)) hr = MFCreateSample(&s);
                        if (SUCCEEDED(hr)) { s->AddBuffer(b.Get()); s->SetSampleTime(sent * dur); s->SetSampleDuration(dur); }
                        if (SUCCEEDED(hr)) { hr = enc->encode(s.Get(), sink); if (hr == S_OK) ++sent; else if (hr == S_FALSE) hr = S_OK; }
                    } else hr = enc->pump(sink);
                    Sleep(5);
                }
                if (FAILED(hr)) logHr(lg, L"probe encode", hr);
                wchar_t b[128];
                swprintf(b, 128, L"probe: %d frame(s) in, %llu out\n", sent, enc->outputs);
                lg += b;
                ok = SUCCEEDED(hr) && enc->outputs > 0;
            }
        }
        int expect = 0;
        st->log = lg; st->ok = ok;
        if (st->state.compare_exchange_strong(expect, 1)) {
            enc->release();
            enc.reset();
            if (SUCCEEDED(co)) CoUninitialize();
        } else {
            enc->leak(); enc.release();  // caller gave up on us: never touch the MFT again
            pdev.Detach(); pctx.Detach();
        }
    });
    ULONGLONG end = GetTickCount64() + timeoutMs;
    while (st->state.load() == 0 && GetTickCount64() < end) Sleep(10);
    int expect = 0;
    if (st->state.compare_exchange_strong(expect, 2)) {
        t.detach();
        log += L"probe: TIMED OUT (encoder stalled), leaked and skipped\n";
        return false;
    }
    t.join();
    log += st->log;
    return st->ok;
}

}  // namespace cr
