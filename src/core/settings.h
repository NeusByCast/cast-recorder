// Platform-independent settings model with a small, dependency-free JSON (de)serializer.
#pragma once
#include <string>
#include "hotkey.h"

namespace cr {

enum class Encoder { Auto, NVENC, AMF, QSV, Software };
enum class Container { MP4, MKV, MOV };
enum class Quality { Low, Medium, High, Ultra };

struct Settings {
    int fps = 60;                       // 15, 30 or 60
    Quality quality = Quality::High;
    Encoder encoder = Encoder::Auto;
    Container container = Container::MP4;
    std::string outputDir;              // UTF-8; empty = default (Videos\Cast Recorder)
    Hotkey toggleRecord{MOD_ALT_, 0x78};              // Alt+F9
    Hotkey openFolder{MOD_ALT_ | MOD_SHIFT_, 0x78};   // Alt+Shift+F9
    bool captureCursor = true;
    bool startMinimized = false;
};

const char* toString(Encoder e);
const char* toString(Container c);
const char* toString(Quality q);
const char* extension(Container c);  // ".mp4"
bool fromString(const std::string& s, Encoder& e);
bool fromString(const std::string& s, Container& c);
bool fromString(const std::string& s, Quality& q);

// Target bitrate in bits/s for a resolution, fps and quality preset.
unsigned bitrateFor(Quality q, int width, int height, int fps);
// Clamps to 15/30/60 (nearest).
int normalizeFps(int fps);

std::string toJson(const Settings& s);
// Unknown keys are ignored; invalid values keep defaults. Returns false only on malformed JSON.
bool fromJson(const std::string& json, Settings& out, std::string* err = nullptr);

bool loadFile(const std::string& path, Settings& out);   // false if missing/invalid (out = defaults)
bool saveFile(const std::string& path, const Settings& s);  // atomic write via temp + rename

}  // namespace cr
