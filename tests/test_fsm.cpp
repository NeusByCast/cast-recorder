#include <cstdio>
#include "../src/core/fsm.h"
#include "../src/core/nv12.h"
#include <vector>
using namespace cr;
static int F=0,P=0;
#define T(n,c) do{ if(c) ++P; else { ++F; std::printf("FAIL %s\n",n);} }while(0)
int main() {
    RecFsm f;
    T("idle", f.get()==RecState::Idle && !f.busy());
    T("start", f.tryStart() && f.isRecording());
    T("no double start", !f.tryStart());
    T("pause", f.pause() && f.isPaused());
    T("pause while paused fails", !f.pause());
    T("resume", f.resume() && f.isRecording());
    T("resume while recording fails", !f.resume());
    T("stop", f.requestStop() && f.get()==RecState::Stopping);
    T("stop idempotent", f.requestStop() && f.get()==RecState::Stopping);
    T("finish", f.beginFinish() && f.get()==RecState::Finishing);
    T("finish idempotent", f.beginFinish());
    T("busy while finishing", f.busy() && !f.isRecording());
    f.toIdle();
    T("idle again", f.get()==RecState::Idle && !f.busy());
    T("stop while idle is false", !f.requestStop());
    RecFsm g;
    T("fresh start", g.tryStart());
    g.pause();
    T("stop from paused", g.requestStop() && g.get()==RecState::Stopping);
    g.beginFinish(); g.toIdle();
    T("restart after full cycle", g.tryStart() && g.isRecording());
    {   // CPU NV12 fallback: white/black/red, downscale 4x2 -> 2x2
        std::vector<uint8_t> src(4 * 2 * 4);
        for (int i = 0; i < 8; ++i) { uint8_t* p = &src[i * 4]; p[0] = p[1] = p[2] = 255; p[3] = 255; }
        std::vector<uint8_t> out(2 * 2 * 3 / 2);
        bgraToNv12(src.data(), 4, 2, 16, out.data(), 2, 2);
        T("white Y=235", out[0] == 235 && out[3] == 235);
        T("white UV=128", out[4] == 128 && out[5] == 128);
        for (auto& b : src) b = 0;
        bgraToNv12(src.data(), 4, 2, 16, out.data(), 2, 2);
        T("black Y=16", out[0] == 16);
        for (int i = 0; i < 8; ++i) { uint8_t* p = &src[i * 4]; p[0] = 0; p[1] = 0; p[2] = 255; }
        bgraToNv12(src.data(), 4, 2, 16, out.data(), 2, 2);
        T("red V high", out[5] > 200 && out[4] < 128);
    }
    std::printf("%d passed, %d failed\n", P, F);
    return F ? 1 : 0;
}
