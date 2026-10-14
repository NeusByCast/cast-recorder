// Recording lifecycle used by the UI and the recorder thread. Platform-independent so it can be unit-tested.
#pragma once
#include <atomic>
#include <cstdint>

namespace cr {

enum class RecState : uint8_t {
    Idle = 0,
    Recording = 1,
    Stopping = 2,   // stop requested; still capturing/writing
    Finishing = 3,  // loop exited; Finalize / remux in progress
};

inline const char* toString(RecState s) {
    switch (s) {
        case RecState::Recording: return "recording";
        case RecState::Stopping: return "stopping";
        case RecState::Finishing: return "finishing";
        default: return "idle";
    }
}

// Tiny atomic state machine. All transitions are idempotent where it matters (double-stop is fine).
struct RecFsm {
    std::atomic<RecState> state{RecState::Idle};

    bool tryStart() {
        auto e = RecState::Idle;
        return state.compare_exchange_strong(e, RecState::Recording);
    }
    // Request stop from Recording (or ignore if already stopping/finishing/idle).
    bool requestStop() {
        auto cur = state.load();
        if (cur == RecState::Recording)
            return state.compare_exchange_strong(cur, RecState::Stopping);
        return cur == RecState::Stopping || cur == RecState::Finishing;  // already underway
    }
    bool beginFinish() {
        auto cur = state.load();
        if (cur == RecState::Stopping || cur == RecState::Recording)
            return state.compare_exchange_strong(cur, RecState::Finishing) || state.load() == RecState::Finishing;
        return cur == RecState::Finishing;
    }
    void toIdle() { state.store(RecState::Idle); }
    RecState get() const { return state.load(); }
    bool busy() const { return get() != RecState::Idle; }
    bool isRecording() const { return get() == RecState::Recording; }
};

}  // namespace cr
