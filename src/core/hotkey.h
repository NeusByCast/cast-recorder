// Platform-independent hotkey model: parse, format, validate, detect conflicts.
// Virtual-key values match Win32 VK_* codes so they can be passed straight to RegisterHotKey.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cr {

enum Mod : uint32_t { MOD_NONE_ = 0, MOD_ALT_ = 1, MOD_CTRL_ = 2, MOD_SHIFT_ = 4, MOD_WIN_ = 8 };

struct Hotkey {
    uint32_t mods = 0;  // bitmask of Mod (same bits as Win32 MOD_ALT/MOD_CONTROL/MOD_SHIFT/MOD_WIN)
    uint32_t vk = 0;    // Win32 virtual-key code; 0 = unset
    bool empty() const { return vk == 0; }
    bool operator==(const Hotkey& o) const { return mods == o.mods && vk == o.vk; }
    bool operator!=(const Hotkey& o) const { return !(*this == o); }
};

// Human-readable name for a virtual key ("V", "F9", "Num 5", "PrtSc"), empty if unknown.
std::string keyName(uint32_t vk);
// Reverse of keyName (case-insensitive, accepts some aliases). Returns 0 if unknown.
uint32_t keyFromName(const std::string& name);
bool isModifierKey(uint32_t vk);

// "Ctrl+Shift+F9" style. Canonical order: Ctrl, Alt, Shift, Win.
std::string format(const Hotkey& hk);
// Parses "ctrl + shift + f9", "Alt+V", "Win+PrtSc". Returns false on error and sets err.
bool parse(const std::string& text, Hotkey& out, std::string* err = nullptr);

enum class Severity { Ok, Warning, Error };
struct Issue { Severity severity; std::string message; };

// Checks a single combo: modifier-only, unknown key, reserved by Windows, plain typing keys.
Issue validate(const Hotkey& hk);

struct NamedHotkey { std::string action; Hotkey key; };
// Returns pairs of indices (i, j) in `list` that use the same non-empty combo.
std::vector<std::pair<size_t, size_t>> findConflicts(const std::vector<NamedHotkey>& list);

}  // namespace cr
