#pragma once
// Explicit hardware H.264 encoder MFT driver (async MFT event model), bound to one D3D11 device's adapter.
#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <functional>
#include <string>
#include <vector>
#include <codecapi.h>

namespace cr {

struct HwEncoderInfo {
    std::wstring vendor;   // MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, e.g. VEN_1002
    std::wstring name;     // friendly name
    UINT pciVendor = 0;
    int index = 0;         // ordinal in the LUID-filtered enumeration
};

// Hardware H.264 encoders registered for the adapter that owns `dev` (MFT_ENUM_ADAPTER_LUID).
std::vector<HwEncoderInfo> hwEncodersForDevice(ID3D11Device* dev, std::wstring& log);

class HwEncoder {
public:
    using OutputFn = std::function<HRESULT(IMFSample*)>;
    ~HwEncoder();
    // Creates and configures the MFT. Order: D3D manager -> output type -> input type -> codec api -> streaming.
    bool open(ID3D11Device* dev, const HwEncoderInfo& info, UINT w, UINT h, int fps, unsigned bitrate, bool high,
              std::wstring& log);
    // Microsoft software H.264 MFT (synchronous), same driver + same pass-through MP4 writer as hardware.
    bool openSoftware(UINT w, UINT h, int fps, unsigned bitrate, bool high, std::wstring& log);
    // Non-blocking: pumps events, submits the sample if the MFT wants input. Returns S_FALSE if the frame was
    // dropped because the encoder hasn't asked for input (counts as backpressure, never blocks).
    HRESULT encode(IMFSample* s, const OutputFn& out);
    // Pumps pending events (HaveOutput) without submitting.
    HRESULT pump(const OutputFn& out);
    // Drain with a timeout. Returns false if the MFT didn't complete the drain in time (stalled).
    bool drain(const OutputFn& out, DWORD timeoutMs);
    IMFMediaType* outputType() { return outType_.Get(); }
    // Output type as the MFT currently reports it (may carry MF_MT_MPEG_SEQUENCE_HEADER after the first frame).
    Microsoft::WRL::ComPtr<IMFMediaType> currentOutputType();
    Microsoft::WRL::ComPtr<IMFMediaType> muxType();
    void release();  // shut down normally
    void leak();     // abandon a stalled MFT without touching it again
    std::wstring label;
    unsigned long long inputs = 0, outputs = 0, backpressureDrops = 0;
private:
    bool configure(UINT w, UINT h, int fps, unsigned bitrate, bool high, std::wstring& log);
    HRESULT processOutput(const OutputFn& out);
    HRESULT deliver(IMFSample* s, const OutputFn& out);
    std::vector<Microsoft::WRL::ComPtr<IMFSample>> queue_;  // waiting for METransformNeedInput (max 4)
    HRESULT submitQueued();
    bool sync_ = false;
    bool gotKey_ = false;
    std::vector<BYTE> seqHeader_;
    UINT w_ = 0, h_ = 0; int fps_ = 30; unsigned br_ = 0; UINT32 profile_ = 77;
    LONGLONG nextTime_ = 0;
    Microsoft::WRL::ComPtr<IMFTransform> mft_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> ev_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> mgr_;
    Microsoft::WRL::ComPtr<IMFMediaType> outType_;
    Microsoft::WRL::ComPtr<IMFActivate> act_;
    DWORD inId_ = 0, outId_ = 0;
    int needInput_ = 0;
    bool drained_ = false;
    bool providesSamples_ = false;
    DWORD outBufSize_ = 0;
};

// Pre-flight probe: opens the encoder on `dev`, feeds ~12 frames and requires output within timeoutMs.
// Runs on a helper thread; a stalled MFT is leaked (never touched again), never blocks the caller.
bool probeHwEncoder(ID3D11Device* dev, const HwEncoderInfo& info, UINT w, UINT h, int fps, DWORD timeoutMs,
                    std::wstring& log);

}  // namespace cr
