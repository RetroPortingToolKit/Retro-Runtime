#include "core_link.hpp"

#include "link_io.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <new>

namespace retro::corelink {

CoreLink::~CoreLink() {
    if (state_ != LinkState::Idle) stop();
    for (Region& r : regions_) release_shared(r.memory);
    release_shared(region_);
    channel_.close();
}

bool CoreLink::start(const LaunchSpec& spec, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    if (state_ != LinkState::Idle) return fail("this link was already started");
    spec_ = spec;
    std::error_code ec;
    fs::create_directories(spec_.session_dir, ec);

    // ---- the shared region: the hub's, so it outlives a crashed runner ------
    std::string err;
    if (!create_shared(shared_total_size(), "retro-core-link", region_, &err)) {
        return fail("shared region: " + err);
    }
    shm_ = new (region_.data) SharedHeader{};
    std::memcpy(shm_->magic, kMagic, sizeof kMagic);
    shm_->protocol_major = kProtocolMajor;
    shm_->protocol_minor = kProtocolMinor;
    shm_->header_size = sizeof(SharedHeader);
    shm_->total_size = shared_total_size();
    shm_->frames_offset = shared_frames_offset();
    shm_->audio_offset = shared_audio_offset();
    shm_->middle.store(1, std::memory_order_relaxed); // front 0, middle 1, back 2
    shm_->audio_capacity = kAudioCapacity;

    // ---- argv: the headless flags, plus --link ------------------------------
    SpawnSpec sp;
    std::vector<std::string>& args = sp.args;
    args = {path_utf8(spec_.runner), "--link",
            "--core", path_utf8(spec_.core),
            "--rom", spec_.rom,
            "--title-dir", path_utf8(spec_.title_dir),
            "--out", path_utf8(spec_.session_dir)};
    if (!spec_.package.empty()) {
        args.push_back("--package");
        args.push_back(spec_.package);
    }
    if (spec_.gl) args.push_back("--gl");
    if (spec_.strict) args.push_back("--strict");
    for (const auto& [k, v] : spec_.options) {
        args.push_back("--opt");
        args.push_back(k + "=" + v);
    }
    if (spec_.load_state) {
        args.push_back("--load-state");
        args.push_back(path_utf8(*spec_.load_state));
    }
    for (std::size_t seat = 0; seat < spec_.tpak_roms.size(); ++seat) {
        if (spec_.tpak_roms[seat].empty()) continue;
        args.push_back("--tpak" + std::to_string(seat + 1) + "-rom");
        args.push_back(spec_.tpak_roms[seat]);
    }
    args.insert(args.end(), spec_.extra_args.begin(), spec_.extra_args.end());
    sp.env = spec_.env;
    sp.log = runner_log();
    if (!spawn_runner(sp, region_, channel_, process_, &err)) return fail(err);
    state_ = LinkState::Starting;
    return true;
}

void CoreLink::pump(int timeout_ms) {
    if (state_ == LinkState::Idle || state_ == LinkState::Ended) return;
    std::vector<unsigned char> buf;
    std::vector<NativeHandle> handles;
    bool first = true;
    for (;;) {
        handles.clear();
        const RecvResult rr = recv_packet(channel_, buf, &handles, first ? timeout_ms : 0);
        first = false;
        if (rr == RecvResult::WouldBlock) break;
        if (rr != RecvResult::Packet) {
            // EOF or error: the runner is gone. Collect how it ended.
            reap(-1);
            return;
        }
        handle_packet(buf, handles);
        for (NativeHandle h : handles) close_handle(h); // those handle_packet did not take
        if (state_ == LinkState::Ended) return;
    }
    reap(0);
}

void CoreLink::handle_packet(const std::vector<unsigned char>& buf,
                             std::vector<NativeHandle>& handles) {
    Msg t{};
    if (!packet_type(buf, t)) return;
    switch (t) {
        case Msg::Hello: {
            HelloMsg m{};
            if (!as_msg(buf, m)) break;
            identity_.sha256 = m.sha256;
            identity_.core_id = m.core_id;
            identity_.core_version = m.core_version;
            identity_.platforms = m.platforms;
            identity_.capabilities = m.capabilities;
            identity_.abi_major = m.abi_major;
            identity_.draft_revision = m.draft_revision;
            identity_.engine_dirty = m.engine_dirty != 0;
            identity_.protocol_minor = std::min(m.protocol_minor, kProtocolMinor);
            break;
        }
        case Msg::SaveRegions: {
            SaveRegionsMsg m{};
            if (!as_msg(buf, m) || m.count > kMaxRegions || handles.size() != m.count) {
                exit_reason_ = "link: malformed save regions";
                stop(0);
                return;
            }
            // Map each region, fill it -- erase value, then the save file --
            // and keep the mapping: persistence is the hub's from here on.
            for (std::uint32_t i = 0; i < m.count; ++i) {
                const RegionDesc& d = m.region[i];
                Region r;
                r.id = d.id;
                r.size = static_cast<std::size_t>(d.size);
                std::string err;
                const NativeHandle h = handles[i];
                handles[i] = kNoHandle; // map_shared owns it now, mapped or not
                if (!map_shared(h, r.size, r.memory, &err)) {
                    exit_reason_ = "link: cannot map save region " + r.id + ": " + err;
                    stop(0);
                    return;
                }
                close_shared_handle(r.memory); // the mapping keeps the memory
                r.data = static_cast<std::uint8_t*>(r.memory.data);
                std::memset(r.data, static_cast<int>(d.erase_value & 0xff), r.size);
                if (auto f = spec_.save_files.find(r.id); f != spec_.save_files.end()) {
                    r.file = f->second;
                } else if (!spec_.save_dir.empty()) {
                    std::error_code dir_ec;
                    fs::create_directories(spec_.save_dir, dir_ec);
                    r.file = spec_.save_dir / (r.id + ".sav");
                }
                if (r.file) {
                    std::ifstream in(*r.file, std::ios::binary);
                    if (in) in.read(reinterpret_cast<char*>(r.data), std::streamsize(r.size));
                }
                regions_.push_back(std::move(r));
            }
            SavesFilledMsg ack{};
            ack.h.type = Msg::SavesFilled;
            for (std::uint32_t i = 0; i < RCORE_MAX_SEATS; ++i) {
                ack.pads[i] = spec_.initial_pads[i];
                ack.pads[i].struct_size = sizeof(rcore_pad);
            }
            send_msg(channel_, ack);
            break;
        }
        case Msg::Ready:
            state_ = LinkState::Ready;
            break;
        case Msg::FrameDone: {
            FrameDoneMsg m{};
            if (!as_msg(buf, m)) break;
            if (outstanding_ > 0) --outstanding_;
            done_ = m.frame_number;
            break;
        }
        case Msg::Log: {
            LogMsg m{};
            if (as_msg(buf, m)) logs.push_back({m.level, m.text});
            break;
        }
        case Msg::Event: {
            EventMsg m{};
            if (as_msg(buf, m)) events.push_back({m.kind, m.frame_number, m.guest_address, m.detail});
            break;
        }
        case Msg::Exiting: {
            ExitingMsg m{};
            if (as_msg(buf, m)) exit_reason_ = m.reason;
            break;
        }
        case Msg::StateDone: {
            StateDoneMsg m{};
            if (!as_msg(buf, m) || !state_pending_) break;
            m.detail[sizeof m.detail - 1] = '\0';
            StateResult r = state_request_;
            r.ok = m.ok != 0;
            r.bytes = m.bytes;
            r.detail = m.detail;
            state_result_ = r;
            state_pending_ = false;
            break;
        }
        default:
            break;
    }
}

bool CoreLink::grant(const rcore_pad pads[RCORE_MAX_SEATS]) {
    if (!can_grant()) return false;
    GrantMsg g{};
    g.h.type = Msg::Grant;
    g.frame_number = granted_ + 1;
    for (std::uint32_t i = 0; i < RCORE_MAX_SEATS; ++i) g.pads[i] = pads[i];
    if (!send_msg(channel_, g)) return false;
    ++granted_;
    ++outstanding_;
    return true;
}

bool CoreLink::states_supported() const {
    return state_ == LinkState::Ready && link_has_savestates(identity_.protocol_minor) &&
           (identity_.capabilities & RCORE_CAP_SAVESTATE);
}

bool CoreLink::request_state(Msg type, const fs::path& path) {
    if (!can_request_state()) return false;
    StateRequestMsg m{};
    m.h.type = type;
    m.frame_number = done_;
    const std::string p = path_utf8(path);
    if (p.size() >= sizeof m.path) return false;
    std::memcpy(m.path, p.c_str(), p.size() + 1);
    if (!send_msg(channel_, m)) return false;
    state_pending_ = true;
    state_request_ = StateResult{};
    state_request_.save = type == Msg::SaveState;
    state_request_.path = path;
    state_result_.reset();
    return true;
}

bool CoreLink::request_save_state(const fs::path& path) { return request_state(Msg::SaveState, path); }
bool CoreLink::request_load_state(const fs::path& path) { return request_state(Msg::LoadState, path); }

std::optional<StateResult> CoreLink::take_state_result() {
    std::optional<StateResult> r;
    r.swap(state_result_);
    return r;
}

bool CoreLink::take_frame() {
    if (!shm_) return false;
    if (!(shm_->middle.load(std::memory_order_acquire) & kFresh)) return false;
    front_ = shm_->middle.exchange(front_, std::memory_order_acq_rel) & ~kFresh;
    have_frame_ = true;
    return true;
}

const FrameInfo* CoreLink::frame_info() const {
    return have_frame_ ? &shm_->slot[front_] : nullptr;
}

const std::uint8_t* CoreLink::frame_pixels() const {
    if (!have_frame_) return nullptr;
    return reinterpret_cast<const std::uint8_t*>(shm_) + shm_->frames_offset +
           std::size_t(front_) * kFrameSlotBytes;
}

std::uint32_t CoreLink::audio_rate() const {
    return shm_ ? shm_->audio_rate.load(std::memory_order_acquire) : 0;
}

bool CoreLink::frame_rate(std::uint32_t& num, std::uint32_t& den) const {
    const std::uint64_t packed = shm_ ? shm_->frame_rate.load(std::memory_order_acquire) : 0;
    num = static_cast<std::uint32_t>(packed >> 32);
    den = static_cast<std::uint32_t>(packed);
    return num && den;
}

std::size_t CoreLink::drain_audio(std::int16_t* out, std::size_t max_frames) {
    if (!shm_) return 0;
    const std::uint64_t cap = shm_->audio_capacity;
    const std::uint64_t r = shm_->audio_read.load(std::memory_order_relaxed);
    const std::uint64_t w = shm_->audio_write.load(std::memory_order_acquire);
    const std::uint64_t n = std::min<std::uint64_t>(w - r, max_frames);
    const auto* ring = reinterpret_cast<const std::int16_t*>(
        reinterpret_cast<const std::uint8_t*>(shm_) + shm_->audio_offset);
    for (std::uint64_t i = 0; i < n; ++i) {
        const std::uint64_t pos = (r + i) % cap;
        out[i * 2] = ring[pos * 2];
        out[i * 2 + 1] = ring[pos * 2 + 1];
    }
    shm_->audio_read.store(r + n, std::memory_order_release);
    return static_cast<std::size_t>(n);
}

void CoreLink::persist_saves() {
    for (const Region& r : regions_) {
        if (!r.file || !r.data) continue;
        // Write beside, then rename: a crash mid-write never leaves half a save.
        const fs::path tmp = r.file->string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(r.data), std::streamsize(r.size));
            if (!out) continue;
        }
        std::error_code ec;
        fs::rename(tmp, *r.file, ec);
    }
}

void CoreLink::reap(int timeout_ms) {
    if (!process_.running()) return;
    if (const auto code = wait_exit(process_, timeout_ms)) on_ended(*code);
}

void CoreLink::on_ended(int code) {
    exit_code_ = code;
    state_ = LinkState::Ended;
    outstanding_ = 0;
    if (state_pending_) {
        StateResult r = state_request_;
        r.ok = false;
        r.detail = "the runner ended before it answered";
        state_result_ = r;
        state_pending_ = false;
    }
    persist_saves(); // the hub's mappings survive the runner
}

void CoreLink::stop(int grace_ms) {
    if (state_ == LinkState::Idle) return;
    if (process_.running()) {
        EmptyMsg q{};
        q.h.type = Msg::Quit;
        send_msg(channel_, q);
        reap(grace_ms);
        if (process_.running()) {
            kill_runner(process_);
            if (exit_reason_.empty()) exit_reason_ = "did not quit in time; killed";
            reap(-1);
        }
    }
    if (state_ != LinkState::Ended) on_ended(exit_code_);
}

} // namespace retro::corelink
