// Ownership hand-off between the recorder worker and abort(). Platform-independent so it can be unit-tested
// (including under ASan/TSan) without Media Foundation.
#pragma once
#include <atomic>
#include <mutex>

namespace cr {

// Exactly one of {worker, abort()} may publish the result of a session and signal Idle.
struct FinishClaim {
    std::mutex m;
    bool claimed = false;
    unsigned gen = 0;
    // New session: returns its generation.
    unsigned arm() { std::lock_guard<std::mutex> l(m); claimed = false; return ++gen; }
    // First caller for generation g wins; later or stale callers get false and must not touch shared state.
    template <class F> bool claim(unsigned g, F&& publish) {
        std::lock_guard<std::mutex> l(m);
        if (claimed || g != gen) return false;
        claimed = true;
        publish();
        return true;
    }
};

// Process-wide record of Media Foundation / capture work that was abandoned on a stuck thread.
// Once poisoned, MFShutdown/CoUninitialize are skipped and no new session may start until process exit.
struct AbandonLatch {
    std::atomic<bool> poisoned{false};
    std::atomic<int> live{0};  // abandoned helper threads still running
    void abandon() { poisoned = true; live.fetch_add(1); }
    void helperFinished() { live.fetch_sub(1); }
    bool canStart() const { return !poisoned.load(); }
};

}  // namespace cr
