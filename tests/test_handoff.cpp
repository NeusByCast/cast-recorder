// Stuck-Finalize hand-off logic: exactly one publisher, abandoned work poisons further starts.
#include <cstdio>
#include <thread>
#include <vector>
#include <chrono>
#include "../src/core/handoff.h"
#include "../src/core/fsm.h"
using namespace cr;
static int F = 0, P = 0;
#define T(n, c) do { if (c) ++P; else { ++F; std::printf("FAIL %s\n", n); } } while (0)

int main() {
    {   // single claim
        FinishClaim c; unsigned g = c.arm(); int n = 0;
        T("first claim wins", c.claim(g, [&] { ++n; }));
        T("second claim loses", !c.claim(g, [&] { ++n; }));
        T("publish ran once", n == 1);
        unsigned g2 = c.arm();
        T("stale gen loses", !c.claim(g, [&] { ++n; }));
        T("new gen wins", c.claim(g2, [&] { ++n; }) && n == 2);
    }
    {   // race: worker vs abort, many rounds; result written exactly once per round
        int bad = 0;
        for (int round = 0; round < 2000; ++round) {
            FinishClaim c; unsigned g = c.arm();
            int* result = new int(0);  // heap so ASan catches double/late writes after free
            std::atomic<int> wins{0};
            std::thread a([&] { if (c.claim(g, [&] { *result = 1; })) ++wins; });
            std::thread b([&] { if (c.claim(g, [&] { *result = 2; })) ++wins; });
            a.join(); b.join();
            if (wins != 1 || *result == 0) ++bad;
            delete result;
        }
        T("exactly one winner under race", bad == 0);
    }
    {   // simulated stuck Finalize: helper owns its refs by value, main abandons, no second start
        AbandonLatch L; RecFsm fsm; FinishClaim c;
        T("can start initially", L.canStart() && fsm.tryStart());
        unsigned g = c.arm();
        struct Ref { std::atomic<int> rc{1}; };
        Ref* writer = new Ref;              // stands in for IMFSinkWriter
        writer->rc++;                       // helper's by-value ref
        std::atomic<bool> finDone{false};
        std::thread fin([writer, &finDone] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));  // "hung" Finalize
            writer->rc.load();              // touches the object after main gave up: must still be alive
            finDone = true;
            if (--writer->rc == 0) delete writer;
        });
        fsm.requestStop(); fsm.beginFinish();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // budget expires
        T("finalize still running", !finDone);
        L.abandon();                        // production: main would leak its ref (no Release) and detach
        // Keep main's Ref* alive until the helper finishes (proves no UAF). Production leaks forever.
        fin.detach();
        bool pub = c.claim(g, [&] { fsm.toIdle(); });
        T("worker publishes timeout", pub);
        T("ui back to idle", !fsm.busy());
        T("second start refused after abandon", !L.canStart());
        for (int i = 0; i < 100 && !finDone; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        T("late finalize completed without UAF", finDone.load());
        L.helperFinished();
        T("no live abandoned helpers", L.live == 0);
        T("still poisoned until exit", !L.canStart());
        if (--writer->rc == 0) delete writer;  // test-only: free main's ref after helper is done
    }
    std::printf("%d passed, %d failed\n", P, F);
    return F ? 1 : 0;
}
