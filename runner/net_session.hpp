#pragma once

// Netplay in the runner (rcore rev 4, the ruling of 2026-09-25: THE RUNNER
// OWNS THE SESSION). One binding of recomp-net's episode driver (rb_driver.h)
// for every core: the core supplies its snapshot ring, a presentation-free
// frame and its digests (rev 4), and its input row (rev 6); the runner owns
// the transport, the identity, the published rows and the clock.
//
// Transport: the host listens and every guest dials it -- recomp-net's LAN
// hub when there are more than two seats (the host relays each guest's
// datagrams to the others), a direct pair when there are two. Or every peer
// dials the lobby server's relay, when one is named (the fallback).
//
// The loop is INCREMENTAL replay: poll() admits one live or one replayed
// frame; the caller runs it (run_frame / run_frame_resim) and calls
// finish(). Only published rows reach the core: seat_pad() is what input_get
// returns for the frame being run (recomp-ai-rules NETPLAY.md §2).

#include "core_library.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace retro::runner {

struct NetParams {
    std::string module_path;   // the netplay module (module builds only)
    int slot = 0;              // this peer's seat
    int slots = 2;             // seats in the match
    std::uint32_t occupied = 0; // bit i = seat i has a player; 0 = all
    int delay = 2;             // input delay, frames
    int prediction = 0;        // 0 = the driver's rule (4 + delay, 6..16)
    std::uint32_t session_id = 1;
    std::string bind = "0.0.0.0:7777";
    std::string peer;          // guests: the host (or relay); host: empty
    bool via_relay = false;    // every peer dials `peer`, a server relay
    std::uint64_t epoch_s = 0; // the session's agreed clock epoch (Unix seconds)
    // The match key's content lines (what every peer must hold identically):
    // content hashes, NETPLAY options, accessories. The runner adds its own.
    std::vector<std::string> content;
};

struct NetStats {
    std::uint64_t live = 0, replayed = 0, stalls = 0;
    std::uint32_t episodes = 0, desyncs = 0, rtt_ms = 0, confirmed_through = 0;
};

class NetSession {
public:
    NetSession();
    ~NetSession();
    NetSession(const NetSession&) = delete;
    NetSession& operator=(const NetSession&) = delete;

    // True when this runner was built with recomp-net.
    static bool compiled_in();
    // Opens the transport and starts the driver. The core must be loaded and
    // at the boundary frame 1 runs from. False with *error saying why (a core
    // without ROLLBACK, a bad address, the driver's refusal).
    bool start(const LoadedCore& core, const NetParams& p, std::string* error);

    enum class Admit { Stall, Live, Replay };
    // The local pad for the frames to come (sampled into the session).
    void stage_local(const struct rcore_pad& pad);
    // Receive, then admit. Stall: nothing to run yet (the caller waits and
    // asks again).
    Admit poll();
    void finish(Admit a);
    // Park up to `ms` for a datagram (between stalled polls).
    void wait(int ms);

    // What input_get returns for `seat` in the frame being run.
    const struct rcore_pad& seat_pad(std::uint32_t seat) const;
    // The frame (tick) the rows last published belong to.
    std::uint32_t tick() const;
    bool in_resim() const;

    // The match is over: refused, a peer gone, or the transport down. Empty
    // while it runs; otherwise one sentence.
    std::string ended() const;
    NetStats stats() const;
    // Coordinated stop (the driver drains in-flight episodes on every peer).
    void request_quiesce();
    bool drained() const;
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace retro::runner
