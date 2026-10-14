#pragma once
// Audio: system audio (process-loopback excluding our own process, or endpoint loopback) + microphone.
// Every WASAPI packet is placed on the recording timeline by its QPC capture time (GetBuffer qpcPosition)
// against the same QPC zero as video; audio from before zero is dropped. Device clock drift is absorbed by a
// small continuous resampling-ratio correction (no silence inserts / trims except on real discontinuities).
// Lost devices (AUDCLNT_E_DEVICE_INVALIDATED) and default-device changes reconnect automatically.
#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cr {

struct AudioDevice { std::wstring id, name; };
std::vector<AudioDevice> listMicrophones();

struct AudioSource;
struct DeviceNotifier;

class AudioMixer {
public:
    static constexpr int kRate = 48000, kChannels = 2;
    AudioMixer();
    ~AudioMixer();
    // Any combination; testTone adds a synthetic 440 Hz source (CI, no audio hardware). Returns true if >=1 source.
    bool start(bool system, bool mic, const std::wstring& micId, bool testTone, std::wstring& log);
    void stop();
    bool active() const { return !sources_.empty() || tone_; }
    // Media-time zero as a QPC value (the first video frame's QPC; shifted by pauses). Audio is mapped against it.
    void setOrigin(long long qpcTicks, long long qpcFreq);
    void setPaused(bool p);
    // Produces interleaved s16 samples from the current position up to `mediaTime` (100 ns, recording clock).
    void produce(long long mediaTime, bool final, std::vector<short>& out, long long& startTime);
    std::atomic<bool> micMuted{false};
    unsigned long long underrunFrames = 0;
    // Mute windows in media time (100 ns) for our own tones when they could be captured (mic, or system audio
    // without process-loopback exclusion). The final flush (stop tone) is always excluded.
    void addMute(long long from, long long to, bool systemToo);
    long long systemMuteUntil = 0;  // start tone: [0, this)
    bool systemExcludesSelf() const;  // process loopback active: our tones never reach the system track
    std::wstring stats() const;
    struct Shared {
        std::atomic<long long> origin100ns{0};  // 0 = not started
        std::atomic<bool> paused{false};
        std::mutex evM;
        std::wstring events;
        void event(const std::wstring& e);
    };
private:
    std::vector<std::unique_ptr<AudioSource>> sources_;
    std::shared_ptr<Shared> sh_;
    DeviceNotifier* notifier_ = nullptr;
    bool tone_ = false;
    long long pos_ = 0;  // frames produced
    unsigned long long toneFrames_ = 0, produced_ = 0;
    double phase_ = 0;
    struct Mute { long long from, to; bool sys; };
    std::mutex muteM_;
    std::vector<Mute> mutes_;
};

}  // namespace cr
