#include "perf_hint.hpp"

#if defined(__ANDROID__)
#include <android/api-level.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sched.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>
#endif

namespace retro::runner {

#if defined(__ANDROID__)
namespace {

// NDK <android/performance_hint.h>, resolved at run time: the APK's floor is
// API 30 and these exist from 33 (APerformanceHint_setThreads from 34), so a
// link-time reference would refuse to load on 30-32.
struct Adpf {
    void* (*get_manager)() = nullptr;
    void* (*create)(void*, const std::int32_t*, std::size_t, std::int64_t) = nullptr;
    int (*update_target)(void*, std::int64_t) = nullptr;
    int (*report)(void*, std::int64_t) = nullptr;
    void (*close)(void*) = nullptr;
    int (*set_threads)(void*, const std::int32_t*, std::size_t) = nullptr; // 34+
    bool usable() const { return get_manager && create && update_target && report && close; }
};

const Adpf& adpf() {
    static const Adpf a = [] {
        Adpf r;
        void* h = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!h) return r;
        auto sym = [h](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(h, name));
        };
        sym(r.get_manager, "APerformanceHint_getManager");
        sym(r.create, "APerformanceHint_createSession");
        sym(r.update_target, "APerformanceHint_updateTargetWorkDuration");
        sym(r.report, "APerformanceHint_reportActualWorkDuration");
        sym(r.close, "APerformanceHint_closeSession");
        sym(r.set_threads, "APerformanceHint_setThreads");
        return r;
    }();
    return a;
}

// Every thread of this process: the runner's, and the core's own workers.
std::vector<std::int32_t> process_threads() {
    std::vector<std::int32_t> tids;
    if (DIR* d = opendir("/proc/self/task")) {
        while (const dirent* e = readdir(d)) {
            char* end = nullptr;
            const long v = std::strtol(e->d_name, &end, 10);
            if (end != e->d_name && *end == '\0' && v > 0) tids.push_back(static_cast<std::int32_t>(v));
        }
        closedir(d);
    }
    return tids;
}

/* Pin the calling thread to the CPUs whose cpuinfo_max_freq is the highest
(the big cluster, read from sysfs so it holds on any SoC). A description for
runner.log, or why not. */
std::string pin_to_fastest_cpus() {
    if (const char* e = std::getenv("RETRO_PIN_FAST"); e && std::strcmp(e, "0") == 0) {
        return "affinity: left to the scheduler (RETRO_PIN_FAST=0)";
    }
    long best = 0;
    std::vector<int> cpus;
    for (int c = 0; c < CPU_SETSIZE; ++c) {
        char path[96];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        std::FILE* f = std::fopen(path, "r");
        if (!f) {
            if (c > 64) break;
            continue;
        }
        long khz = 0;
        if (std::fscanf(f, "%ld", &khz) != 1) khz = 0;
        std::fclose(f);
        if (khz > best) {
            best = khz;
            cpus.clear();
        }
        if (khz == best && khz > 0) cpus.push_back(c);
    }
    if (cpus.empty()) return "affinity: no cpufreq in sysfs; left to the scheduler";
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cpus) CPU_SET(c, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0) {
        return std::string("affinity: sched_setaffinity refused (") + std::strerror(errno) + ")";
    }
    std::string list;
    for (int c : cpus) list += (list.empty() ? "" : ",") + std::to_string(c);
    return "affinity: the frame thread on cpu " + list + " (" + std::to_string(best / 1000) + " MHz)";
}

} // namespace

std::uint64_t PerfHint::scaled(std::uint64_t period_ns) const {
    return period_ns * target_pct_ / 100;
}

PerfHint::~PerfHint() {
    if (session_) adpf().close(session_);
}

void PerfHint::open(std::uint64_t period_ns) {
    const int api = android_get_device_api_level();
    const std::string pin = pin_to_fastest_cpus();
    if (const char* e = std::getenv("RETRO_PERF_HINT_TARGET")) {
        const long v = std::strtol(e, nullptr, 10);
        if (v >= 10 && v <= 100) target_pct_ = static_cast<unsigned>(v);
    }
    const std::uint64_t target_ns = scaled(period_ns);
    std::fprintf(stderr, "perf_hint: %s\n", pin.c_str());
    if (const char* e = std::getenv("RETRO_PERF_HINT"); e && std::strcmp(e, "0") == 0) {
        status_ = "perf_hint: off (RETRO_PERF_HINT=0)";
        return;
    }
    const Adpf& a = adpf();
    if (!a.usable()) {
        status_ = "perf_hint: unavailable (API " + std::to_string(api) +
                  ": no APerformanceHint; it needs 33)";
        return;
    }
    void* mgr = a.get_manager();
    const std::vector<std::int32_t> tids = process_threads();
    session_ = mgr && !tids.empty()
                   ? a.create(mgr, tids.data(), tids.size(), static_cast<std::int64_t>(target_ns))
                   : nullptr;
    if (!session_) {
        status_ = "perf_hint: unavailable (API " + std::to_string(api) +
                  ": the system refused a session; hint sessions may be off on this device)";
        return;
    }
    target_ns_ = target_ns;
    threads_ = tids.size();
    char line[224];
    std::snprintf(line, sizeof line,
                  "perf_hint: ADPF session on %zu thread(s), target %.3f ms (%u %% of the %.3f ms period; API %d%s)",
                  threads_, target_ns / 1e6, target_pct_, period_ns / 1e6, api,
                  a.set_threads ? "" : "; the thread set is fixed at the first frame");
    status_ = line;
}

void PerfHint::refresh_threads() {
    const Adpf& a = adpf();
    if (!session_ || !a.set_threads) return;
    const std::vector<std::int32_t> tids = process_threads();
    if (tids.size() <= threads_) return;
    if (a.set_threads(session_, tids.data(), tids.size()) == 0) {
        threads_ = tids.size();
        std::fprintf(stderr, "perf_hint: the session now covers %zu thread(s)\n", threads_);
    }
}

void PerfHint::frame(std::uint64_t work_ns, std::uint64_t target_ns) {
    if (!tried_) {
        tried_ = true;
        open(target_ns);
    }
    if (!session_) return;
    const Adpf& a = adpf();
    if (target_ns && scaled(target_ns) != target_ns_) {
        const std::uint64_t t = scaled(target_ns);
        if (a.update_target(session_, static_cast<std::int64_t>(t)) == 0) target_ns_ = t;
    }
    if (work_ns && a.report(session_, static_cast<std::int64_t>(work_ns)) != 0) {
        // EPIPE: the system closed the session (e.g. hint sessions turned off).
        std::fprintf(stderr, "perf_hint: the session stopped accepting reports; closed\n");
        a.close(session_);
        session_ = nullptr;
        return;
    }
    /* the core may start workers after the first frame (a raster pool on its
    first draw): look again now and then, cheaply */
    if (++frames_ % 120 == 0) refresh_threads();
}

#else // not Android: no hint API to speak to

PerfHint::~PerfHint() = default;
void PerfHint::open(std::uint64_t) {}
void PerfHint::refresh_threads() {}
void PerfHint::frame(std::uint64_t, std::uint64_t) {}

#endif

} // namespace retro::runner
