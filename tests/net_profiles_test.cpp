#include "net_session.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace retro::runner;
thread_local rcore_execution_profiles offer{};
static std::string module_path;
static const rcore_execution_profiles* profiles() { return &offer; }
static rcore_result save(std::uint32_t) { return RCORE_OK; }
static std::uint32_t has(std::uint32_t) { return 1; }
static std::uint32_t oldest(std::uint32_t* out) { *out = 0; return 1; }
static void drop(std::uint32_t) {}
static std::uint32_t depth() { return 40; }
static rcore_result frame() { return RCORE_OK; }
static std::uint64_t hash() { return 0x12345678; }

static bool run(unsigned sequence, unsigned expected, bool force_portable) {
    std::array<std::thread, 2> peers;
    std::atomic<unsigned> ready{0}, done{0}, failures{0};
    const unsigned port = 30000 + (static_cast<unsigned>(getpid()) % 4000) * 4 + sequence * 2;
    for (unsigned seat = 0; seat < 2; ++seat) peers[seat] = std::thread([&, seat] {
        offer = {sizeof offer, seat + 1, 10u + seat, 20u + seat,
                 force_portable && seat == 1 ? 1u : 15u, 0u, "test.fixed-profiles.v1"};
        rcore_core_info info{};
        info.capabilities = RCORE_CAP_DETERMINISTIC | RCORE_CAP_ROLLBACK;
        rcore_core_api api{};
        api.struct_size = sizeof api;
        api.execution_profiles = profiles;
        api.rb_snap_save = save; api.rb_snap_load = save; api.rb_snap_has = has;
        api.rb_snap_oldest = oldest; api.rb_snap_drop_after = drop;
        api.rb_snap_depth = depth; api.run_frame_resim = frame; api.state_hash = hash;
        LoadedCore core;
        core.api = &api;
        core.info = &info;
        core.sha256 = "same-core-build";
        NetParams p;
        p.module_path = module_path;
        p.slot = static_cast<int>(seat);
        p.session_id = sequence + 1;
        p.bind = "127.0.0.1:" + std::to_string(port + seat);
        p.peer = seat ? "127.0.0.1:" + std::to_string(port) : "";
        rcore_init_params init{};
        init.struct_size = sizeof init;
        init.flags = RCORE_INIT_NETPLAY;
        NetSession session;
        ++ready;
        while (ready != 2) std::this_thread::yield();
        std::string why;
        if (!session.prepare(core, p, init, &why) ||
            !(init.flags & RCORE_INIT_PROFILE_AGREED) || init.execution_profile != expected) {
            std::fprintf(stderr, "prepare seat %u failed: %s, selected %u\n", seat, why.c_str(), init.execution_profile);
            ++failures;
        }
        // Keep the transport alive until both have accepted their init params.
        // No core init/load/frame or runtime profile switch occurs in this test.
        ++done;
        while (done != 2) std::this_thread::yield();
        NetParams changed = p;
        ++changed.session_id;
        if (session.start(core, changed, &why)) {
            std::fprintf(stderr, "changed startup configuration was accepted\n");
            ++failures;
            return;
        }
        if (!session.start(core, p, &why)) {
            std::fprintf(stderr, "start failed: %s\n", why.c_str());
            ++failures;
            return;
        }
        const auto started = std::chrono::steady_clock::now();
        unsigned live = 0;
        while (!session.drained() && session.ended().empty() &&
               std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) {
            const auto admitted = session.poll();
            if (admitted != NetSession::Admit::Stall) {
                session.finish(admitted);
                if (admitted == NetSession::Admit::Live && ++live == 20 && seat == 0)
                    session.request_quiesce();
            } else session.wait(1);
        }
        if (!session.drained() || !session.ended().empty()) {
            std::fprintf(stderr, "clean stop failed on seat %u: %s\n", seat, session.ended().c_str());
            ++failures;
        }
    });
    for (auto& peer : peers) peer.join();
    return failures == 0;
}

int main(int argc, char** argv) {
    if (argc > 1) module_path = argv[1];
    const bool ok = run(1, 3, false) && run(2, 0, true);
    std::puts(ok ? "runner startup profiles: PASS" : "runner startup profiles: FAIL");
    return ok ? 0 : 1;
}
