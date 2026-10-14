#include "audio.h"
#include "micfx.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propkey.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <climits>
#include <deque>

using Microsoft::WRL::ComPtr;

namespace cr {
namespace {
const CLSID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4c32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const PROPERTYKEY kPKEY_FriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const IID kIID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
const IID kIID_IAudioCaptureClient = {0xC8ADBD64, 0xE71E, 0x48a0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
void logHr(std::wstring& log, const wchar_t* what, HRESULT hr) {
    wchar_t b[200]; swprintf(b, 200, L"%ls: 0x%08lX%ls\n", what, (unsigned long)hr, SUCCEEDED(hr) ? L" ok" : L" FAILED"); log += b;
}
ComPtr<IMMDeviceEnumerator> enumerator() {
    ComPtr<IMMDeviceEnumerator> e;
    CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)e.GetAddressOf());
    return e;
}
const IID kIID_IMMNotificationClient = {0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};
const IID kIID_IActivateHandler = {0x41D949AB, 0x9862, 0x444A, {0x80, 0xF6, 0xC2, 0x61, 0x33, 0x4D, 0xA5, 0xEB}};
const IID kIID_IAgileObject = {0x94EA2B94, 0xE9CC, 0x49E0, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};
const IID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const HRESULT kDEVICE_INVALIDATED = (HRESULT)0x88890004;

// audioclientactivationparams.h (not in every SDK/MinGW)
struct ProcLoopbackParams { DWORD TargetProcessId; int ProcessLoopbackMode; };  // 1 = EXCLUDE_TARGET_PROCESS_TREE
struct ActivationParams { int ActivationType; ProcLoopbackParams p; };            // 1 = PROCESS_LOOPBACK

DWORD winBuild() {
    typedef LONG(WINAPI * RtlGetVersionT)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{sizeof v};
    if (auto f = (RtlGetVersionT)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) f(&v);
    return v.dwBuildNumber;
}

struct ActivateHandler : IActivateAudioInterfaceCompletionHandler {
    std::atomic<ULONG> ref{1};
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~ActivateHandler() { CloseHandle(done); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
        if (IsEqualIID(r, kIID_IUnknown) || IsEqualIID(r, kIID_IActivateHandler) || IsEqualIID(r, kIID_IAgileObject)) { *p = this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = --ref; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override { SetEvent(done); return S_OK; }
};

// Process loopback of everything EXCEPT this process tree: our start/stop/pause tones never reach the file.
HRESULT activateProcessLoopback(ComPtr<IAudioClient>& out) {
    if (winBuild() < 19041) return E_NOTIMPL;
    typedef HRESULT(WINAPI * ActT)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*, IActivateAudioInterfaceAsyncOperation**);
    HMODULE m = LoadLibraryExW(L"mmdevapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto act = m ? (ActT)GetProcAddress(m, "ActivateAudioInterfaceAsync") : nullptr;
    if (!act) return E_NOTIMPL;
    ActivationParams ap{1, {GetCurrentProcessId(), 1}};
    PROPVARIANT pv; PropVariantInit(&pv);
    pv.vt = VT_BLOB; pv.blob.cbSize = sizeof ap; pv.blob.pBlobData = (BYTE*)&ap;
    ActivateHandler* h = new ActivateHandler;
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = act(L"VAD\\Process_Loopback", kIID_IAudioClient, &pv, h, &op);
    if (SUCCEEDED(hr)) {
        if (WaitForSingleObject(h->done, 3000) != WAIT_OBJECT_0) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        else {
            HRESULT ar = E_FAIL; ComPtr<IUnknown> unk;
            hr = op->GetActivateResult(&ar, &unk);
            if (SUCCEEDED(hr)) hr = ar;
            if (SUCCEEDED(hr)) hr = unk->QueryInterface(kIID_IAudioClient, (void**)out.ReleaseAndGetAddressOf());
        }
    }
    h->Release();
    return hr;
}
}  // namespace

void AudioMixer::Shared::event(const std::wstring& e) {
    std::lock_guard<std::mutex> g(evM);
    if (events.size() < 4000) events += e + L"; ";
}

AudioFx& audioFx() { static AudioFx f; return f; }

// gain -> noise suppression -> gate on a mono downmix; push-to-talk / mute silence. Returns the output peak.
struct MicChain {
    MicFx fx;
    std::vector<float> mono;
    float run(float* st, size_t n) {
        auto& a = audioFx();
        MicFxParams p; p.gain = a.micGain.load(); p.noiseSuppression = a.noiseSuppression.load(); p.gateDb = a.gateDb.load();
        mono.resize(n);
        for (size_t i = 0; i < n; ++i) mono[i] = 0.5f * (st[i * 2] + st[i * 2 + 1]);
        fx.process(mono.data(), n, p);
        bool silent = a.micMuted.load() || (a.pushToTalk.load() && !a.pttDown.load());
        float pk = 0;
        for (size_t i = 0; i < n; ++i) {
            float v = silent ? 0.f : mono[i];
            st[i * 2] = st[i * 2 + 1] = v;
            float av = v < 0 ? -v : v; if (av > pk) pk = av;
        }
        return pk;
    }
};
static void peakTo(std::atomic<float>& a, float v) { float c = a.load(); while (v > c && !a.compare_exchange_weak(c, v)) {} }

// Plays the processed microphone on the default output (monitoring). Drops audio rather than adding latency.
struct MonitorOut {
    ComPtr<IAudioClient> client; ComPtr<IAudioRenderClient> rc; UINT32 cap = 0; bool failed = false;
    bool open() {
        auto e = enumerator(); ComPtr<IMMDevice> d;
        if (!e || FAILED(e->GetDefaultAudioEndpoint(eRender, eConsole, &d))) return false;
        if (FAILED(d->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)client.GetAddressOf()))) return false;
        WAVEFORMATEX wf{}; wf.wFormatTag = 3; wf.nChannels = 2; wf.nSamplesPerSec = 48000; wf.wBitsPerSample = 32; wf.nBlockAlign = 8; wf.nAvgBytesPerSec = 48000 * 8;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 600000, 0, &wf, nullptr))) return false;
        if (FAILED(client->GetBufferSize(&cap)) || FAILED(client->GetService(kIID_IAudioRenderClient, (void**)rc.GetAddressOf()))) return false;
        return SUCCEEDED(client->Start());
    }
    void write(const float* st, UINT32 n) {
        if (failed) return;
        if (!rc && !open()) { failed = true; rc.Reset(); client.Reset(); return; }
        UINT32 pad = 0; if (FAILED(client->GetCurrentPadding(&pad))) { rc.Reset(); client.Reset(); return; }
        UINT32 room = cap - pad; if (pad > 4800) return;  // keep latency <= 100 ms
        if (n > room) n = room;
        BYTE* b = nullptr; if (!n || FAILED(rc->GetBuffer(n, &b))) return;
        memcpy(b, st, (size_t)n * 8); rc->ReleaseBuffer(n, 0);
    }
    void stop() { if (client) client->Stop(); rc.Reset(); client.Reset(); }
};

static std::vector<AudioDevice> listEndpoints(EDataFlow flow) {
    std::vector<AudioDevice> r;
    auto e = enumerator();
    ComPtr<IMMDeviceCollection> c;
    if (!e || FAILED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &c))) return r;
    UINT n = 0; c->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d; if (FAILED(c->Item(i, &d))) continue;
        LPWSTR id = nullptr; d->GetId(&id);
        AudioDevice a; if (id) { a.id = id; CoTaskMemFree(id); }
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kPKEY_FriendlyName, &v)) && v.vt == VT_LPWSTR) a.name = v.pwszVal;
            PropVariantClear(&v);
        }
        if (a.name.empty()) a.name = flow == eCapture ? L"Microphone" : L"Speakers";
        r.push_back(a);
    }
    return r;
}
std::vector<AudioDevice> listMicrophones() { return listEndpoints(eCapture); }
std::vector<AudioDevice> listOutputDevices() { return listEndpoints(eRender); }

// One capture stream (48 kHz stereo float via the engine's converter) placed on the media timeline by QPC.
struct AudioSource {
    bool isMic = false, loopback = false, procLoopback = false, pcm16 = false, allowMonitor = false;
    MicChain chain;
    MonitorOut mon;
    std::wstring id, name, devName;
    std::shared_ptr<AudioMixer::Shared> sh;
    std::mutex m;
    std::deque<float> buf;          // interleaved stereo; buf[0] is media frame bufStart
    long long bufStart = 0, writePos = LLONG_MIN;
    double ratio = 1.0, errS = 0, rpos = 0;  // resampler state: rpos is relative to prev (-1)
    float prevL = 0, prevR = 0;
    unsigned long long received = 0, gapPadded = 0, trimmed = 0, beforeZero = 0, resyncs = 0, reconnects = 0, qpcMissing = 0;
    double maxDriftPpm = 0;
    std::atomic<bool> quit{false}, reopen{false};
    std::thread th;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> cap;
    HANDLE ev = nullptr;
    LARGE_INTEGER qf{};

    bool openOnce(std::wstring& log) {
        cap.Reset(); client.Reset();
        HRESULT hr = E_FAIL;
        procLoopback = false;
        if (loopback && id.empty()) {
            hr = activateProcessLoopback(client);
            logHr(log, L"audio: process loopback (excluding Cast Recorder)", hr);
            if (SUCCEEDED(hr)) { procLoopback = true; devName = L"process loopback"; }
        }
        if (!procLoopback) {
            auto e = enumerator();
            if (!e) { log += L"audio: no MMDeviceEnumerator\n"; return false; }
            ComPtr<IMMDevice> dev;
            hr = id.empty() ? e->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, loopback ? eConsole : eCommunications, &dev)
                            : e->GetDevice(id.c_str(), &dev);
            logHr(log, loopback ? L"audio: default output device" : L"audio: microphone device", hr);
            if (FAILED(hr)) return false;
            ComPtr<IPropertyStore> ps;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(kPKEY_FriendlyName, &v)) && v.vt == VT_LPWSTR) devName = v.pwszVal;
                PropVariantClear(&v);
            }
            hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)client.GetAddressOf());
            if (FAILED(hr)) { logHr(log, L"audio: Activate", hr); return false; }
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            pcm16 = attempt == 1;
            WAVEFORMATEX wf{};
            wf.wFormatTag = pcm16 ? WAVE_FORMAT_PCM : 3 /* IEEE_FLOAT */; wf.nChannels = 2; wf.nSamplesPerSec = AudioMixer::kRate;
            wf.wBitsPerSample = pcm16 ? 16 : 32; wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8; wf.nAvgBytesPerSec = AudioMixer::kRate * wf.nBlockAlign;
            DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                          (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000 /* 200 ms */, 0, &wf, nullptr);
            logHr(log, pcm16 ? L"audio: Initialize(48k stereo s16)" : L"audio: Initialize(48k stereo float)", hr);
            if (SUCCEEDED(hr)) break;
            if (!procLoopback) break;
            // a failed Initialize leaves the client unusable: activate again for the second format
            if (FAILED(activateProcessLoopback(client))) break;
        }
        if (FAILED(hr)) return false;
        if (!ev) ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = client->SetEventHandle(ev);
        if (SUCCEEDED(hr)) hr = client->GetService(kIID_IAudioCaptureClient, (void**)cap.GetAddressOf());
        if (SUCCEEDED(hr)) hr = client->Start();
        logHr(log, L"audio: Start", hr);
        if (FAILED(hr)) { cap.Reset(); client.Reset(); return false; }
        writePos = LLONG_MIN;  // resync on the next packet's QPC time
        return true;
    }
    bool open(std::wstring& log) {
        QueryPerformanceFrequency(&qf);
        if (!openOnce(log)) return false;
        log += L"audio: " + name + L" -> " + devName + L"\n";
        th = std::thread([this] { loop(); });
        return true;
    }
    void reconnect(const wchar_t* why, HRESULT hr) {
        wchar_t b[160]; swprintf(b, 160, L"%ls: %ls (0x%08lX), reconnecting", name.c_str(), why, (unsigned long)hr);
        sh->event(b);
        if (client) client->Stop();
        cap.Reset(); client.Reset();
        while (!quit) {
            std::wstring l;
            if (openOnce(l)) { ++reconnects; sh->event(name + L": reconnected to " + devName); return; }
            for (int i = 0; i < 10 && !quit; ++i) Sleep(50);
        }
    }
    // Append n input frames (stereo) whose first frame was captured at QPC time t100 (100 ns).
    void place(const float* in, UINT32 n, long long t100) {
        std::lock_guard<std::mutex> g(m);
        received += n;
        const long long origin = sh->origin100ns.load();
        if (!origin || sh->paused.load()) { writePos = LLONG_MIN; return; }
        long long expected = (t100 - origin) * AudioMixer::kRate / 10000000LL;
        UINT32 skip = 0;
        if (expected + (long long)n <= 0) { beforeZero += n; writePos = LLONG_MIN; return; }  // before video zero
        if (writePos == LLONG_MIN) {
            if (expected < 0) { skip = (UINT32)(-expected); beforeZero += skip; expected = 0; }
            writePos = expected; errS = 0; ratio = 1.0; rpos = 0;
            if (buf.empty()) bufStart = writePos;
            else if (bufStart + (long long)buf.size() / 2 < writePos) { gapPadded += writePos - (bufStart + buf.size() / 2); buf.resize((size_t)(writePos - bufStart) * 2, 0.f); }
            else if (bufStart + (long long)buf.size() / 2 > writePos) { long long keep = writePos - bufStart; if (keep < 0) { buf.clear(); bufStart = writePos; } else buf.resize((size_t)keep * 2); }
            prevL = in[skip * 2]; prevR = in[skip * 2 + 1];
        }
        long long err = expected - writePos;
        if (err > 960 || err < -960) {  // > 20 ms off: discontinuity (silent loopback gap, glitch, reconnect)
            ++resyncs;
            if (err > 0) { gapPadded += err; buf.resize(buf.size() + (size_t)err * 2, 0.f); writePos += err; }
            else { UINT32 d = (UINT32)((-err) < (long long)n ? -err : n); trimmed += d; skip += d; if (skip >= n) return; }
            errS = 0; ratio = 1.0; rpos = 0; prevL = in[skip * 2]; prevR = in[skip * 2 + 1];
        } else {
            // drift: steer the resampling ratio so the device clock tracks QPC (max +-0.5 %)
            errS = errS * 0.9 + (double)err * 0.1;
            double adj = errS / 48000.0 * 0.5;
            if (adj > 0.005) adj = 0.005; if (adj < -0.005) adj = -0.005;
            ratio = 1.0 + adj;
            double ppm = (ratio - 1.0) * 1e6; if (ppm < 0) ppm = -ppm; if (ppm > maxDriftPpm) maxDriftPpm = ppm;
        }
        // linear-interpolating resampler, output step 1/ratio input frames
        const float* p = in + skip * 2; const UINT32 cnt = n - skip;
        const double step = 1.0 / ratio;
        long long outN = 0;
        while (rpos < (double)cnt - 1.0) {
            long long i = (long long)std::floor(rpos);
            double t = rpos - (double)i;
            float l0 = i < 0 ? prevL : p[i * 2], r0 = i < 0 ? prevR : p[i * 2 + 1];
            float l1 = p[(i + 1) * 2], r1 = p[(i + 1) * 2 + 1];
            buf.push_back(l0 + (float)((l1 - l0) * t));
            buf.push_back(r0 + (float)((r1 - r0) * t));
            ++outN; rpos += step;
        }
        rpos -= (double)cnt;
        prevL = p[(cnt - 1) * 2]; prevR = p[(cnt - 1) * 2 + 1];
        writePos += outN;
        // bound memory: keep <= 3 s
        const size_t maxS = (size_t)AudioMixer::kRate * 3 * 2;
        if (buf.size() > maxS) { size_t d = buf.size() - maxS; buf.erase(buf.begin(), buf.begin() + d); bufStart += d / 2; }
    }
    void loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<float> conv;
        while (!quit) {
            if (reopen.exchange(false)) reconnect(L"default device changed", S_OK);
            if (!cap) { reconnect(L"no stream", E_FAIL); continue; }
            WaitForSingleObject(ev, 10);
            UINT32 pkt = 0;
            HRESULT hr;
            while (SUCCEEDED(hr = cap->GetNextPacketSize(&pkt)) && pkt) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD fl = 0; UINT64 devPos = 0, qpc = 0;
                hr = cap->GetBuffer(&data, &frames, &fl, &devPos, &qpc);
                if (FAILED(hr)) break;
                if (hr == AUDCLNT_S_BUFFER_EMPTY || !frames) { cap->ReleaseBuffer(frames); continue; }
                if (!qpc) {  // no device timestamp: estimate from arrival
                    ++qpcMissing;
                    LARGE_INTEGER q; QueryPerformanceCounter(&q);
                    qpc = (UINT64)(q.QuadPart / qf.QuadPart * 10000000LL + q.QuadPart % qf.QuadPart * 10000000LL / qf.QuadPart) - (UINT64)frames * 10000000ULL / AudioMixer::kRate;
                }
                conv.resize((size_t)frames * 2);
                if (fl & AUDCLNT_BUFFERFLAGS_SILENT) std::fill(conv.begin(), conv.end(), 0.f);
                else if (pcm16) { const short* s16 = (const short*)data; for (size_t i = 0; i < conv.size(); ++i) conv[i] = s16[i] / 32768.f; }
                else memcpy(conv.data(), data, conv.size() * sizeof(float));
                cap->ReleaseBuffer(frames);
                if (isMic) {
                    peakTo(audioFx().levelMic, chain.run(conv.data(), frames));
                    // the noise suppressor delays its output: those samples were captured that much earlier
                    qpc -= (UINT64)chain.fx.delay() * 10000000ULL / AudioMixer::kRate;
                    if (allowMonitor && audioFx().monitoring.load()) mon.write(conv.data(), frames);
                    else if (mon.client) mon.stop();
                } else {
                    float pk = 0; for (float v : conv) { float a = v < 0 ? -v : v; if (a > pk) pk = a; }
                    peakTo(audioFx().levelSys, audioFx().systemMuted.load() ? 0.f : pk);
                }
                place(conv.data(), frames, (long long)qpc);
            }
            if (FAILED(hr)) reconnect(hr == kDEVICE_INVALIDATED ? L"device lost" : L"capture error", hr);
        }
        CoUninitialize();
    }
    // Fill dst with media frames [from, from+n); returns frames that had real data.
    size_t pull(long long from, float* dst, size_t n) {
        std::lock_guard<std::mutex> g(m);
        std::fill(dst, dst + n * 2, 0.f);
        if (from > bufStart && !buf.empty()) {  // drop what's already been mixed
            long long d = from - bufStart; size_t ds = (size_t)(d * 2 < (long long)buf.size() ? d * 2 : buf.size());
            buf.erase(buf.begin(), buf.begin() + ds); bufStart += ds / 2;
        }
        if (buf.empty()) { if (bufStart < from) bufStart = from; return 0; }
        size_t got = 0;
        long long off = bufStart - from;  // >=0
        for (size_t i = 0; i < n; ++i) {
            long long k = (long long)i - off;
            if (k < 0 || (size_t)k * 2 + 1 >= buf.size()) continue;
            dst[i * 2] = buf[(size_t)k * 2]; dst[i * 2 + 1] = buf[(size_t)k * 2 + 1]; ++got;
        }
        return got;
    }
    ~AudioSource() {
        quit = true;
        if (ev) SetEvent(ev);
        if (th.joinable()) th.join();
        if (client) client->Stop();
        mon.stop();
        if (ev) CloseHandle(ev);
    }
};

// Default output/microphone changed (or a device went away): reconnect the affected default-following source.
struct DeviceNotifier : IMMNotificationClient {
    std::atomic<ULONG> ref{1};
    std::mutex m;
    std::vector<AudioSource*> srcs;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
        if (IsEqualIID(r, kIID_IUnknown) || IsEqualIID(r, kIID_IMMNotificationClient)) { *p = this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = --ref; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        std::lock_guard<std::mutex> g(m);
        for (auto* s : srcs) {
            if (!s->id.empty() || s->procLoopback) continue;  // explicit device / process loopback follow by themselves
            if ((s->loopback && flow == eRender && role == eConsole) || (!s->loopback && flow == eCapture && role == eCommunications)) s->reopen = true;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }  // loss surfaces as DEVICE_INVALIDATED
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

AudioMixer::AudioMixer() : sh_(std::make_shared<Shared>()) {}
AudioMixer::~AudioMixer() { stop(); }

bool AudioMixer::start(bool system, bool mic, const std::wstring& micId, bool testTone, std::wstring& log,
                       const std::wstring& sysId, bool testMic, bool allowMonitor) {
    if (system) {
        auto s = std::make_unique<AudioSource>(); s->name = L"system"; s->loopback = true; s->sh = sh_; s->id = sysId;
        if (s->open(log)) sources_.push_back(std::move(s)); else log += L"audio: system audio unavailable\n";
    }
    if (mic) {
        auto s = std::make_unique<AudioSource>(); s->name = L"mic"; s->isMic = true; s->id = micId; s->sh = sh_; s->allowMonitor = allowMonitor;
        if (s->open(log)) sources_.push_back(std::move(s)); else log += L"audio: microphone unavailable\n";
    }
    if (!sources_.empty()) {
        if (auto e = enumerator()) {
            notifier_ = new DeviceNotifier;
            for (auto& s : sources_) notifier_->srcs.push_back(s.get());
            if (FAILED(e->RegisterEndpointNotificationCallback(notifier_))) { notifier_->Release(); notifier_ = nullptr; }
        }
    }
    tone_ = testTone;
    testMic_ = testMic;
    if (testMic) { testChain_ = std::make_unique<MicChain>(); log += L"audio: synthetic test microphone\n"; }
    pos_ = 0;
    return active();
}

void AudioMixer::setOrigin(long long q, long long f) {
    sh_->origin100ns = q / f * 10000000LL + q % f * 10000000LL / f;
}
void AudioMixer::setPaused(bool p) { sh_->paused = p; }
bool AudioMixer::systemExcludesSelf() const {
    for (auto& s : sources_) if (s->loopback) return s->procLoopback;
    return true;
}
void AudioMixer::addMute(long long from, long long to, bool sys) { std::lock_guard<std::mutex> g(muteM_); mutes_.push_back({from, to, sys}); }

std::wstring AudioMixer::stats() const {
    std::wstring r;
    wchar_t b[400];
    swprintf(b, 400, L"produced=%llu underrun=%llu tone=%llu; ", produced_, underrunFrames, toneFrames_);
    r += b;
    for (auto& s : sources_) {
        swprintf(b, 400, L"%ls[%ls%ls]: received=%llu before_zero=%llu gap_padded=%llu trimmed=%llu resyncs=%llu reconnects=%llu max_drift_ppm=%.0f qpc_missing=%llu; ",
                 s->name.c_str(), s->devName.c_str(), s->pcm16 ? L", s16" : L"", s->received, s->beforeZero, s->gapPadded, s->trimmed,
                 s->resyncs, s->reconnects, s->maxDriftPpm, s->qpcMissing);
        r += b;
    }
    if (testMic_ && tmNoiseN_) {
        swprintf(b, 400, L"testmic: noise_in_db=%.1f noise_out_db=%.1f ns=%d gate_db=%.0f; ", 10 * std::log10(tmNoiseIn_ / tmNoiseN_ + 1e-20),
                 10 * std::log10(tmNoiseOut_ / tmNoiseN_ + 1e-20), (int)audioFx().noiseSuppression.load(), audioFx().gateDb.load());
        r += b;
    }
    if (sources_.empty() && !tone_ && !testMic_) r += L"no sources; ";
    std::lock_guard<std::mutex> g(sh_->evM);
    if (!sh_->events.empty()) r += L"events: " + sh_->events;
    return r;
}

void AudioMixer::stop() {
    if (notifier_) {
        if (auto e = enumerator()) e->UnregisterEndpointNotificationCallback(notifier_);
        notifier_->Release(); notifier_ = nullptr;
    }
    sources_.clear(); tone_ = false; testMic_ = false;
}

void AudioMixer::produce(long long mediaTime, bool final, std::vector<short>& out, long long& startTime,
                         std::vector<short>* sysOut, std::vector<short>* micOut) {
    out.clear();
    if (sysOut) sysOut->clear();
    if (micOut) micOut->clear();
    // Mix 80 ms behind the video clock so device packets (10-30 ms latency) have been placed.
    if (!final) mediaTime -= 800000;
    long long target = mediaTime > 0 ? mediaTime * kRate / 10000000LL : 0;
    startTime = pos_ * 10000000LL / kRate;
    if (target <= pos_) return;
    size_t n = (size_t)(target - pos_);
    std::vector<float> mix(n * 2, 0.f), tmp(n * 2);
    const bool split = sysOut || micOut;
    std::vector<float> sysMix(split ? n * 2 : 0, 0.f), micMix(split ? n * 2 : 0, 0.f);
    const long long t100 = pos_ * 10000000LL / kRate;
    bool muteSys = final, muteMic = final;
    if (t100 < systemMuteUntil) { muteMic = true; if (!systemExcludesSelf()) muteSys = true; }
    {
        std::lock_guard<std::mutex> g(muteM_);
        for (auto& mu : mutes_) if (t100 >= mu.from && t100 < mu.to) { muteMic = true; if (mu.sys && !systemExcludesSelf()) muteSys = true; }
    }
    for (auto& s : sources_) {
        size_t got = s->pull(pos_, tmp.data(), n);
        if (got < n) underrunFrames += n - got;
        if (s->isMic && (micMuted.load() || muteMic)) continue;
        if (!s->isMic && (muteSys || audioFx().systemMuted.load())) continue;
        for (size_t i = 0; i < n * 2; ++i) mix[i] += tmp[i];
        if (split) { auto& d = s->isMic ? micMix : sysMix; for (size_t i = 0; i < n * 2; ++i) d[i] += tmp[i]; }
    }
    if (tone_) {
        toneFrames_ += n;
        for (size_t i = 0; i < n; ++i) {
            float v = 0.2f * (float)std::sin(phase_);
            phase_ += 2 * 3.14159265358979 * 440.0 / kRate;
            if (phase_ > 6.283185307179586) phase_ -= 6.283185307179586;
            mix[i * 2] += v; mix[i * 2 + 1] += v;
            if (split) { sysMix[i * 2] += v; sysMix[i * 2 + 1] += v; }
        }
    }
    if (testMic_) {
        // noise (-50 dBFS rms) everywhere + 300 Hz tone bursts in [0, 0.5) s of every second
        std::vector<float> m(n * 2);
        std::vector<float> raw(n);
        for (size_t i = 0; i < n; ++i) {
            unsigned long long f = tmFrames_ + i;
            rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL;
            float nz = ((float)((rng_ >> 40) & 0xFFFFFF) / 16777216.f - 0.5f) * 0.011f;  // uniform, rms ~0.0032
            float v = nz;
            if ((f % kRate) < (unsigned long long)kRate / 2) { v += 0.25f * (float)std::sin(micPhase_); }
            micPhase_ += 2 * 3.14159265358979 * 300.0 / kRate; if (micPhase_ > 6.283185307179586) micPhase_ -= 6.283185307179586;
            raw[i] = v; m[i * 2] = m[i * 2 + 1] = v;
        }
        peakTo(audioFx().levelMic, testChain_->run(m.data(), n));
        // noise floor before/after, measured in the middle of the silent half (well past gate hold + NS delay)
        const int d = testChain_->fx.delay();
        for (size_t i = 0; i < n; ++i) {
            unsigned long long f = tmFrames_ + i;
            if (f < (unsigned long long)kRate) continue;  // skip convergence
            unsigned long long ph = f % kRate;
            if (ph >= (unsigned long long)(kRate * 0.75) && ph < (unsigned long long)(kRate * 0.95)) {
                tmNoiseIn_ += (double)raw[i] * raw[i];
                unsigned long long fo = f;  // output frame i is input frame f - d; same window shifted by d
                (void)fo; (void)d;
                tmNoiseOut_ += (double)m[i * 2] * m[i * 2]; ++tmNoiseN_;
            }
        }
        tmFrames_ += n;
        if (!micMuted.load() && !muteMic) for (size_t i = 0; i < n * 2; ++i) { mix[i] += m[i]; if (split) micMix[i] += m[i]; }
    }
    auto toPcm = [&](const std::vector<float>& src, std::vector<short>& dst) {
        if (channels == 1) {
            dst.resize(n);
            for (size_t i = 0; i < n; ++i) {
                float v = 0.5f * (src[i * 2] + src[i * 2 + 1]) * 1.41f;  // ~equal-loudness downmix
                v = v > 1.f ? 1.f : v < -1.f ? -1.f : v;
                dst[i] = (short)std::lrint(v * 32767.f);
            }
        } else {
            dst.resize(n * 2);
            for (size_t i = 0; i < n * 2; ++i) {
                float v = src[i];
                v = v > 1.f ? 1.f : v < -1.f ? -1.f : v;
                dst[i] = (short)std::lrint(v * 32767.f);
            }
        }
    };
    toPcm(mix, out);
    if (sysOut) toPcm(sysMix, *sysOut);
    if (micOut) toPcm(micMix, *micOut);
    produced_ += n;
    pos_ = target;
}

}  // namespace cr
