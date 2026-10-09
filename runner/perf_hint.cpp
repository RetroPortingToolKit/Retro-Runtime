#include "perf_hint.hpp"

#if defined(__ANDROID__)
#include <android/api-level.h>
#include <dirent.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

} // namespace

PerfHint::~PerfHint() {
    if (session_) adpf().close(session_);
}

void PerfHint::open(std::uint64_t target_ns) {
    const int api = android_get_device_api_level();
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
    char line[160];
    std::snprintf(line, sizeof line,
                  "perf_hint: ADPF session on %zu thread(s), target %.3f ms (API %d%s)",
                  threads_, target_ns / 1e6, api,
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
    if (target_ns && target_ns != target_ns_) {
        if (a.update_target(session_, static_cast<std::int64_t>(target_ns)) == 0) target_ns_ = target_ns;
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
