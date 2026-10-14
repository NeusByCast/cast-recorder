#include "hotkey.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace cr {
namespace {

struct KeyEntry { uint32_t vk; const char* name; };
// Named non-alphanumeric keys (Win32 VK codes).
const KeyEntry kKeys[] = {
    {0x08, "Backspace"}, {0x09, "Tab"}, {0x0D, "Enter"}, {0x13, "Pause"}, {0x14, "CapsLock"},
    {0x1B, "Esc"}, {0x20, "Space"}, {0x21, "PageUp"}, {0x22, "PageDown"}, {0x23, "End"},
    {0x24, "Home"}, {0x25, "Left"}, {0x26, "Up"}, {0x27, "Right"}, {0x28, "Down"},
    {0x2C, "PrtSc"}, {0x2D, "Insert"}, {0x2E, "Delete"}, {0x6A, "Num *"}, {0x6B, "Num +"},
    {0x6D, "Num -"}, {0x6E, "Num ."}, {0x6F, "Num /"}, {0x90, "NumLock"}, {0x91, "ScrollLock"},
    {0xBA, ";"}, {0xBB, "="}, {0xBC, ","}, {0xBD, "-"}, {0xBE, "."}, {0xBF, "/"}, {0xC0, "`"},
    {0xDB, "["}, {0xDC, "\\"}, {0xDD, "]"}, {0xDE, "'"},
    {0xAD, "VolumeMute"}, {0xAE, "VolumeDown"}, {0xAF, "VolumeUp"},
    {0xB0, "MediaNext"}, {0xB1, "MediaPrev"}, {0xB2, "MediaStop"}, {0xB3, "MediaPlayPause"},
};
const KeyEntry kAliases[] = {
    {0x1B, "Escape"}, {0x0D, "Return"}, {0x2C, "PrintScreen"}, {0x2C, "PrtScn"}, {0x2C, "Print"},
    {0x2D, "Ins"}, {0x2E, "Del"}, {0x21, "PgUp"}, {0x22, "PgDn"}, {0x13, "Break"},
    {0x25, "ArrowLeft"}, {0x26, "ArrowUp"}, {0x27, "ArrowRight"}, {0x28, "ArrowDown"},
    {0xBB, "Plus"}, {0xBD, "Minus"}, {0xBC, "Comma"}, {0xBE, "Period"},
};

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
bool ieq(const std::string& a, const char* b) { return lower(a) == lower(b); }

}  // namespace

bool isModifierKey(uint32_t vk) {
    switch (vk) {
        case 0x10: case 0x11: case 0x12: case 0x5B: case 0x5C:
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: return true;
        default: return false;
    }
}

std::string keyName(uint32_t vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x70 + 1);  // F1..F24
    if (vk >= 0x60 && vk <= 0x69) return "Num " + std::to_string(vk - 0x60);
    for (auto& k : kKeys) if (k.vk == vk) return k.name;
    return {};
}

uint32_t keyFromName(const std::string& raw) {
    std::string n = trim(raw);
    if (n.empty()) return 0;
    if (n.size() == 1) {
        char c = (char)std::toupper((unsigned char)n[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (uint32_t)c;
    }
    if ((n[0] == 'F' || n[0] == 'f') && n.size() <= 3 && n.size() >= 2) {
        bool digits = std::all_of(n.begin() + 1, n.end(), [](char c) { return std::isdigit((unsigned char)c); });
        if (digits) {
            int f = std::stoi(n.substr(1));
            if (f >= 1 && f <= 24) return 0x70 + (uint32_t)(f - 1);
        }
    }
    std::string l = lower(n);
    if (l.rfind("num", 0) == 0) {
        std::string rest = trim(l.substr(l.compare(0, 6, "numpad") == 0 ? 6 : 3));
        if (rest.size() == 1 && std::isdigit((unsigned char)rest[0])) return 0x60 + (uint32_t)(rest[0] - '0');
    }
    for (auto& k : kKeys) if (ieq(n, k.name)) return k.vk;
    for (auto& k : kAliases) if (ieq(n, k.name)) return k.vk;
    return 0;
}

std::string format(const Hotkey& hk) {
    std::string s;
    auto add = [&](const char* p) { if (!s.empty()) s += "+"; s += p; };
    if (hk.mods & MOD_CTRL_) add("Ctrl");
    if (hk.mods & MOD_ALT_) add("Alt");
    if (hk.mods & MOD_SHIFT_) add("Shift");
    if (hk.mods & MOD_WIN_) add("Win");
    if (hk.vk) {
        std::string k = keyName(hk.vk);
        if (k.empty()) { char b[16]; std::snprintf(b, sizeof b, "0x%02X", hk.vk); k = b; }
        add(k.c_str());
    }
    return s;
}

bool parse(const std::string& text, Hotkey& out, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    Hotkey hk;
    std::string t = trim(text);
    if (t.empty()) { out = Hotkey{}; return true; }  // empty = unassigned
    if (t.size() > 256) return fail("Hotkey text too long");
    // Split on '+'. A '+' right after a separator at the end ("Ctrl++") is the plus key (VK_OEM_PLUS, the
    // '=' / '+' key), and "Num +" is the numpad plus.
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '+' && !trim(cur).empty() && !ieq(trim(cur), "num")) { parts.push_back(trim(cur)); cur.clear(); }
        else if (t[i] == '+' && trim(cur).empty() && !parts.empty() && i + 1 == t.size()) cur = "Plus";
        else cur += t[i];
    }
    if (!trim(cur).empty()) parts.push_back(trim(cur));
    if (parts.empty()) return fail("Empty hotkey");
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& p = parts[i];
        uint32_t m = 0;
        if (ieq(p, "ctrl") || ieq(p, "control")) m = MOD_CTRL_;
        else if (ieq(p, "alt")) m = MOD_ALT_;
        else if (ieq(p, "shift")) m = MOD_SHIFT_;
        else if (ieq(p, "win") || ieq(p, "windows") || ieq(p, "super") || ieq(p, "meta")) m = MOD_WIN_;
        if (m) {
            if (hk.mods & m) return fail("Duplicate modifier: " + p);
            hk.mods |= m;
            continue;
        }
        if (hk.vk) return fail("Only one non-modifier key is allowed");
        uint32_t vk = keyFromName(p);
        if (!vk) return fail("Unknown key: " + p);
        hk.vk = vk;
    }
    if (!hk.vk) return fail("Add a key besides the modifiers");
    out = hk;
    return true;
}

Issue validate(const Hotkey& hk) {
    if (hk.empty()) return {Severity::Ok, ""};
    if (isModifierKey(hk.vk)) return {Severity::Error, "Add a key besides the modifiers"};
    if (keyName(hk.vk).empty()) return {Severity::Error, "Unsupported key"};
    const uint32_t m = hk.mods;
    struct R { uint32_t mods, vk; const char* why; };
    static const R reserved[] = {
        {MOD_ALT_, 0x09, "Alt+Tab is reserved by Windows"},
        {MOD_ALT_, 0x73, "Alt+F4 closes the active window"},
        {MOD_CTRL_ | MOD_ALT_, 0x2E, "Ctrl+Alt+Delete is reserved by Windows"},
        {MOD_CTRL_ | MOD_SHIFT_, 0x1B, "Ctrl+Shift+Esc opens Task Manager"},
        {MOD_WIN_, 'L', "Win+L locks the PC"},
        {MOD_WIN_, 'D', "Win+D is used by Windows"},
        {MOD_WIN_, 'G', "Win+G opens the Xbox Game Bar"},
        {MOD_WIN_ | MOD_ALT_, 'R', "Win+Alt+R is used by Game Bar recording"},
        {MOD_CTRL_, 0x1B, "Ctrl+Esc opens the Start menu"},
    };
    for (auto& r : reserved)
        if (r.mods == m && r.vk == hk.vk) return {Severity::Error, r.why};
    const bool typing = (hk.vk >= 'A' && hk.vk <= 'Z') || (hk.vk >= '0' && hk.vk <= '9') || hk.vk == 0x20 ||
                        hk.vk == 0x0D || hk.vk == 0x08 || hk.vk == 0x09 || (hk.vk >= 0xBA && hk.vk <= 0xDE);
    // Overlay/recorder defaults of GPU vendor apps: they usually win the combo while a game runs.
    static const R overlay[] = {
        {MOD_ALT_, 0x78, "Alt+F9 is NVIDIA ShadowPlay's record toggle"},
        {MOD_ALT_, 0x79, "Alt+F10 is NVIDIA's instant replay save"},
        {MOD_ALT_, 'Z', "Alt+Z opens the NVIDIA overlay"},
        {MOD_ALT_, 0x70, "Alt+F1 is NVIDIA's screenshot key"},
        {MOD_ALT_, 'R', "Alt+R toggles NVIDIA's performance overlay"},
        {MOD_ALT_ | MOD_SHIFT_, 0x7B, "Alt+Shift+F12 is NVIDIA's stats overlay"},
        {MOD_CTRL_ | MOD_SHIFT_, 'E', "Ctrl+Shift+E starts AMD Adrenalin recording"},
        {MOD_CTRL_ | MOD_SHIFT_, 'R', "Ctrl+Shift+R is AMD's instant replay"},
        {MOD_CTRL_ | MOD_SHIFT_, 'S', "Ctrl+Shift+S saves an AMD instant replay"},
        {MOD_CTRL_ | MOD_SHIFT_, 'I', "Ctrl+Shift+I takes an AMD screenshot"},
        {MOD_CTRL_ | MOD_SHIFT_, 'O', "Ctrl+Shift+O toggles AMD's metrics overlay"},
    };
    for (const R& r : overlay)
        if (r.why && r.mods == m && r.vk == hk.vk) return {Severity::Warning, std::string(r.why) + " - it may not reach Cast Recorder"};
    if (typing && (m == 0 || m == MOD_SHIFT_))
        return {Severity::Warning, "This key is used for typing and will be blocked everywhere"};
    if (m == 0 && hk.vk != 0x2C && !(hk.vk >= 0x70 && hk.vk <= 0x87) && !(hk.vk >= 0xAD && hk.vk <= 0xB3))
        return {Severity::Warning, "A key without modifiers may clash with games"};
    return {Severity::Ok, ""};
}

std::vector<std::pair<size_t, size_t>> findConflicts(const std::vector<NamedHotkey>& list) {
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t i = 0; i < list.size(); ++i)
        for (size_t j = i + 1; j < list.size(); ++j)
            if (!list[i].key.empty() && list[i].key == list[j].key) out.emplace_back(i, j);
    return out;
}

}  // namespace cr
