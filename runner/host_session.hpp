#pragma once

// The host side of the rcore contract, as one session: everything the runner
// owns regardless of where frames go. Declared options (an undeclared key is a
// fault), host-owned save memory, the wall clock, and the lent GL context.
// Where frames, audio, input and events go is the Sink's business -- files in
// headless mode, the hub's shared memory once the link exists -- so the
// contract rules live here once.

#include "core_library.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace retro::runner {

class Sink {
public:
    virtual ~Sink() = default;
    virtual void log(std::uint32_t level, const char* msg) = 0;
    virtual void event(const rcore_event& e) = 0;
    virtual void frame(const rcore_frame& f) = 0;
    virtual void audio(const std::int16_t* samples, std::uint32_t frames) = 0;
    virtual void audio_rate(std::uint32_t hz) = 0;
    // The state of `seat` for the frame being run. Must be identical for every
    // call within one frame (the contract's rollback-replay rule).
    virtual void input(std::uint32_t seat, rcore_pad& out) = 0;
    virtual void rumble(std::uint32_t, std::uint16_t, std::uint16_t) {}
    // The core's nominal frame rate, num/den per second (rev 5); 0/0 = unstated.
    virtual void frame_rate(std::uint32_t, std::uint32_t) {}
    // One message from a data accessory for the host (rev 7 accessory_notify):
    // its bytes, copied before this returns. Stderr headless, the hub over the
    // link. May arrive at any point of a frame.
    virtual void accessory_notify(std::uint32_t, std::uint32_t, const void*, std::size_t) {}
};

struct SaveRegion {
    std::string id;
    std::uint32_t kind = 0;
    std::uint32_t seat = 0;
    std::uint32_t slot = 0;
    std::uint32_t erase_value = 0;
    std::uint8_t* data = nullptr; // `owned`, or memory someone else maps
    std::size_t size = 0;
    std::vector<std::uint8_t> owned;
    std::optional<fs::path> file; // filled from and persisted to, if any
};

class HostSession {
public:
    HostSession(const LoadedCore& core, Sink& sink);

    // Declared options with their defaults, then `overrides` on top. An
    // override naming an undeclared key is refused (returns false, *error).
    bool set_options(const std::map<std::string, std::string>& overrides, std::string* error);
    // Declared keys and their resolved values; nullopt = unset.
    const std::map<std::string, std::optional<std::string>>& options() const { return options_; }

    // Lend a GL context: `proc` resolves names against a context the caller
    // keeps current on the core thread for every core call. Before init():
    // the core keeps the table it is handed.
    void lend_gl(void* (*proc)(const char* name)) {
        gl_proc_ = proc;
        host_.gl_get_proc_address = proc ? h_gl_proc : nullptr;
    }

    // The host table the core is given. Stable for the session's lifetime.
    const rcore_host_api* host_api() const { return &host_; }

    // After load(): allocate every declared region, filled with its erase
    // value, then from `files[id]` where one is given.
    void adopt_save_regions(const rcore_save_region* regions, std::uint32_t count,
                            const std::map<std::string, fs::path>& files);
    // The same, over memory the caller provides (the hub link: one shared
    // mapping per region, filled and persisted by the hub). `memory[i]` holds
    // regions[i].size bytes and is already filled.
    void adopt_external_save_regions(const rcore_save_region* regions, std::uint32_t count,
                                     const std::vector<std::uint8_t*>& memory);
    const std::vector<SaveRegion>& save_regions() const { return regions_; }
    // Write every file-backed region back, as a frontend persists saves.
    void persist_save_regions() const;

    // Data accessories (rev 7, docs/CORE_ABI.md "Accessory data"): the host
    // side of accessory_poll / accessory_notify. A binding is registered before
    // init(); from then on that seat reads NO pad -- input_get answers
    // connected = 0 whatever the Sink or the netplay source says, because the
    // console reads the port as the accessory alone, and a host that grants a
    // pad there must not leak it. Messages queued with queue_accessory_data()
    // wait until begin_frame_accessories(), which makes everything queued so
    // far THE frame's sequence: accessory_poll hands it out one message at a
    // time, and a drain (calls until 0) can be repeated within the frame and
    // see the same sequence again. The next begin_frame_accessories() drops
    // what the core did not take. Bytes for a (seat, slot) with no binding
    // are refused (false), never queued.
    void bind_data_accessory(std::uint32_t seat, std::uint32_t slot);
    bool is_data_accessory(std::uint32_t seat, std::uint32_t slot) const;
    bool queue_accessory_data(std::uint32_t seat, std::uint32_t slot, const void* data,
                              std::size_t len);
    void begin_frame_accessories();

    // Netplay (net_session.hpp): input_get answers from `source` -- the
    // published rows -- instead of the Sink, which then only feeds the local
    // pad into the session.
    void set_input_source(std::function<void(std::uint32_t, rcore_pad&)> source) {
        input_source_ = std::move(source);
    }
    // Netplay: wall_clock_us becomes a pure function of the session's agreed
    // epoch and the frame number (rev 4): epoch + tick() frames at 60 Hz.
    void set_session_clock(std::uint64_t epoch_us, std::function<std::uint64_t()> tick) {
        clock_epoch_us_ = epoch_us;
        clock_tick_ = std::move(tick);
    }

private:
    static HostSession* self(void* ctx) { return static_cast<HostSession*>(ctx); }

    static void h_log(void*, std::uint32_t, const char*);
    static void h_report(void*, const rcore_event*);
    static void h_video(void*, const rcore_frame*);
    static void h_audio(void*, const std::int16_t*, std::uint32_t);
    static void h_rate(void*, std::uint32_t);
    static void h_input(void*, std::uint32_t, rcore_pad*);
    static void h_rumble(void*, std::uint32_t, std::uint16_t, std::uint16_t);
    static const char* h_option(void*, const char*);
    static std::uint32_t h_options_changed(void*);
    static void* h_save(void*, const char*);
    static void* h_gl_proc(void*, const char*);
    static std::uint64_t h_wall_clock(void*);
    static void h_frame_rate(void*, std::uint32_t, std::uint32_t);
    static std::size_t h_accessory_poll(void*, std::uint32_t, std::uint32_t, void*, std::size_t);
    static void h_accessory_notify(void*, std::uint32_t, std::uint32_t, const void*, std::size_t);

    struct AccessoryQueue {
        std::uint32_t seat = 0, slot = 0;
        std::vector<std::vector<std::uint8_t>> pending; // arrived, for the next frame
        std::vector<std::vector<std::uint8_t>> frame;   // this frame's sequence
        std::size_t cursor = 0;                         // the next frame[] to hand out
    };
    AccessoryQueue* accessory_queue(std::uint32_t seat, std::uint32_t slot);

    const LoadedCore& core_;
    Sink& sink_;
    rcore_host_api host_{};
    std::map<std::string, std::optional<std::string>> options_;
    std::vector<SaveRegion> regions_;
    std::vector<AccessoryQueue> accessories_;
    void* (*gl_proc_)(const char*) = nullptr;
    std::function<void(std::uint32_t, rcore_pad&)> input_source_;
    std::uint64_t clock_epoch_us_ = 0;
    std::function<std::uint64_t()> clock_tick_;
};

} // namespace retro::runner
