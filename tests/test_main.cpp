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
    // GPU vendor overlay defaults warn (they usually swallow the combo in games)
    for (const char* k : {"Alt+F9", "Alt+F10", "Alt+Z", "Alt+F1", "Ctrl+Shift+E", "Ctrl+Shift+R", "Ctrl+Shift+S"})
        CHECK(validate(P(k)).severity == Severity::Warning);
    CHECK(validate(P("Ctrl+Shift+F9")).severity == Severity::Ok);
    CHECK(validate(P("Ctrl+Shift+F10")).severity == Severity::Ok);
    std::vector<NamedHotkey> l = {{"rec", P("Alt+V")}, {"folder", P("Alt+F9")}, {"x", P("alt+v")}, {"y", Hotkey{}}, {"z", Hotkey{}}};
    auto c = findConflicts(l);
    CHECK_EQ(c.size(), (size_t)1);
    if (!c.empty()) { CHECK_EQ(c[0].first, (size_t)0); CHECK_EQ(c[0].second, (size_t)2); }
    CHECK(findConflicts({{"a", P("Alt+V")}, {"b", P("Ctrl+V")}}).empty());
}

static void testSettings() {
    Settings d;
    CHECK_EQ(d.fps, 30);
    CHECK(d.encoder == Encoder::Auto);
    Settings s;
    s.fps = 30; s.quality = Quality::Ultra; s.encoder = Encoder::NVENC; s.container = Container::MKV;
    s.outputDir = "C:\\Users\\Ünïcødé \"q\"\\Videos\\Cast Recorder";
    s.toggleRecord = P("Ctrl+Shift+F9"); s.openFolder = P("Alt+V"); s.captureCursor = false; s.startMinimized = true;
    s.scale = OutputScale::P1080; s.gpuIndex = 1; s.bitrateCap = 12000000;
    Settings r;
    CHECK(fromJson(toJson(s), r));
    CHECK_EQ(r.fps, 30);
    CHECK(r.scale == OutputScale::P1080);
    CHECK_EQ(r.gpuIndex, 1);
    CHECK_EQ(r.bitrateCap, 12000000u); CHECK(r.quality == Quality::Ultra); CHECK(r.encoder == Encoder::NVENC);
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
    CHECK(!loadFile("does/not/exist.json", f)); CHECK_EQ(f.fps, 30);
    // Defaults and v1 -> v2 hotkey migration (only untouched old defaults move)
    CHECK(Settings{}.toggleRecord == P("Ctrl+Shift+F9"));
    CHECK(Settings{}.pauseToggle == P("Ctrl+Shift+F10"));
    { Settings m; CHECK(fromJson("{\"version\":1,\"hotkeyToggleRecord\":\"Alt+F9\",\"hotkeyPause\":\"Alt+F10\"}", m));
      CHECK(m.toggleRecord == P("Ctrl+Shift+F9")); CHECK(m.pauseToggle == P("Ctrl+Shift+F10")); CHECK(m.hotkeysMigrated);
      Settings again; CHECK(fromJson(toJson(m), again)); CHECK(!again.hotkeysMigrated); }
    { Settings m; CHECK(fromJson("{\"version\":1,\"hotkeyToggleRecord\":\"Alt+V\"}", m)); CHECK(m.toggleRecord == P("Alt+V")); CHECK(!m.hotkeysMigrated); }
    { Settings m; CHECK(fromJson("{\"version\":2,\"hotkeyToggleRecord\":\"Alt+F9\"}", m)); CHECK(m.toggleRecord == P("Alt+F9")); }
    { Settings m; CHECK(fromJson(toJson(Settings{}), m)); CHECK(m.toggleRecord == Settings{}.toggleRecord); }
    // Bitrates
    CHECK(bitrateFor(Quality::High, 1920, 1080, 60) > bitrateFor(Quality::Medium, 1920, 1080, 60));
    CHECK(bitrateFor(Quality::High, 1920, 1080, 60) > bitrateFor(Quality::High, 1920, 1080, 30));
    CHECK(bitrateFor(Quality::Ultra, 7680, 4320, 60) <= 150000000u);
    CHECK(bitrateFor(Quality::Low, 320, 240, 15) >= 1000000u);
    CHECK(bitrateFor(Quality::Ultra, 1920, 1080, 60, 5000000u) == 5000000u);
    CHECK(std::string(toString(OutputScale::Native)) == "native");
    CHECK(std::string(toString(OutputScale::P1080)) == "1080p");
    OutputScale sc = OutputScale::Native;
    CHECK(fromString("720p", sc) && sc == OutputScale::P720);
    CHECK_EQ(std::string(extension(Container::MKV)), std::string(".mkv"));
}

#include "micfx.h"
#include <cmath>
#include <random>
static double rms(const std::vector<float>& v, size_t a, size_t b) { double s = 0; for (size_t i = a; i < b; ++i) s += v[i] * v[i]; return std::sqrt(s / (double)(b - a)); }
static void testMicFx() {
    // 1 s of noise, then 1 s of 300 Hz tone + the same noise level, in 480-sample chunks
    std::mt19937 rng(1); std::normal_distribution<float> nd(0.f, 0.01f);
    const size_t N = 96000;
    std::vector<float> in(N), out(N);
    for (size_t i = 0; i < N; ++i) in[i] = nd(rng) + (i >= 48000 ? 0.25f * (float)std::sin(2 * 3.14159265 * 300 * i / 48000.0) : 0.f);
    cr::MicFx fx; cr::MicFxParams p; p.noiseSuppression = true; p.gateDb = -80;
    out = in;
    for (size_t i = 0; i < N; i += 480) fx.process(out.data() + i, 480, p);
    CHECK(fx.delay() == cr::MicFx::kN);
    // noise-only region (after convergence) attenuated by >= 10 dB
    double nin = rms(in, 24000, 46000), nout = rms(out, 24000 + 1024, 46000 + 1024);
    std::printf("micfx: noise %.4f -> %.4f\n", nin, nout);
    CHECK(nout < nin * 0.32);
    // tone survives (aligned by the reported delay): within 2 dB
    double tin = rms(in, 60000, 90000), tout = rms(out, 60000 + 1024, 90000 + 1024);
    CHECK(tout > tin * 0.79 && tout < tin * 1.26);
    // gate at -42 dBFS closes on -40 dBFS noise only... use quieter noise: 0.002 rms (-54 dB)
    std::vector<float> q(N);
    for (size_t i = 0; i < N; ++i) q[i] = nd(rng) * 0.2f + (i >= 48000 ? 0.25f * (float)std::sin(2 * 3.14159265 * 300 * i / 48000.0) : 0.f);
    std::vector<float> qo = q;
    cr::MicFx g2; cr::MicFxParams p2; p2.gateDb = -42;
    for (size_t i = 0; i < N; i += 480) g2.process(qo.data() + i, 480, p2);
    CHECK(rms(qo, 24000, 46000) < rms(q, 24000, 46000) * 0.05);  // closed
    CHECK(rms(qo, 60000, 90000) > rms(q, 60000, 90000) * 0.95);  // open on the tone
    // gain
    std::vector<float> gv(480, 0.5f); cr::MicFx g3; cr::MicFxParams p3; p3.gain = 2.0f; p3.gateDb = -80;
    g3.process(gv.data(), 480, p3);
    CHECK(std::fabs(gv[100] - 1.0f) < 1e-6f);
}

static void testSettingsV2() {
    cr::Settings a; a.sourceKind = "window"; a.sourceExe = "notepad.exe"; a.sourceTitle = "Untitled - Notepad \"x\"";
    a.sourceMonitor = "\\\\.\\DISPLAY2"; a.systemDevice = "{0.0.0.00000000}.{abc}"; a.systemMuted = true; a.micMuted = true;
    a.micVolume = 150; a.noiseSuppression = true; a.noiseGateDb = -50; a.monitoring = true; a.pushToTalk = true;
    a.pttKey = cr::Hotkey{cr::MOD_ALT_, 0x56}; a.channels = 2;
    cr::Settings b; CHECK(cr::fromJson(cr::toJson(a), b));
    CHECK(b.sourceKind == "window" && b.sourceExe == a.sourceExe && b.sourceTitle == a.sourceTitle && b.sourceMonitor == a.sourceMonitor);
    CHECK(b.systemDevice == a.systemDevice && b.systemMuted && b.micMuted && b.micVolume == 150 && b.noiseSuppression);
    CHECK(b.noiseGateDb == -50 && b.monitoring && b.pushToTalk && b.pttKey == a.pttKey && b.channels == 2);
    cr::Settings c; CHECK(cr::fromJson("{\"micVolume\": 900, \"channels\": 7, \"sourceKind\": \"bogus\", \"noiseGateDb\": 5}", c));
    CHECK(c.micVolume == 100 && c.channels == 1 && c.sourceKind == "all" && c.noiseGateDb == -42);
}

static void testSettingsV11() {
    cr::Settings d;
    CHECK(d.container == cr::Container::MKV && d.autoRemux && !d.replayEnabled && d.replaySeconds == 30 && !d.separateTracks);
    CHECK(cr::format(d.saveReplay) == "Ctrl+Shift+F8" && cr::format(d.screenshot) == "Ctrl+Shift+F12");
    CHECK(d.codec == cr::Codec::Auto && d.theme == "auto" && d.accent == "red" && d.screenshotFormat == "png");
    cr::Settings a; a.codec = cr::Codec::HEVC; a.autoRemux = false; a.replayEnabled = true; a.replaySeconds = 600;
    a.saveReplay = cr::Hotkey{cr::MOD_CTRL_ | cr::MOD_ALT_, 0x52}; a.screenshot = cr::Hotkey{cr::MOD_CTRL_ | cr::MOD_ALT_, 0x53};
    a.screenshotFormat = "jpg"; a.separateTracks = true; a.theme = "light"; a.accent = "purple"; a.fileNamePattern = "source_date_time";
    a.scale = cr::OutputScale::P1440;
    cr::Settings b; CHECK(cr::fromJson(cr::toJson(a), b));
    CHECK(b.codec == cr::Codec::HEVC && !b.autoRemux && b.replayEnabled && b.replaySeconds == 600);
    CHECK(b.saveReplay == a.saveReplay && b.screenshot == a.screenshot && b.screenshotFormat == "jpg" && b.separateTracks);
    CHECK(b.theme == "light" && b.accent == "purple" && b.fileNamePattern == "source_date_time" && b.scale == cr::OutputScale::P1440);
    cr::Settings c;
    CHECK(cr::fromJson("{\"replaySeconds\": 2, \"accent\": \"teal\", \"theme\": \"x\", \"codec\": \"vp9\", \"screenshotFormat\": \"gif\","
                       " \"hotkeyToggleRecord\": \"Ctrl+Shift+F9\", \"hotkeySaveReplay\": \"Ctrl+Shift+F9\", \"fileNamePattern\": \"../x\"}", c));
    CHECK(c.replaySeconds == 30 && c.accent == "red" && c.theme == "auto" && c.codec == cr::Codec::Auto && c.screenshotFormat == "png");
    CHECK(c.saveReplay.empty() && c.fileNamePattern == "cast_date_time");  // duplicate of Start/Stop is cleared
    CHECK_EQ(cr::fileNameFor("cast_date_time", 2026, 10, 14, 9, 5, 7, "", 1), std::string("cast_2026-10-14_09-05-07"));
    CHECK_EQ(cr::fileNameFor("date_time", 2026, 10, 14, 9, 5, 7, "", 1), std::string("2026-10-14_09-05-07"));
    CHECK_EQ(cr::fileNameFor("source_date_time", 2026, 1, 2, 3, 4, 5, "Note:pad?.", 1), std::string("Notepad_2026-01-02_03-04-05"));
    CHECK_EQ(cr::fileNameFor("cast_counter", 2026, 1, 2, 3, 4, 5, "", 12), std::string("cast_012"));
}

int main() {
    testSettingsV11();
    testSettingsV2();
    testMicFx();
    testParseFormat();
    testValidateConflicts();
    testSettings();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
