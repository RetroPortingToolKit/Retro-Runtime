#include "runner_link.hpp"

#include "../corelink/link_io.hpp"
#include "state_keeper.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace retro::runner {

using namespace retro::corelink;

namespace {

void copy_str(char* dst, std::size_t cap, const char* src) {
    std::snprintf(dst, cap, "%s", src ? src : "");
}

// Frames and audio into the shared region; logs and events to the session
// dir (everything) and to the hub (what it shows); input from the grant.
class LinkSink final : public Sink {
public:
    LinkSink(Channel& sock, SharedHeader* shm, const fs::path& out)
        : sock_(sock), shm_(shm), log_(out / "core.log"), events_(out / "events.tsv") {
        base_ = reinterpret_cast<std::uint8_t*>(shm);
    }

    void log(std::uint32_t level, const char* msg) override {
        log_ << level << '\t' << msg << '\n';
        if (level <= RCORE_LOG_WARN) {
            LogMsg m{};
            m.h.type = Msg::Log;
            m.level = level;
            copy_str(m.text, sizeof m.text, msg);
            send_msg(sock_, m);
        }
    }

    void event(const rcore_event& e) override {
        const char* kind = e.kind == RCORE_EVENT_DISPATCH_MISS ? "DISPATCH_MISS"
                           : e.kind == RCORE_EVENT_BRIDGE      ? "BRIDGE"
                           : e.kind == RCORE_EVENT_FAULT       ? "FAULT"
                                                               : "?";
        char addr[24];
        std::snprintf(addr, sizeof addr, "0x%08llx",
                      static_cast<unsigned long long>(e.guest_address));
        events_ << kind << '\t' << e.frame_number << '\t' << addr << '\t'
                << (e.detail ? e.detail : "") << '\n';
        EventMsg m{};
        m.h.type = Msg::Event;
        m.kind = e.kind;
        m.frame_number = e.frame_number;
        m.guest_address = e.guest_address;
        copy_str(m.detail, sizeof m.detail, e.detail);
        send_msg(sock_, m);
        if (e.kind == RCORE_EVENT_FAULT) ++faults;
    }

    void frame(const rcore_frame& f) override {
        if (f.pixel_format != RCORE_PIXEL_RGBA8 || f.width > kMaxFrameWidth ||
            f.height > kMaxFrameHeight) {
            ++bad_frames; // reported at the end of the frame, not mid-core
            return;
        }
        std::uint8_t* dst = base_ + shm_->frames_offset + std::size_t(back_) * kFrameSlotBytes;
        const auto* src = static_cast<const std::uint8_t*>(f.pixels);
        const std::size_t row = std::size_t(f.width) * 4;
        for (std::uint32_t y = 0; y < f.height; ++y) {
            std::memcpy(dst + y * row, src + std::size_t(y) * f.stride, row);
        }
        FrameInfo& info = shm_->slot[back_];
        info.width = f.width;
        info.height = f.height;
        info.stride = static_cast<std::uint32_t>(row);
        info.pixel_format = f.pixel_format;
        info.aspect_num = f.aspect_num;
        info.aspect_den = f.aspect_den;
        info.frame_number = frame_number;
        // Publish: our back becomes middle, and middle's old slot becomes ours.
        published_ = static_cast<int>(back_);
        back_ = shm_->middle.exchange(back_ | kFresh, std::memory_order_acq_rel) & ~kFresh;
    }

    // The last published picture, shrunk for a savestate. Between frames that
    // slot is the middle or the hub's front: nobody writes it until the runner
    // publishes again, which it is not doing while it answers a request.
    std::vector<std::uint8_t> thumbnail() const {
        if (published_ < 0) return {};
        const FrameInfo& info = shm_->slot[published_];
        return state::make_thumbnail(base_ + shm_->frames_offset +
                                         std::size_t(published_) * kFrameSlotBytes,
                                     info.width, info.height, info.stride);
    }

    void audio(const std::int16_t* samples, std::uint32_t n) override {
        const std::uint64_t cap = shm_->audio_capacity;
        const std::uint64_t w = shm_->audio_write.load(std::memory_order_relaxed);
        const std::uint64_t r = shm_->audio_read.load(std::memory_order_acquire);
        const std::uint64_t room = cap - (w - r);
        const std::uint64_t take = std::min<std::uint64_t>(n, room);
        auto* ring = reinterpret_cast<std::int16_t*>(base_ + shm_->audio_offset);
        for (std::uint64_t i = 0; i < take; ++i) {
            const std::uint64_t pos = (w + i) % cap;
            ring[pos * 2] = samples[i * 2];
            ring[pos * 2 + 1] = samples[i * 2 + 1];
        }
        shm_->audio_write.store(w + take, std::memory_order_release);
        if (take < n) shm_->audio_dropped.fetch_add(n - take, std::memory_order_relaxed);
    }

    void audio_rate(std::uint32_t hz) override {
        shm_->audio_rate.store(hz, std::memory_order_release);
    }

    void frame_rate(std::uint32_t num, std::uint32_t den) override {
        shm_->frame_rate.store((std::uint64_t(num) << 32) | den, std::memory_order_release);
    }

    void input(std::uint32_t seat, rcore_pad& pad) override {
        if (seat < RCORE_MAX_SEATS) pad = pads[seat];
    }

    rcore_pad pads[RCORE_MAX_SEATS]{};
    std::uint64_t frame_number = 0; // the granted frame being run
    std::uint32_t faults = 0;
    std::uint32_t bad_frames = 0;

private:
    Channel& sock_;
    SharedHeader* shm_;
    std::uint8_t* base_;
    std::uint32_t back_ = 2; // the triple buffer's initial back slot
    int published_ = -1;     // the slot last swapped into middle
    std::ofstream log_, events_;
};

int exiting(Channel& sock, int code, const std::string& reason) {
    ExitingMsg m{};
    m.h.type = Msg::Exiting;
    m.code = code;
    copy_str(m.reason, sizeof m.reason, reason.c_str());
    send_msg(sock, m);
    std::fprintf(stderr, "retro-core-runner: %s\n", reason.c_str());
    return code;
}

} // namespace

int run_link_mode(const LoadedCore& core, const CoreManifest& manifest, const LinkArgs& a,
                  void (*lend_gl)(HostSession&)) {
    Channel sock;
    NativeHandle shared_handle = kNoHandle;
    std::string link_err;
    if (!runner_endpoints(a.link_handles, sock, shared_handle, &link_err)) {
        std::fprintf(stderr, "retro-core-runner: link: %s\n", link_err.c_str());
        return 2;
    }

    // ---- the shared region the hub created ----------------------------------
    SharedMemory region;
    if (!map_shared(shared_handle, shared_total_size(), region, &link_err)) {
        return exiting(sock, 2, "link: the shared region is missing or too small (" + link_err + ")");
    }
    auto* shm = static_cast<SharedHeader*>(region.data);
    if (std::memcmp(shm->magic, kMagic, sizeof kMagic) != 0) {
        return exiting(sock, 2, "link: the shared region is not a link region");
    }
    if (shm->protocol_major != kProtocolMajor) {
        return exiting(sock, 2,
                       "link: protocol major " + std::to_string(shm->protocol_major) +
                           " from the host, " + std::to_string(kProtocolMajor) +
                           " in this runner -- update whichever is older");
    }

    // ---- identity first, so the hub can show what it is about to run --------
    HelloMsg hello{};
    hello.h.type = Msg::Hello;
    hello.protocol_major = kProtocolMajor;
    hello.protocol_minor = kProtocolMinor;
    hello.abi_major = core.info->abi_major;
    hello.draft_revision = static_cast<std::uint32_t>(manifest.draft_revision);
    hello.engine_dirty = manifest.engine_dirty ? 1u : 0u;
    hello.capabilities = core.info->capabilities;
    copy_str(hello.sha256, sizeof hello.sha256, core.sha256.c_str());
    copy_str(hello.core_id, sizeof hello.core_id, core.info->core_id);
    copy_str(hello.core_version, sizeof hello.core_version, core.info->core_version);
    copy_str(hello.platforms, sizeof hello.platforms, core.info->platforms);
    if (!send_msg(sock, hello)) return 2;

    LinkSink sink(sock, shm, a.out);
    HostSession session(core, sink);
    std::string err;
    if (!session.set_options(a.overrides, &err)) return exiting(sock, 2, err);
    if (a.gl) lend_gl(session);

    const std::string cache = a.out.string();
    rcore_init_params ip{};
    ip.struct_size = sizeof ip;
    ip.flags = a.strict ? RCORE_INIT_STRICT : 0u;
    ip.cache_dir = cache.c_str();
    if (const rcore_result rc = core.api->init(session.host_api(), &ip); rc != RCORE_OK) {
        return exiting(sock, 2, "init -> " + std::to_string(rc));
    }

    std::vector<rcore_accessory_binding> bindings;
    if (!a.tpak_rom.empty()) {
        rcore_accessory_binding b{};
        b.struct_size = sizeof b;
        b.type_id = "n64.transfer_pak";
        b.content_path = a.tpak_rom.c_str();
        bindings.push_back(b);
    }
    rcore_load_params lp{};
    lp.struct_size = sizeof lp;
    lp.content_path = a.rom.c_str();
    lp.package_path = a.package.empty() ? nullptr : a.package.c_str();
    lp.title_dir = a.title_dir.c_str();
    lp.accessories = bindings.empty() ? nullptr : bindings.data();
    lp.accessory_count = static_cast<std::uint32_t>(bindings.size());
    const rcore_save_region* regs = nullptr;
    std::uint32_t nregs = 0;
    if (const rcore_result rc = core.api->load(&lp, &regs, &nregs); rc != RCORE_OK) {
        return exiting(sock, 2, "load -> " + std::to_string(rc));
    }
    if (nregs > kMaxRegions) return exiting(sock, 2, "the core declared too many save regions");

    // ---- save memory: one object per region, handed to the hub --------------
    // The runner keeps its handles for the whole session: on Windows the hub
    // duplicates them out of this process after it reads SaveRegions.
    SaveRegionsMsg sr{};
    sr.h.type = Msg::SaveRegions;
    sr.count = nregs;
    std::vector<SharedMemory> objects(nregs);
    std::vector<NativeHandle> handles;
    std::vector<std::uint8_t*> memory;
    for (std::uint32_t i = 0; i < nregs; ++i) {
        const rcore_save_region& r = regs[i];
        RegionDesc& d = sr.region[i];
        copy_str(d.id, sizeof d.id, r.id);
        d.kind = r.kind;
        d.seat = r.seat;
        d.slot = r.slot;
        d.erase_value = RCORE_HAS(&r, rcore_save_region, erase_value) ? r.erase_value : 0;
        d.size = r.size;
        std::string err;
        if (!create_shared(static_cast<std::size_t>(r.size), d.id, objects[i], &err)) {
            return exiting(sock, 2, std::string("cannot create save memory for ") + d.id +
                                        ": " + err);
        }
        handles.push_back(objects[i].handle);
        memory.push_back(static_cast<std::uint8_t*>(objects[i].data));
    }
    if (!send_msg(sock, sr, handles.data(), handles.size())) return 2;

    // The hub fills each region (erase value, then its save file) and says so.
    std::vector<unsigned char> buf;
    for (;;) {
        const RecvResult rr = recv_packet(sock, buf, nullptr, -1);
        if (rr != RecvResult::Packet) return 0; // the hub went away before playing
        Msg t{};
        packet_type(buf, t);
        SavesFilledMsg filled{};
        if (t == Msg::SavesFilled && as_msg(buf, filled)) {
            std::copy(std::begin(filled.pads), std::end(filled.pads), std::begin(sink.pads));
            break;
        }
        if (t == Msg::Quit) {
            core.api->unload();
            core.api->deinit();
            return 0;
        }
    }
    session.adopt_external_save_regions(regs, nregs, memory);

    StateKeeper keeper(core, session, a.rom, a.package_sha256, bindings);
    // A core that can save gets its content hashed now, off the core thread,
    // so the player's first save does not wait on it.
    if (core.info->capabilities & RCORE_CAP_SAVESTATE) keeper.hash_in_background();
    if (a.load_state) {
        // The launch's own state: an envelope is checked; a bare state, as the
        // gates write them, goes to the core as it always did.
        std::uint64_t bytes = 0;
        std::string why;
        if (!keeper.load(*a.load_state, true, &bytes, &why)) {
            return exiting(sock, 2, "--load-state " + a.load_state->string() + ": " + why);
        }
    }

    EmptyMsg ready{};
    ready.h.type = Msg::Ready;
    if (!send_msg(sock, ready)) return 2;

    // ---- one frame per grant, until Quit or the hub goes away ---------------
    int code = 0;
    for (;;) {
        const RecvResult rr = recv_packet(sock, buf, nullptr, -1);
        if (rr != RecvResult::Packet) break; // hub gone: stop quietly
        Msg t{};
        packet_type(buf, t);
        if (t == Msg::Quit) break;
        if (t == Msg::SaveState || t == Msg::LoadState) {
            // 1.1. Between frames by construction: one grant at a time, and
            // the hub sends a request only when none is outstanding.
            StateDoneMsg d{};
            d.h.type = Msg::StateDone;
            d.request = t;
            StateRequestMsg r{};
            std::string why;
            std::uint64_t bytes = 0;
            bool ok = false;
            if (!as_msg(buf, r)) {
                why = "malformed request";
            } else {
                r.path[sizeof r.path - 1] = '\0';
                const fs::path path = utf8_path(r.path);
                ok = t == Msg::SaveState
                         ? keeper.save(path, r.frame_number, sink.thumbnail(), &bytes, &why)
                         : keeper.load(path, false, &bytes, &why);
                std::fprintf(stderr, "state: %s %s: %s\n",
                             t == Msg::SaveState ? "save" : "load", r.path,
                             ok ? ("ok, " + std::to_string(bytes) + " bytes").c_str()
                                : why.c_str());
            }
            d.ok = ok ? 1 : 0;
            d.bytes = bytes;
            copy_str(d.detail, sizeof d.detail, why.c_str());
            send_msg(sock, d);
            continue;
        }
        GrantMsg g{};
        if (t != Msg::Grant || !as_msg(buf, g)) {
            code = exiting(sock, 2, "link: unexpected message from the hub");
            break;
        }
        std::copy(std::begin(g.pads), std::end(g.pads), std::begin(sink.pads));
        sink.frame_number = g.frame_number;
        FrameDoneMsg done{};
        done.h.type = Msg::FrameDone;
        done.frame_number = g.frame_number;
        done.result = core.api->run_frame();
        send_msg(sock, done);
        if (sink.bad_frames) {
            code = exiting(sock, 1, "the core submitted a frame this link cannot carry "
                                    "(format, or larger than 1024x1024)");
            break;
        }
        if (done.result != RCORE_OK) {
            code = exiting(sock, 1, "run_frame " + std::to_string(g.frame_number) + " -> " +
                                        std::to_string(done.result));
            break;
        }
    }
    core.api->unload();
    core.api->deinit();
    return code ? code : (sink.faults ? 1 : 0);
}

} // namespace retro::runner
