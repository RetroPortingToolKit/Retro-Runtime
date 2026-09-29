#include "net_session.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>

#if defined(RETRO_RUNNER_NETPLAY)
extern "C" {
#include "recomp_net/rb_driver.h"
#include "recomp_net/session.h"
}
#endif

namespace retro::runner {

namespace {

// The runner's own netplay identity: bump when anything here changes what a
// peer simulates (row codec, clock, publish order).
constexpr const char* kRunnerNetVersion = "retro-core-runner netplay 1";

std::uint32_t fnv1a(const std::string& s, std::uint32_t h = 2166136261u) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

std::uint32_t fold(std::uint64_t v) { return static_cast<std::uint32_t>(v ^ (v >> 32)); }

std::uint32_t steady_ms() {
    using namespace std::chrono;
    return static_cast<std::uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Without the core's own codec (rev 6): buttons 0-15 and the left stick.
void generic_from_pad(const rcore_pad& p, rcore_net_row& r) {
    r.buttons = static_cast<std::uint16_t>(p.buttons & 0xFFFFu);
    r.stick_x = static_cast<std::int8_t>(p.axes[RCORE_AXIS_LX] >> 8);
    r.stick_y = static_cast<std::int8_t>(p.axes[RCORE_AXIS_LY] >> 8);
}
void generic_to_pad(const rcore_net_row& r, rcore_pad& p) {
    p = rcore_pad{};
    p.struct_size = sizeof(rcore_pad);
    p.buttons = r.buttons;
    p.axes[RCORE_AXIS_LX] = static_cast<std::int16_t>(r.stick_x * 256);
    p.axes[RCORE_AXIS_LY] = static_cast<std::int16_t>(r.stick_y * 256);
}

} // namespace

#if defined(RETRO_RUNNER_NETPLAY)

struct NetSession::Impl {
    const LoadedCore* core = nullptr;
    const rcore_core_api* api = nullptr;
    NetParams p;
    RNetSession* session = nullptr;
    RNetRbDriver* drv = nullptr;
    int slot = 0, slots = 2, delay = 2, prediction = 0;
    rcore_pad staged{};
    std::array<rcore_pad, RCORE_MAX_SEATS> pads{};
    std::uint32_t last_tick = 0;
    bool resim = false;
    bool codec = false;
    std::string ended;
    std::uint32_t started_ms = 0;
    bool was_running = false;
    NetStats stats;

    void from_pad(const rcore_pad& pad, rcore_net_row& row) const {
        if (codec) api->net_row_from_pad(&pad, &row);
        else generic_from_pad(pad, row);
    }
    void to_pad(const rcore_net_row& row, rcore_pad& pad) const {
        if (codec) api->net_row_to_pad(&row, &pad);
        else generic_to_pad(row, pad);
    }
    // A row every peer reads the same way: through the core's own codec.
    void canon(RNetRbFrame& f) const {
        rcore_net_row row{f.buttons, f.stick_x, f.stick_y};
        rcore_pad pad{};
        to_pad(row, pad);
        from_pad(pad, row);
        f.buttons = row.buttons;
        f.stick_x = row.stick_x;
        f.stick_y = row.stick_y;
        f.analog = 0;
    }
    static Impl* of(void* ctx) { return static_cast<Impl*>(ctx); }

    // ---- RNetRbHost ----
    static int snap_save(void* c, std::uint32_t t) {
        return of(c)->api->rb_snap_save(t) == RCORE_OK ? 1 : 0;
    }
    static int snap_load(void* c, std::uint32_t t) {
        return of(c)->api->rb_snap_load(t) == RCORE_OK ? 1 : 0;
    }
    static int snap_has(void* c, std::uint32_t t) { return of(c)->api->rb_snap_has(t) ? 1 : 0; }
    static int snap_oldest(void* c, std::uint32_t* o) {
        return of(c)->api->rb_snap_oldest(o) ? 1 : 0;
    }
    static void snap_drop_after(void* c, std::uint32_t t) { of(c)->api->rb_snap_drop_after(t); }
    static void publish(void* c, std::uint32_t tick, const RNetRbFrame* rows, int n, int) {
        Impl* s = of(c);
        s->last_tick = tick;
        for (std::uint32_t seat = 0; seat < RCORE_MAX_SEATS; ++seat) {
            rcore_pad& pad = s->pads[seat];
            pad = rcore_pad{};
            pad.struct_size = sizeof(rcore_pad);
            const bool in_match = static_cast<int>(seat) < n && rows &&
                                  (s->p.occupied == 0 || (s->p.occupied >> seat) & 1u);
            if (!in_match) continue; // settled: an empty seat has no controller
            const rcore_net_row row{rows[seat].buttons, rows[seat].stick_x, rows[seat].stick_y};
            s->to_pad(row, pad);
            pad.struct_size = sizeof(rcore_pad);
            pad.connected = 1;
        }
    }
    static void resim_begin(void* c) { of(c)->resim = true; }
    static void resim_end(void* c) { of(c)->resim = false; }
    static std::uint32_t digest_master(void* c) {
        const rcore_core_api* a = of(c)->api;
        return fold(a->state_hash ? a->state_hash() : 0);
    }
    static void digest_parts(void* c, RNetRbDigestParts* out) {
        if (!out) return;
        const rcore_core_api* a = of(c)->api;
        std::uint64_t parts[3] = {0, 0, 0};
        const char* names[3] = {nullptr, nullptr, nullptr};
        if (a->state_hash_parts) a->state_hash_parts(parts, names);
        out->master = fold(a->state_hash ? a->state_hash() : 0);
        for (int i = 0; i < 3; ++i) out->part[i] = fold(parts[i]);
    }
    static void decode_sample(void* c, int, const RNetInputSample* in, RNetRbFrame* out) {
        if (!in || !out) return;
        out->buttons = static_cast<std::uint16_t>(in->bytes[0] | (in->bytes[1] << 8));
        out->stick_x = static_cast<std::int8_t>(in->bytes[2]);
        out->stick_y = static_cast<std::int8_t>(in->bytes[3]);
        of(c)->canon(*out);
    }
    static void sanitize_row(void* c, int, RNetRbFrame* row) {
        if (row) of(c)->canon(*row);
    }
    static void neutral_row(void* c, int, RNetRbFrame* out) {
        if (!out) return;
        rcore_pad pad{};
        pad.struct_size = sizeof(rcore_pad);
        rcore_net_row row{};
        of(c)->from_pad(pad, row);
        out->buttons = row.buttons;
        out->stick_x = row.stick_x;
        out->stick_y = row.stick_y;
        out->analog = 0;
    }
    static void boot_digest_noted(void* c) {
        std::uint64_t parts[3] = {0, 0, 0};
        const char* names[3] = {"?", "?", "?"};
        const rcore_core_api* a = of(c)->api;
        if (a->state_hash_parts) a->state_hash_parts(parts, names);
        std::fprintf(stderr, "runner_netplay: RB boot parts %s=%016llx %s=%016llx %s=%016llx\n",
                     names[0], static_cast<unsigned long long>(parts[0]), names[1],
                     static_cast<unsigned long long>(parts[1]), names[2],
                     static_cast<unsigned long long>(parts[2]));
    }
    static void return_to_lobby(void* c) {
        Impl* s = of(c);
        const char* why = s->drv ? rnet_rb_driver_refusal(s->drv) : nullptr;
        s->ended = std::string("the match was refused: ") + (why ? why : "unknown");
    }
    static void log(void*, const char* line) {
        if (line) std::fputs(line, stderr);
    }
    static std::uint32_t now_ms(void*) { return steady_ms(); }

    // ---- the session's host vtable: the local pad ----
    static void sample_local(rnet_u32 tick, RNetInputSample* out, void* c) {
        if (!out) return;
        Impl* s = of(c);
        rcore_net_row row{};
        s->from_pad(s->staged, row);
        std::memset(out, 0, sizeof *out);
        out->tick = tick;
        out->size = 4;
        out->bytes[0] = static_cast<rnet_u8>(row.buttons & 0xFF);
        out->bytes[1] = static_cast<rnet_u8>(row.buttons >> 8);
        out->bytes[2] = static_cast<rnet_u8>(row.stick_x);
        out->bytes[3] = static_cast<rnet_u8>(row.stick_y);
        out->valid = 1;
    }
    static void publish_unused(rnet_u32, const RNetInputSample*, int, void*) {}
};

NetSession::NetSession() : impl_(std::make_unique<Impl>()) {}
NetSession::~NetSession() { shutdown(); }
bool NetSession::compiled_in() { return true; }

bool NetSession::start(const LoadedCore& core, const NetParams& p, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    Impl& s = *impl_;
    const rcore_core_api* a = core.api;
    if (!(core.info->capabilities & RCORE_CAP_DETERMINISTIC))
        return fail("the core does not declare DETERMINISTIC, so it cannot play netplay");
    if (!(core.info->capabilities & RCORE_CAP_ROLLBACK) || !a->rb_snap_save || !a->rb_snap_load ||
        !a->rb_snap_has || !a->rb_snap_oldest || !a->rb_snap_drop_after || !a->run_frame_resim ||
        !a->state_hash)
        return fail("the core does not declare ROLLBACK with every rev 4 slot (rb_snap_*, "
                    "run_frame_resim, state_hash)");
    if (p.slots < 2 || p.slots > static_cast<int>(RCORE_MAX_SEATS) || p.slot < 0 ||
        p.slot >= p.slots)
        return fail("seat " + std::to_string(p.slot) + " of " + std::to_string(p.slots) +
                    " is not a seat");
    s.codec = RCORE_HAS(a, rcore_core_api, net_row_to_pad) && a->net_row_from_pad &&
              a->net_row_to_pad;
    if (!s.codec) {
        // A second stick or a trigger does not fit a generic row (rev 4, amended).
        std::uint32_t n = 0;
        const rcore_input_descriptor* d = a->input_descriptors ? a->input_descriptors(&n) : nullptr;
        for (std::uint32_t i = 0; d && i < n; ++i) {
            if (d[i].axis && d[i].axis - 1 != RCORE_AXIS_LX && d[i].axis - 1 != RCORE_AXIS_LY)
                return fail(std::string("the core's '") + (d[i].label ? d[i].label : "?") +
                            "' rides an axis a generic netplay row cannot carry, and the core "
                            "has no net_row codec (rcore rev 6)");
        }
    }
    s.core = &core;
    s.api = a;
    s.p = p;
    s.slot = p.slot;
    s.slots = p.slots;
    s.delay = p.delay;
    s.prediction = p.prediction;
    s.staged = rcore_pad{};
    s.staged.struct_size = sizeof(rcore_pad);
    s.started_ms = steady_ms();

    // ---- identity: what every peer must hold identically ----
    std::uint32_t build_fp = fnv1a(kRunnerNetVersion);
    build_fp = fnv1a(core.sha256, build_fp);
    std::uint32_t content_fp = fnv1a("slots " + std::to_string(p.slots) + " occupied " +
                                     std::to_string(p.occupied) + " epoch " +
                                     std::to_string(p.epoch_s));
    for (const std::string& l : p.content) {
        content_fp = fnv1a(l + "\n", content_fp);
        std::fprintf(stderr, "runner_netplay: IDENT content %s\n", l.c_str());
    }
    std::fprintf(stderr, "runner_netplay: IDENT build=%08x content=%08x\n", build_fp, content_fp);

    // ---- the wire ----
    RNetConfig rc;
    rnet_config_init_defaults(&rc);
    rc.slot_count = static_cast<rnet_u8>(p.slots);
    rc.local_slot = static_cast<rnet_u8>(p.slot);
    rc.input_delay = static_cast<rnet_u8>(p.delay);
    rc.session_id = p.session_id;
    rc.occupied_mask = p.occupied;
    RNetHostVTable hv{};
    hv.sample_local = Impl::sample_local;
    hv.publish = Impl::publish_unused;
    hv.ctx = &s;
    s.session = rnet_session_create(&rc, &hv);
    if (!s.session) return fail("recomp-net refused the session configuration");
    int rc_start;
    std::string how;
    if (p.via_relay) {
        rc_start = rnet_session_start_lan(s.session, p.bind.c_str(), p.peer.c_str());
        how = "via the relay " + p.peer;
    } else if (p.slot == 0 && p.slots > 2) {
        rc_start = rnet_session_start_lan_hub(s.session, p.bind.c_str());
        how = "host, relaying for " + std::to_string(p.slots - 1) + " guests on " + p.bind;
    } else if (p.slot == 0) {
        rc_start = rnet_session_start_lan(s.session, p.bind.c_str(), "");
        how = "host on " + p.bind;
    } else {
        rc_start = rnet_session_start_lan(s.session, p.bind.c_str(), p.peer.c_str());
        how = "guest, dialling the host " + p.peer;
    }
    if (rc_start != 0) {
        rnet_session_destroy(s.session);
        s.session = nullptr;
        return fail("could not open the UDP transport (" + how + ")");
    }

    // ---- the driver ----
    s.drv = rnet_rb_driver_create();
    if (!s.drv) return fail("rnet_rb_driver_create failed");
    rnet_rb_driver_set_identity(s.drv, build_fp, content_fp);
    static const char* kNames[3] = {"part0", "part1", "part2"};
    const char* names[3] = {kNames[0], kNames[1], kNames[2]};
    if (a->state_hash_parts) {
        std::uint64_t parts[3];
        const char* n[3] = {nullptr, nullptr, nullptr};
        a->state_hash_parts(parts, n);
        for (int i = 0; i < 3; ++i)
            if (n[i]) names[i] = n[i];
    }
    RNetRbDriverConfig dc{};
    dc.session = &s.session;
    dc.local_slot = &s.slot;
    dc.slot_count = &s.slots;
    dc.input_delay = &s.delay;
    dc.input_prediction = s.prediction > 0 ? &s.prediction : nullptr;
    dc.occupied_mask = p.occupied;
    dc.replay_mode = RNET_RB_REPLAY_INCREMENTAL;
    for (int i = 0; i < 3; ++i) dc.part_names[i] = names[i];
    dc.snap_depth = a->rb_snap_depth ? a->rb_snap_depth() : 0;
    dc.log_prefix = "runner_netplay";
    dc.env_alias = "RETRO_RB";
    // The validation injector (RETRO_RB_FORCE_MISPREDICT) must flip bits the
    // core's pad carries: the driver's default 0x0040 is masked on N64, so an
    // injected mispredict there would open episodes the guest never saw.
    for (int bit = 15, found = 0; bit >= 0 && found < 2; --bit) {
        RNetRbFrame f{};
        f.buttons = static_cast<std::uint16_t>(1u << bit);
        s.canon(f);
        if (f.buttons == (1u << bit)) {
            dc.inject_flip_bits = static_cast<std::uint16_t>(dc.inject_flip_bits | (1u << bit));
            ++found;
        }
    }
    RNetRbHost h{};
    h.ctx = &s;
    h.snap_save = Impl::snap_save;
    h.snap_load = Impl::snap_load;
    h.snap_has = Impl::snap_has;
    h.snap_oldest = Impl::snap_oldest;
    h.snap_drop_after = Impl::snap_drop_after;
    h.publish = Impl::publish;
    h.run_tick = nullptr;
    h.resim_begin = Impl::resim_begin;
    h.resim_end = Impl::resim_end;
    h.digest_master = Impl::digest_master;
    h.digest_parts = Impl::digest_parts;
    h.decode_sample = Impl::decode_sample;
    h.sanitize_row = Impl::sanitize_row;
    h.neutral_row = Impl::neutral_row;
    h.admit_sample = nullptr;
    h.boot_digest_noted = Impl::boot_digest_noted;
    h.request_return_to_lobby = Impl::return_to_lobby;
    h.log = Impl::log;
    h.now_ms = Impl::now_ms;
    if (!rnet_rb_driver_start(s.drv, &dc, &h)) {
        rnet_rb_driver_destroy(s.drv);
        s.drv = nullptr;
        return fail("the episode driver refused to start (its log says why)");
    }
    std::fprintf(stderr,
                 "runner_netplay: started %s; seat %d of %d, occupied 0x%x, delay %d, session %u, "
                 "epoch %llu, row codec %s\n",
                 how.c_str(), p.slot, p.slots, p.occupied, p.delay, p.session_id,
                 static_cast<unsigned long long>(p.epoch_s), s.codec ? "the core's" : "generic");
    return true;
}

void NetSession::stage_local(const rcore_pad& pad) { impl_->staged = pad; }

NetSession::Admit NetSession::poll() {
    Impl& s = *impl_;
    if (!s.session || !s.drv || !s.ended.empty()) return Admit::Stall;
    rnet_session_pump(s.session);
    const bool running = rnet_session_is_running(s.session) != 0;
    if (running) s.was_running = true;
    if (!running && !s.was_running && steady_ms() - s.started_ms > 30000) {
        s.ended = "no peer answered within 30 s";
        return Admit::Stall;
    }
    if (s.was_running && !running) {
        // A peer's BYE (or the transport closing) stops the session at once.
        s.ended = "a player left the match";
        return Admit::Stall;
    }
    if (s.was_running && rnet_session_peer_disconnected(s.session, 5000)) {
        s.ended = "a player left the match (no packets for 5 s)";
        return Admit::Stall;
    }
    switch (rnet_rb_driver_poll_admit(s.drv)) {
    case RNET_RB_ADMIT_LIVE: return Admit::Live;
    case RNET_RB_ADMIT_REPLAY: return Admit::Replay;
    default: ++s.stats.stalls; return Admit::Stall;
    }
}

void NetSession::finish(Admit a) {
    Impl& s = *impl_;
    if (!s.drv) return;
    if (a == Admit::Live) ++s.stats.live;
    if (a == Admit::Replay) ++s.stats.replayed;
    rnet_rb_driver_finish_frame(s.drv);
}

void NetSession::wait(int ms) {
    if (impl_->session) rnet_session_wait_recv(impl_->session, ms);
}

const rcore_pad& NetSession::seat_pad(std::uint32_t seat) const {
    static const rcore_pad none{};
    return seat < RCORE_MAX_SEATS ? impl_->pads[seat] : none;
}

std::uint32_t NetSession::tick() const { return impl_->last_tick; }
bool NetSession::in_resim() const { return impl_->resim; }
std::string NetSession::ended() const { return impl_->ended; }

NetStats NetSession::stats() const {
    NetStats st = impl_->stats;
    if (impl_->drv) {
        st.episodes = rnet_rb_driver_episode_count(impl_->drv);
        st.desyncs = rnet_rb_driver_desync_count(impl_->drv);
        st.rtt_ms = rnet_rb_driver_rtt_estimate_ms(impl_->drv);
        st.confirmed_through = rnet_rb_driver_confirmed_through(impl_->drv);
    }
    return st;
}

void NetSession::request_quiesce() {
    if (impl_->drv) rnet_rb_driver_request_quiesce(impl_->drv);
}

bool NetSession::drained() const {
    if (!impl_->drv) return false;
    const RNetRbQuiesce q = rnet_rb_driver_quiesce_state(impl_->drv);
    return q == RNET_RB_QUIESCE_DRAINED || q == RNET_RB_QUIESCE_TIMED_OUT;
}

void NetSession::shutdown() {
    if (!impl_) return;
    Impl& s = *impl_;
    if (s.session) rnet_session_send_bye(s.session);
    if (s.drv) {
        rnet_rb_driver_shutdown(s.drv);
        rnet_rb_driver_destroy(s.drv);
        s.drv = nullptr;
    }
    if (s.session) {
        rnet_session_destroy(s.session);
        s.session = nullptr;
    }
}

#else // !RETRO_RUNNER_NETPLAY

struct NetSession::Impl {
    std::array<rcore_pad, RCORE_MAX_SEATS> pads{};
};
NetSession::NetSession() : impl_(std::make_unique<Impl>()) {}
NetSession::~NetSession() = default;
bool NetSession::compiled_in() { return false; }
bool NetSession::start(const LoadedCore&, const NetParams&, std::string* error) {
    if (error) *error = "this runner was built without recomp-net (RETRO_RUNTIME_RECOMP_NET_DIR)";
    return false;
}
void NetSession::stage_local(const rcore_pad&) {}
NetSession::Admit NetSession::poll() { return Admit::Stall; }
void NetSession::finish(Admit) {}
void NetSession::wait(int) {}
const rcore_pad& NetSession::seat_pad(std::uint32_t seat) const { return impl_->pads[seat % RCORE_MAX_SEATS]; }
std::uint32_t NetSession::tick() const { return 0; }
bool NetSession::in_resim() const { return false; }
std::string NetSession::ended() const { return "not built with netplay"; }
NetStats NetSession::stats() const { return {}; }
void NetSession::request_quiesce() {}
bool NetSession::drained() const { return true; }
void NetSession::shutdown() {}

#endif

} // namespace retro::runner
