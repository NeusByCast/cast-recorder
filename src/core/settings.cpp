#include "settings.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace cr {

const char* toString(Encoder e) {
    switch (e) { case Encoder::NVENC: return "nvenc"; case Encoder::AMF: return "amf";
        case Encoder::QSV: return "qsv"; case Encoder::Software: return "software"; default: return "auto"; }
}
const char* toString(Container c) {
    switch (c) { case Container::MKV: return "mkv"; case Container::MOV: return "mov"; default: return "mp4"; }
}
const char* toString(Quality q) {
    switch (q) { case Quality::Low: return "low"; case Quality::Medium: return "medium";
        case Quality::Ultra: return "ultra"; default: return "high"; }
}
const char* toString(OutputScale s) {
    switch (s) { case OutputScale::P1080: return "1080p"; case OutputScale::P720: return "720p"; default: return "native"; }
}
const char* extension(Container c) {
    switch (c) { case Container::MKV: return ".mkv"; case Container::MOV: return ".mov"; default: return ".mp4"; }
}
bool fromString(const std::string& s, Encoder& e) {
    for (Encoder v : {Encoder::Auto, Encoder::NVENC, Encoder::AMF, Encoder::QSV, Encoder::Software})
        if (s == toString(v)) { e = v; return true; }
    return false;
}
bool fromString(const std::string& s, Container& c) {
    for (Container v : {Container::MP4, Container::MKV, Container::MOV})
        if (s == toString(v)) { c = v; return true; }
    return false;
}
bool fromString(const std::string& s, Quality& q) {
    for (Quality v : {Quality::Low, Quality::Medium, Quality::High, Quality::Ultra})
        if (s == toString(v)) { q = v; return true; }
    return false;
}
bool fromString(const std::string& s, OutputScale& o) {
    for (OutputScale v : {OutputScale::Native, OutputScale::P1080, OutputScale::P720})
        if (s == toString(v)) { o = v; return true; }
    return false;
}

int normalizeFps(int fps) {
    if (fps <= 22) return 15;
    if (fps <= 45) return 30;
    return 60;
}

unsigned bitrateFor(Quality q, int w, int h, int fps, unsigned cap) {
    // Bits per pixel per frame tuned for H.264 screen/game content (CBR-ish target).
    double bpp = q == Quality::Low ? 0.04 : q == Quality::Medium ? 0.07 : q == Quality::High ? 0.11 : 0.17;
    double br = bpp * (double)w * (double)h * (double)fps;
    if (br < 1e6) br = 1e6;
    if (br > 150e6) br = 150e6;
    if (cap && br > (double)cap) br = (double)cap;
    return (unsigned)br;
}

// ---------------- JSON ----------------
namespace {
std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

struct Value { enum T { Str, Num, Bool, Null, Other } t = Null; std::string s; double n = 0; bool b = false; };

struct Parser {
    const std::string& j; size_t i = 0; std::string err;
    explicit Parser(const std::string& s) : j(s) {}
    void ws() { while (i < j.size() && (j[i] == ' ' || j[i] == '\n' || j[i] == '\r' || j[i] == '\t')) ++i; }
    bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at " + std::to_string(i); return false; }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > j.size()) return fail("bad \\u escape");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = j[i++]; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return fail("bad hex");
        }
        return true;
    }
    bool str(std::string& o) {
        if (j[i] != '"') return fail("expected string");
        ++i;
        while (i < j.size()) {
            unsigned char c = (unsigned char)j[i++];
            if (c == '"') return true;
            if (c < 0x20) return fail("control character in string");
            if (c >= 0x80) {  // validate one UTF-8 sequence (no overlongs, no surrogates, <= U+10FFFF)
                int n = (c >= 0xC2 && c <= 0xDF) ? 1 : (c >= 0xE0 && c <= 0xEF) ? 2 : (c >= 0xF0 && c <= 0xF4) ? 3 : -1;
                if (n < 0 || i + n > j.size()) return fail("invalid UTF-8");
                unsigned char c1 = (unsigned char)j[i];
                if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 > 0x9F) || (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 > 0x8F))
                    return fail("invalid UTF-8");
                o += (char)c;
                for (int k = 0; k < n; ++k) {
                    unsigned char cc = (unsigned char)j[i++];
                    if ((cc & 0xC0) != 0x80) return fail("invalid UTF-8");
                    o += (char)cc;
                }
                continue;
            }
            if (c != '\\') { o += (char)c; continue; }
            if (i >= j.size()) break;
            char e = j[i++];
            switch (e) {
                case '"': o += '"'; break; case '\\': o += '\\'; break; case '/': o += '/'; break;
                case 'b': o += '\b'; break; case 'f': o += '\f'; break; case 'n': o += '\n'; break;
                case 'r': o += '\r'; break; case 't': o += '\t'; break;
                case 'u': {
                    unsigned cp = 0; if (!hex4(cp)) return false;
                    if (cp == 0) return fail("\\u0000 not allowed");
                    if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("lone low surrogate");
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        if (!(i + 1 < j.size() && j[i] == '\\' && j[i + 1] == 'u')) return fail("lone high surrogate");
                        i += 2; unsigned lo = 0; if (!hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail("lone high surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(o, cp); break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool skipValue(int depth = 0) {  // for nested objects/arrays we don't use
        if (depth > 32) return fail("too deep");
        ws();
        if (i >= j.size()) return fail("unexpected end");
        char open = j[i];
        if (open != '{' && open != '[') { Value v; return scalar(v); }
        char close = open == '{' ? '}' : ']';
        ++i; ws();
        if (i < j.size() && j[i] == close) { ++i; return true; }
        while (true) {
            ws();
            if (open == '{') { std::string k; if (!str(k)) return false; ws(); if (j[i] != ':') return fail("expected :"); ++i; }
            if (!skipValue(depth + 1)) return false;
            ws();
            if (i >= j.size()) return fail("unexpected end");
            if (j[i] == ',') { ++i; continue; }
            if (j[i] == close) { ++i; return true; }
            return fail("expected , or close");
        }
    }
    bool scalar(Value& v) {
        ws();
        if (i >= j.size()) return fail("unexpected end");
        char c = j[i];
        if (c == '"') { v.t = Value::Str; return str(v.s); }
        if (j.compare(i, 4, "true") == 0) { i += 4; v.t = Value::Bool; v.b = true; return true; }
        if (j.compare(i, 5, "false") == 0) { i += 5; v.t = Value::Bool; v.b = false; return true; }
        if (j.compare(i, 4, "null") == 0) { i += 4; v.t = Value::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            // RFC 8259 number: -? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?
            size_t st = i;
            auto digits = [&] { size_t a = i; while (i < j.size() && std::isdigit((unsigned char)j[i])) ++i; return i > a; };
            if (j[i] == '-') ++i;
            if (i < j.size() && j[i] == '0') ++i;
            else if (!digits()) return fail("invalid number");
            if (i < j.size() && j[i] == '.') { ++i; if (!digits()) return fail("invalid number"); }
            if (i < j.size() && (j[i] == 'e' || j[i] == 'E')) {
                ++i;
                if (i < j.size() && (j[i] == '+' || j[i] == '-')) ++i;
                if (!digits()) return fail("invalid number");
            }
            v.t = Value::Num; v.n = std::strtod(j.substr(st, i - st).c_str(), nullptr); return true;
        }
        if (c == '{' || c == '[') { v.t = Value::Other; return skipValue(); }
        return fail("unexpected character");
    }
    bool object(std::map<std::string, Value>& out) {
        ws();
        if (i >= j.size() || j[i] != '{') return fail("expected {");
        ++i; ws();
        if (i < j.size() && j[i] == '}') { ++i; return trailing(); }
        while (true) {
            ws(); std::string k;
            if (i >= j.size() || !str(k)) return fail("expected key");
            ws();
            if (i >= j.size() || j[i] != ':') return fail("expected :");
            ++i;
            Value v; if (!scalar(v)) return false;
            out[k] = v;
            ws();
            if (i >= j.size()) return fail("unexpected end");
            if (j[i] == ',') { ++i; continue; }
            if (j[i] == '}') { ++i; return trailing(); }
            return fail("expected , or }");
        }
    }
    bool trailing() { ws(); return i == j.size() ? true : fail("trailing data"); }
};
}  // namespace

std::string toJson(const Settings& s) {
    std::ostringstream o;
    o << "{\n"
      << "  \"version\": 2,\n"
      << "  \"fps\": " << s.fps << ",\n"
      << "  \"quality\": \"" << toString(s.quality) << "\",\n"
      << "  \"encoder\": \"" << toString(s.encoder) << "\",\n"
      << "  \"container\": \"" << toString(s.container) << "\",\n"
      << "  \"outputDir\": \"" << esc(s.outputDir) << "\",\n"
      << "  \"hotkeyToggleRecord\": \"" << esc(format(s.toggleRecord)) << "\",\n"
      << "  \"hotkeyOpenFolder\": \"" << esc(format(s.openFolder)) << "\",\n"
      << "  \"hotkeyPause\": \"" << esc(format(s.pauseToggle)) << "\",\n"
      << "  \"captureCursor\": " << (s.captureCursor ? "true" : "false") << ",\n"
      << "  \"startMinimized\": " << (s.startMinimized ? "true" : "false") << ",\n"
      << "  \"scale\": \"" << toString(s.scale) << "\",\n"
      << "  \"gpuIndex\": " << s.gpuIndex << ",\n"
      << "  \"bitrateCap\": " << s.bitrateCap << "\n"
      << "}\n";
    return o.str();
}

bool fromJson(const std::string& json, Settings& out, std::string* err) {
    Parser p(json);
    std::map<std::string, Value> m;
    if (!p.object(m)) { if (err) *err = p.err; return false; }
    Settings s;  // defaults
    auto str = [&](const char* k, std::string& dst) { auto it = m.find(k); if (it != m.end() && it->second.t == Value::Str) { dst = it->second.s; return true; } return false; };
    auto boo = [&](const char* k, bool& dst) { auto it = m.find(k); if (it != m.end() && it->second.t == Value::Bool) dst = it->second.b; };
    auto it = m.find("fps");
    // Range-check before converting so huge or negative values can't overflow the int cast.
    if (it != m.end() && it->second.t == Value::Num && std::isfinite(it->second.n) && it->second.n >= 1 && it->second.n <= 1000)
        s.fps = normalizeFps((int)it->second.n);
    std::string v;
    if (str("quality", v)) fromString(v, s.quality);
    if (str("encoder", v)) fromString(v, s.encoder);
    if (str("container", v)) fromString(v, s.container);
    str("outputDir", s.outputDir);
    Hotkey hk;
    if (str("hotkeyToggleRecord", v) && parse(v, hk) && validate(hk).severity != Severity::Error) s.toggleRecord = hk;
    if (str("hotkeyOpenFolder", v) && parse(v, hk) && validate(hk).severity != Severity::Error) s.openFolder = hk;
    if (str("hotkeyPause", v)) { if (v.empty()) s.pauseToggle = Hotkey{}; else if (parse(v, hk) && validate(hk).severity != Severity::Error) s.pauseToggle = hk; }
    if (!s.pauseToggle.empty() && (s.pauseToggle == s.toggleRecord || s.pauseToggle == s.openFolder)) s.pauseToggle = Hotkey{};
    // v1 -> v2: replace the old defaults (NVIDIA ShadowPlay combos) only if the user never changed them.
    auto ver = m.find("version");
    bool v1 = ver == m.end() || ver->second.t != Value::Num || ver->second.n < 2;
    if (v1) {
        if (s.toggleRecord == Hotkey{MOD_ALT_, 0x78}) s.toggleRecord = Settings{}.toggleRecord;
        if (s.pauseToggle == Hotkey{MOD_ALT_, 0x79}) s.pauseToggle = Settings{}.pauseToggle;
    }
    // Two actions can't share one combo: keep Start/stop and clear the duplicate.
    if (!s.openFolder.empty() && s.openFolder == s.toggleRecord) s.openFolder = Hotkey{};
    boo("captureCursor", s.captureCursor);
    boo("startMinimized", s.startMinimized);
    if (str("scale", v)) fromString(v, s.scale);
    auto git = m.find("gpuIndex");
    if (git != m.end() && git->second.t == Value::Num && std::isfinite(git->second.n) && git->second.n >= -1 && git->second.n <= 32)
        s.gpuIndex = (int)git->second.n;
    auto bit = m.find("bitrateCap");
    if (bit != m.end() && bit->second.t == Value::Num && std::isfinite(bit->second.n) && bit->second.n >= 0 && bit->second.n <= 500e6)
        s.bitrateCap = (unsigned)bit->second.n;
    out = s;
    return true;
}

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
#endif

bool loadFile(const std::string& path, Settings& out) {
    out = Settings{};
#ifdef _WIN32
    std::ifstream f(widen(path).c_str(), std::ios::binary);
#else
    std::ifstream f(path, std::ios::binary);
#endif
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    std::string data = ss.str();
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) data.erase(0, 3);
    Settings s;
    if (!fromJson(data, s)) return false;
    out = s;
    return true;
}

bool saveFile(const std::string& path, const Settings& s) {
    const std::string tmp = path + ".tmp";
    {
#ifdef _WIN32
        std::ofstream f(widen(tmp).c_str(), std::ios::binary | std::ios::trunc);
#else
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
#endif
        if (!f) return false;
        f << toJson(s);
        if (!f.flush()) return false;
    }
#ifdef _WIN32
    return MoveFileExW(widen(tmp).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

}  // namespace cr
