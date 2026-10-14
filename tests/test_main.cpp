// Dependency-free unit tests for the platform-independent core (runs on Linux and Windows).
#include <cstdio>
#include <cstdlib>
#include <string>
#include "../src/core/hotkey.h"
#include "../src/core/settings.h"

using namespace cr;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (_a == _b) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

static Hotkey P(const char* s) { Hotkey h; std::string e; if (!parse(s, h, &e)) std::printf("  parse('%s') error: %s\n", s, e.c_str()); return h; }

static void testParseFormat() {
    CHECK_EQ(format(P("Alt+V")), std::string("Alt+V"));
    CHECK_EQ(format(P("ctrl + shift + f9")), std::string("Ctrl+Shift+F9"));
    CHECK_EQ(format(P("Shift+Ctrl+F9")), std::string("Ctrl+Shift+F9"));  // canonical order
    CHECK_EQ(format(P("win+alt+prtsc")), std::string("Alt+Win+PrtSc"));
    CHECK_EQ(format(P("Ctrl+Alt+Shift+Win+F24")), std::string("Ctrl+Alt+Shift+Win+F24"));
    CHECK_EQ(format(P("F12")), std::string("F12"));
    CHECK_EQ(format(P("Ctrl+Num 5")), std::string("Ctrl+Num 5"));
    CHECK_EQ(format(P("Ctrl+Numpad5")), std::string("Ctrl+Num 5"));
    CHECK_EQ(format(P("Ctrl+Num +")), std::string("Ctrl+Num +"));
    CHECK_EQ(format(P("Alt+PageUp")), std::string("Alt+PageUp"));
    CHECK_EQ(format(P("Alt+escape")), std::string("Alt+Esc"));
    CHECK_EQ(format(P("Control+;")), std::string("Ctrl+;"));
    CHECK_EQ(format(P("Alt+7")), std::string("Alt+7"));
    CHECK_EQ(P("Alt+V").vk, (uint32_t)'V');
    CHECK_EQ(P("Alt+V").mods, (uint32_t)MOD_ALT_);
    CHECK_EQ(P("Ctrl+Shift+F9").vk, (uint32_t)0x78);
    CHECK(P("").empty());
    Hotkey h; std::string e;
    CHECK(!parse("Ctrl+Shift", h, &e)); CHECK(!e.empty());
    CHECK(!parse("Ctrl+Ctrl+A", h, &e));
    CHECK(!parse("Ctrl+A+B", h, &e));
    CHECK(!parse("Ctrl+Foo", h, &e));
    CHECK(!parse("Ctrl+F25", h, &e));
    // Round-trip every named key.
    int rt = 0;
    for (uint32_t vk = 1; vk < 256; ++vk) {
        std::string n = keyName(vk);
        if (n.empty()) continue;
        Hotkey k{MOD_CTRL_ | MOD_ALT_, vk}, back;
        CHECK(parse(format(k), back) && back == k);
        ++rt;
    }
    CHECK(rt > 90);
}

static void testValidateConflicts() {
    CHECK(validate(P("Alt+V")).severity == Severity::Ok);
    CHECK(validate(P("Ctrl+Shift+F9")).severity == Severity::Ok);
    CHECK(validate(P("F9")).severity == Severity::Ok);
    CHECK(validate(P("Alt+Tab")).severity == Severity::Error);
    CHECK(validate(P("Alt+F4")).severity == Severity::Error);
    CHECK(validate(P("Win+L")).severity == Severity::Error);
    CHECK(validate(P("Win+G")).severity == Severity::Error);
    CHECK(validate(P("V")).severity == Severity::Warning);
    CHECK(validate(P("Shift+V")).severity == Severity::Warning);
    CHECK(validate(P("Home")).severity == Severity::Warning);
    CHECK(validate(Hotkey{MOD_CTRL_, 0x11}).severity == Severity::Error);  // modifier as key
    CHECK(validate(Hotkey{}).severity == Severity::Ok);
    std::vector<NamedHotkey> l = {{"rec", P("Alt+V")}, {"folder", P("Alt+F9")}, {"x", P("alt+v")}, {"y", Hotkey{}}, {"z", Hotkey{}}};
    auto c = findConflicts(l);
    CHECK_EQ(c.size(), (size_t)1);
    if (!c.empty()) { CHECK_EQ(c[0].first, (size_t)0); CHECK_EQ(c[0].second, (size_t)2); }
    CHECK(findConflicts({{"a", P("Alt+V")}, {"b", P("Ctrl+V")}}).empty());
}

static void testSettings() {
    Settings d;
    CHECK_EQ(d.fps, 60);
    CHECK(d.encoder == Encoder::Auto);
    Settings s;
    s.fps = 30; s.quality = Quality::Ultra; s.encoder = Encoder::NVENC; s.container = Container::MKV;
    s.outputDir = "C:\\Users\\Ünïcødé \"q\"\\Videos\\Cast Recorder";
    s.toggleRecord = P("Ctrl+Shift+F9"); s.openFolder = P("Alt+V"); s.captureCursor = false; s.startMinimized = true;
    Settings r;
    CHECK(fromJson(toJson(s), r));
    CHECK_EQ(r.fps, 30); CHECK(r.quality == Quality::Ultra); CHECK(r.encoder == Encoder::NVENC);
    CHECK(r.container == Container::MKV); CHECK_EQ(r.outputDir, s.outputDir);
    CHECK(r.toggleRecord == s.toggleRecord); CHECK(r.openFolder == s.openFolder);
    CHECK(!r.captureCursor); CHECK(r.startMinimized);
    // Bad values fall back to defaults, unknown keys and nested values ignored.
    CHECK(fromJson("{\"fps\":59,\"encoder\":\"bogus\",\"quality\":7,\"extra\":{\"a\":[1,2,{}]},\"hotkeyToggleRecord\":\"Alt+Tab\"}", r));
    CHECK_EQ(r.fps, 60); CHECK(r.encoder == Encoder::Auto); CHECK(r.quality == Quality::High);
    CHECK(r.toggleRecord == Settings{}.toggleRecord);
    CHECK(fromJson("{\"fps\":14}", r)); CHECK_EQ(r.fps, 15);
    CHECK(fromJson("{\"outputDir\":\"\\u00dc\\ud83d\\ude00\"}", r)); CHECK_EQ(r.outputDir, std::string("\xC3\x9C\xF0\x9F\x98\x80"));
    std::string err;
    CHECK(!fromJson("{\"fps\":", r, &err)); CHECK(!err.empty());
    CHECK(!fromJson("not json", r));
    CHECK(!fromJson("{} trailing", r));
    CHECK(!fromJson("{\"a\":\"unterminated}", r));
    // File I/O
    const char* path = "cr_test_settings.json";
    CHECK(saveFile(path, s));
    Settings f; CHECK(loadFile(path, f)); CHECK_EQ(f.fps, 30); CHECK_EQ(f.outputDir, s.outputDir);
    std::remove(path);
    CHECK(!loadFile("does/not/exist.json", f)); CHECK_EQ(f.fps, 60);
    // Bitrates
    CHECK(bitrateFor(Quality::High, 1920, 1080, 60) > bitrateFor(Quality::Medium, 1920, 1080, 60));
    CHECK(bitrateFor(Quality::High, 1920, 1080, 60) > bitrateFor(Quality::High, 1920, 1080, 30));
    CHECK(bitrateFor(Quality::Ultra, 7680, 4320, 60) <= 150000000u);
    CHECK(bitrateFor(Quality::Low, 320, 240, 15) >= 1000000u);
    CHECK_EQ(std::string(extension(Container::MKV)), std::string(".mkv"));
}

int main() {
    testParseFormat();
    testValidateConflicts();
    testSettings();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
