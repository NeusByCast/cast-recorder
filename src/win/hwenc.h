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
    GUID subtype = MFVideoFormat_H264;  // H.264, HEVC or AV1 output
    bool argb = false;     // feed BGRA (MFVideoFormat_ARGB32) surfaces instead of NV12 (VideoProcessor unavailable)
    int cfg = 0;           // EncCfg: MFT setup variant that passed the probe
};

// MFT setup variants, tried in this order by the probe; the first that sustains real-time output is used.
// Some AMD iGPU MFTs take one D3D11 frame and never ask for another with the default setup.
enum EncCfg : int {
    kCfgDefault = 0,     // peak-constrained VBR, quality preset, LowLatencyMode off, D3D11 surfaces
    kCfgLowLatency = 1,  // CBR + LowLatencyMode on + MF_LOW_LATENCY, D3D11 surfaces
    kCfgPlain = 2,       // only the bitrate set (driver defaults), no MF_LOW_LATENCY, D3D11 surfaces
    kCfgSysMem = 3,      // driver defaults + bitrate, no D3D manager: CPU NV12 buffers (GPU convert + readback)
    kCfgCount = 4
};
const wchar_t* encCfgName(int cfg);

// Hardware H.264 encoders registered for the adapter that owns `dev` (MFT_ENUM_ADAPTER_LUID).
std::vector<HwEncoderInfo> hwEncodersForDevice(ID3D11Device* dev, std::wstring& log, const GUID& subtype = MFVideoFormat_H264);
// What this machine can encode: hardware encoders on the adapter with this LUID (MFTEnum2) per codec, plus the
// software MFTs. Labels: "H.264 · NVIDIA", "HEVC · Software" ...
struct CodecSupport { GUID subtype; std::wstring codec, vendor; bool hardware; };
std::vector<CodecSupport> probeCodecSupport(const LUID* adapter);  // nullptr = all hardware adapters
std::vector<LUID> hardwareAdapterLuids();
const wchar_t* codecName(const GUID& subtype);

class HwEncoder {
public:
    using OutputFn = std::function<HRESULT(IMFSample*)>;
    ~HwEncoder();
    // Creates and configures the MFT. Order: D3D manager -> output type -> input type -> codec api -> streaming.
    bool open(ID3D11Device* dev, const HwEncoderInfo& info, UINT w, UINT h, int fps, unsigned bitrate, bool high,
              std::wstring& log);
    // Microsoft software H.264 MFT (synchronous), same driver + same pass-through MP4 writer as hardware.
    bool openSoftware(UINT w, UINT h, int fps, unsigned bitrate, bool high, std::wstring& log, const GUID& subtype = MFVideoFormat_H264);
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
    // Diagnostics (probe log): async events seen, raw outputs (before the wait-for-keyframe gate), eager inputs.
    unsigned long long evNeedInput = 0, evHaveOutput = 0, evOther = 0, rawOutputs = 0, notAccepting = 0, eagerInputs = 0, dropsBeforeKey = 0;
    HRESULT lastError = S_OK;
    std::wstring lastErrorAt;
    // Async MFTs that stop posting METransformNeedInput (seen with AMD AMF after the first frame): offer the oldest
    // queued frame directly. MF_E_NOTACCEPTING is harmless and keeps it queued.
    HRESULT tryEagerInput();
    // Stall rescue: ask for output without a METransformHaveOutput event (some drivers stop posting it).
    // Errors (E_UNEXPECTED / NEED_MORE_INPUT) are expected and ignored.
    HRESULT pollOutput(const OutputFn& out);
    unsigned long long polledOutputs = 0, injectedHeaders = 0;
    bool sysmem() const { return cfg_ == kCfgSysMem; }
    int cfg() const { return cfg_; }
    bool testStall = false;  // CI: behave like a stalled hardware MFT (swallow input, never output)
    bool drainedFlag() const { return drained_; }
    size_t queued() const { return queue_.size(); }
private:
    bool configure(UINT w, UINT h, int fps, unsigned bitrate, bool high, std::wstring& log);
    HRESULT processOutput(const OutputFn& out);
    HRESULT deliver(IMFSample* s, const OutputFn& out);
    std::vector<Microsoft::WRL::ComPtr<IMFSample>> queue_;  // waiting for METransformNeedInput (max 4)
    HRESULT submitQueued();
    GUID sub_ = MFVideoFormat_H264;
    bool sync_ = false;
    bool argb_ = false;
    int cfg_ = kCfgDefault;
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

// Pre-flight probe of one setup variant (info.cfg): opens the encoder on `dev`'s adapter (private device) and
// feeds it like a recording does - distinct NV12 surfaces copied into a 16-surface ring (or CPU NV12 buffers for
// kCfgSysMem), paced at `fps`, through the same queue/event driver. Pass = at least min(fps, 60) encoded frames
// (one second of video) within 1.8 s; an encoder that stops producing output mid-way fails. Runs on a helper
// thread; a failed MFT is leaked (never touched again), never blocks the caller beyond timeoutMs.
// The probe always runs at kProbeW x kProbeH so the verdict doesn't depend on the source (a tiny window).
constexpr UINT kProbeW = 1920, kProbeH = 1080;
// Runs on a helper thread; a stalled MFT is leaked (never touched again), never blocks the caller.
bool probeHwEncoder(ID3D11Device* dev, const HwEncoderInfo& info, UINT w, UINT h, int fps, DWORD timeoutMs,
                    std::wstring& log);

}  // namespace cr
