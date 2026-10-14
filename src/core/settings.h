// Platform-independent settings model with a small, dependency-free JSON (de)serializer.
#pragma once
#include <string>
#include "hotkey.h"

namespace cr {

enum class Encoder { Auto, NVENC, AMF, QSV, Software };
enum class Container { MP4, MKV, MOV };
enum class Quality { Low, Medium, High, Ultra };
enum class OutputScale { Native, P1080, P720 };  // Native / 1080p / 720p

struct Settings {
    int fps = 30;                       // 15, 30 or 60
    Quality quality = Quality::High;
    Encoder encoder = Encoder::Auto;
    Container container = Container::MP4;
    std::string outputDir;              // UTF-8; empty = default (Videos\Cast Recorder)
    Hotkey toggleRecord{MOD_CTRL_ | MOD_SHIFT_, 0x78};  // Ctrl+Shift+F9 (Alt+F9 is NVIDIA ShadowPlay)
    Hotkey openFolder{MOD_ALT_ | MOD_SHIFT_, 0x78};   // Alt+Shift+F9 (no UI field; still registered)
    Hotkey pauseToggle{MOD_CTRL_ | MOD_SHIFT_, 0x79};  // Ctrl+Shift+F10
    Hotkey micMute{MOD_CTRL_ | MOD_SHIFT_, 0x7A};      // Ctrl+Shift+F11
    bool captureCursor = true;
    bool systemAudio = true;           // WASAPI loopback of the default output device
    bool micEnabled = false;
    std::string micDevice;             // UTF-8 endpoint id; empty = default microphone
    bool startMinimized = false;
    bool maximized = false;
    std::string gpuName;             // adapter description (stable across driver/list order changes); gpuIndex is a hint
    bool pauseTones = false;         // play a tick on pause/resume (off: the mic would pick it up)          // window state, restored at launch
    OutputScale scale = OutputScale::Native;
    int gpuIndex = -1;                 // -1 Auto; else adapter ordinal
    unsigned bitrateCap = 0;           // 0 = uncapped beyond quality
    // Transient (never written): set by fromJson when v1 Alt+F9/Alt+F10 defaults were replaced. The UI shows
    // a one-time notice and saves at v2, so it never fires again.
    bool hotkeysMigrated = false;
};

const char* toString(Encoder e);
const char* toString(Container c);
const char* toString(Quality q);
const char* toString(OutputScale s);
const char* extension(Container c);  // ".mp4"
bool fromString(const std::string& s, Encoder& e);
bool fromString(const std::string& s, Container& c);
bool fromString(const std::string& s, Quality& q);
bool fromString(const std::string& s, OutputScale& o);

// Target bitrate in bits/s for a resolution, fps and quality preset.
unsigned bitrateFor(Quality q, int width, int height, int fps, unsigned cap = 0);
// Clamps to 15/30/60 (nearest).
int normalizeFps(int fps);

std::string toJson(const Settings& s);
// Unknown keys are ignored; invalid values keep defaults. Returns false only on malformed JSON.
bool fromJson(const std::string& json, Settings& out, std::string* err = nullptr);

bool loadFile(const std::string& path, Settings& out);   // false if missing/invalid (out = defaults)
bool saveFile(const std::string& path, const Settings& s);  // atomic write via temp + rename

}  // namespace cr
