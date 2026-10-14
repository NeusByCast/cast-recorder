#include <cstdio>
#include "../src/core/fsm.h"
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
    std::printf("%d passed, %d failed\n", P, F);
    return F ? 1 : 0;
}
