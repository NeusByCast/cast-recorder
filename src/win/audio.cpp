#include "audio.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propkey.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace cr {
namespace {
const CLSID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4c32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const PROPERTYKEY kPKEY_FriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
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
}  // namespace

std::vector<AudioDevice> listMicrophones() {
    std::vector<AudioDevice> r;
    auto e = enumerator();
    ComPtr<IMMDeviceCollection> c;
    if (!e || FAILED(e->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &c))) return r;
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
        if (a.name.empty()) a.name = L"Microphone";
        r.push_back(a);
    }
    return r;
}

// One WASAPI endpoint, converted by the audio engine to 48 kHz stereo float, buffered in a small FIFO.
struct AudioSource {
    bool isMic = false;
    std::wstring name;
    std::mutex m;
    std::vector<float> fifo;  // interleaved stereo
    size_t head = 0;
    std::atomic<bool> quit{false};
    std::thread th;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> cap;

    bool open(bool loopback, const std::wstring& id, std::wstring& log) {
        auto e = enumerator();
        if (!e) { log += L"audio: no MMDeviceEnumerator\n"; return false; }
        ComPtr<IMMDevice> dev;
        HRESULT hr = id.empty() ? e->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, loopback ? eConsole : eCommunications, &dev)
                                : e->GetDevice(id.c_str(), &dev);
        logHr(log, loopback ? L"audio: default output device" : L"audio: microphone device", hr);
        if (FAILED(hr)) return false;
        hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)client.GetAddressOf());
        if (FAILED(hr)) { logHr(log, L"audio: Activate", hr); return false; }
        WAVEFORMATEX wf{};
        wf.wFormatTag = 3 /* WAVE_FORMAT_IEEE_FLOAT */; wf.nChannels = 2; wf.nSamplesPerSec = AudioMixer::kRate;
        wf.wBitsPerSample = 32; wf.nBlockAlign = 8; wf.nAvgBytesPerSec = AudioMixer::kRate * 8;
        DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000 /* 200 ms */, 0, &wf, nullptr);
        logHr(log, L"audio: Initialize(48k stereo float)", hr);
        if (FAILED(hr)) return false;
        hr = client->GetService(kIID_IAudioCaptureClient, (void**)cap.GetAddressOf());
        if (SUCCEEDED(hr)) hr = client->Start();
        logHr(log, L"audio: Start", hr);
        if (FAILED(hr)) return false;
        th = std::thread([this] { loop(); });
        return true;
    }
    void loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!quit) {
            Sleep(5);
            UINT32 pkt = 0;
            while (SUCCEEDED(cap->GetNextPacketSize(&pkt)) && pkt) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD fl = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &fl, nullptr, nullptr))) break;
                {
                    std::lock_guard<std::mutex> g(m);
                    const float* f = (const float*)data;
                    for (UINT32 i = 0; i < frames * 2; ++i) fifo.push_back((fl & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.f : f[i]);
                    // keep at most ~250 ms (device clock running ahead of ours)
                    size_t avail = fifo.size() - head, maxS = AudioMixer::kRate / 4 * 2;
                    if (avail > maxS) head += avail - maxS;
                    if (head > 1 << 20) { fifo.erase(fifo.begin(), fifo.begin() + head); head = 0; }
                }
                cap->ReleaseBuffer(frames);
            }
        }
        CoUninitialize();
    }
    // Pull up to `frames` stereo frames; returns how many were available.
    size_t pull(float* dst, size_t frames) {
        std::lock_guard<std::mutex> g(m);
        size_t avail = (fifo.size() - head) / 2, n = avail < frames ? avail : frames;
        for (size_t i = 0; i < n * 2; ++i) dst[i] = fifo[head + i];
        head += n * 2;
        return n;
    }
    void clear() { std::lock_guard<std::mutex> g(m); fifo.clear(); head = 0; }
    ~AudioSource() {
        quit = true;
        if (th.joinable()) th.join();
        if (client) client->Stop();
    }
};

AudioMixer::AudioMixer() = default;
AudioMixer::~AudioMixer() { stop(); }

bool AudioMixer::start(bool system, bool mic, const std::wstring& micId, bool testTone, std::wstring& log) {
    if (system) {
        auto s = std::make_unique<AudioSource>(); s->name = L"system";
        if (s->open(true, L"", log)) sources_.push_back(std::move(s)); else log += L"audio: system audio unavailable\n";
    }
    if (mic) {
        auto s = std::make_unique<AudioSource>(); s->name = L"mic"; s->isMic = true;
        if (s->open(false, micId, log)) sources_.push_back(std::move(s)); else log += L"audio: microphone unavailable\n";
    }
    tone_ = testTone;
    pos_ = 0;
    return active();
}

void AudioMixer::stop() { sources_.clear(); tone_ = false; }

void AudioMixer::discard() { for (auto& s : sources_) s->clear(); }

void AudioMixer::produce(long long mediaTime, bool final, std::vector<short>& out, long long& startTime) {
    out.clear();
    // Mix 60 ms behind the video clock so device packets (10-20 ms latency) have arrived: no false underruns.
    if (!final) mediaTime -= 600000;
    long long target = mediaTime > 0 ? mediaTime * kRate / 10000000LL : 0;
    startTime = pos_ * 10000000LL / kRate;
    if (target <= pos_) return;
    size_t n = (size_t)(target - pos_);
    std::vector<float> mix(n * 2, 0.f), tmp(n * 2);
    for (auto& s : sources_) {
        size_t got = s->pull(tmp.data(), n);
        if (got < n) underrunFrames += n - got;
        if (s->isMic && micMuted.load()) continue;
        for (size_t i = 0; i < got * 2; ++i) mix[i] += tmp[i];
    }
    if (tone_) {
        for (size_t i = 0; i < n; ++i) {
            float v = 0.2f * (float)std::sin(phase_);
            phase_ += 2 * 3.14159265358979 * 440.0 / kRate;
            if (phase_ > 6.283185307179586) phase_ -= 6.283185307179586;
            mix[i * 2] += v; mix[i * 2 + 1] += v;
        }
    }
    out.resize(n * 2);
    for (size_t i = 0; i < n * 2; ++i) {
        float v = mix[i];
        v = v > 1.f ? 1.f : v < -1.f ? -1.f : v;
        out[i] = (short)std::lrint(v * 32767.f);
    }
    pos_ = target;
}

}  // namespace cr
