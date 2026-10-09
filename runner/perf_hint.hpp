#pragma once

// A PERFORMANCE HINT SESSION for the frames the runner runs (Android's ADPF,
// APerformanceHint, API 33+). Everywhere else every call is a no-op.
//
// Why. Over the link the runner idles between grants -- the hub paces them by
// its audio queue -- and with a GPU renderer it also waits on the GPU inside a
// frame. Android's frequency governor reads that idle time as low load and
// lowers the CPU clock, and the frame's CPU work then runs slow: measured on a
// Pixel 6 Pro with n64lle's Pokemon Stadium, the Cortex-X1 at 0.85-2.0 GHz
// through the intro under Vulkan (its ceiling 2.8 GHz, thermal status 0) and
// 30-40 VI/s where the work fits 60 at full clock (docs/CORE_RUNNER.md,
// "Performance hints"). A hint session tells the system the deadline (the
// core's frame period) and how long each frame's work actually took, and the
// system sets the clock for the deadline instead of guessing from idle time.
//
// The session names the runner's threads at its first frame (the core's own
// workers included, whichever it has started by then) and is refreshed when
// that set grows, where the API allows it (34+). RETRO_PERF_HINT=0 disables it.
//
// HEADROOM, AND THE FAST CORES (2026-10-09). A target equal to the frame
// period lets the governor settle where the work just fits: in Retro-Launcher
// on a Pixel 6 Pro the X1 held a median 2.05 GHz of its 2.8, the field took
// 15-20 ms against 16.7, and the emulation thread spent 16 % of its time on
// the A76 and A55 cores. So the session asks for a fraction of the period
// (RETRO_PERF_HINT_TARGET, percent, default 70), and the thread that runs the
// frames is pinned to the CPUs with the highest cpuinfo_max_freq
// (RETRO_PIN_FAST=0 leaves it to the scheduler). Battery and heat are not
// reasons to hold back here (the owner's ruling): a game wants its frames.

#include <cstdint>
#include <string>

namespace retro::runner {

class PerfHint {
public:
    PerfHint() = default;
    PerfHint(const PerfHint&) = delete;
    PerfHint& operator=(const PerfHint&) = delete;
    ~PerfHint();

    // One frame's work, with the deadline it had (the core's frame period).
    // The first call opens the session; a changed period updates its target.
    void frame(std::uint64_t work_ns, std::uint64_t target_ns);

    // One line for runner.log, once: what the session is, or why there is none.
    // Empty until the first frame().
    const std::string& status() const { return status_; }

private:
    void open(std::uint64_t target_ns);
    std::uint64_t scaled(std::uint64_t period_ns) const;
    void refresh_threads();

    bool tried_ = false;
    void* session_ = nullptr;
    std::uint64_t target_ns_ = 0;
    std::uint64_t frames_ = 0;
    std::size_t threads_ = 0;
    unsigned target_pct_ = 70;
    std::string status_;
};

} // namespace retro::runner
