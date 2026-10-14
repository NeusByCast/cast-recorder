#pragma once
// Audio: WASAPI loopback (system) + microphone, mixed on the recording's media clock into 48 kHz stereo s16.
#include <windows.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace cr {

struct AudioDevice { std::wstring id, name; };
std::vector<AudioDevice> listMicrophones();

struct AudioSource;

class AudioMixer {
public:
    static constexpr int kRate = 48000, kChannels = 2;
    AudioMixer();
    ~AudioMixer();
    // Any combination; testTone adds a synthetic 440 Hz source (CI, no audio hardware). Returns true if >=1 source.
    bool start(bool system, bool mic, const std::wstring& micId, bool testTone, std::wstring& log);
    void stop();
    bool active() const { return !sources_.empty() || tone_; }
    // Produces interleaved s16 samples from the current position up to `mediaTime` (100 ns, recording clock).
    // Gaps (no data from a device) are filled with silence, so audio stays locked to the video timeline.
    void produce(long long mediaTime, bool final, std::vector<short>& out, long long& startTime);
    void discard();  // while paused: drop device data, don't advance
    std::atomic<bool> micMuted{false};
    unsigned long long underrunFrames = 0, overrunFrames = 0;
private:
    std::vector<std::unique_ptr<AudioSource>> sources_;
    bool tone_ = false;
    long long pos_ = 0;  // frames produced
    double phase_ = 0;
};

}  // namespace cr
