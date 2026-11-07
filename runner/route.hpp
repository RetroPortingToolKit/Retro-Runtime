#pragma once

#include "host_session.hpp"
#include <array>
#include <fstream>

namespace retro::runner {

// An input movie from boot or a restored anchor. Input is captured at the core callback, not at a
// desktop timer. The route is private: its initial save regions can contain
// player data. The .partial file is flushed after each complete field.
struct RouteArgs {
    fs::path record, play, verdict, runner, runner_reference;
    bool allow_core_change = false;
    bool takeover = false;
    std::map<std::uint64_t, std::string> marks;
    bool active() const { return !record.empty() || !play.empty(); }
};

class Route {
public:
    Route(const LoadedCore&, HostSession&, const RouteArgs&, const fs::path& out);
    bool open(const std::string& rom, const std::string& package_sha,
              const std::string& title_dir, bool strict);
    bool initial_saves();
    bool capture_anchor();
    bool anchored() const { return !anchor_.empty(); }
    bool begin(std::uint32_t width = 0, std::uint32_t height = 0);
    bool end();
    bool finish(bool success, bool stopped = false, bool taken_over = false);
    static bool live_input(const rcore_pad pads[RCORE_MAX_SEATS]);
    bool playback() const { return !args_.play.empty(); }
    bool done() const { return playback() && next_ >= frames_.size(); }
    std::uint64_t length() const { return frames_.size(); }
    std::uint64_t field() const { return next_; }
    std::uint32_t width() const;
    std::uint32_t height() const;
    const std::string& error() const { return error_; }
    int exit_code() const { return diverged_ ? 5 : 2; }

    // Called only by HostSession's shared core callbacks.
    bool input(std::uint32_t, rcore_pad&); // true when playback supplies it
    void delivered(std::uint32_t, const rcore_pad&);
    std::uint64_t clock(std::uint64_t live);
    void video(const rcore_frame&);
    void audio(const std::int16_t*, std::uint32_t);
    void gpu_video();

private:
    struct Input { std::uint32_t seat; rcore_pad pad; };
    struct Field {
        std::vector<Input> inputs;
        std::vector<std::uint64_t> clocks;
        std::uint32_t width = 0, height = 0;
        std::string state, image, audio, mark;
    };
    struct InitialSave {
        std::string id;
        std::uint32_t kind, seat, slot, erase;
        std::vector<std::uint8_t> bytes;
    };
    bool fail(const std::string&, bool divergence = false);
    bool read();
    bool write_field(const Field&, bool boot);
    bool calls_complete();
    std::string state_digest() const;
    void write_verdict(const char* result);
    const LoadedCore& core_;
    HostSession& session_;
    RouteArgs args_;
    fs::path out_, partial_;
    std::map<std::string, std::string> identity_, recorded_identity_;
    std::vector<InitialSave> saves_;
    Field boot_, current_;
    std::vector<Field> frames_;
    std::vector<std::uint8_t> picture_, pcm_;
    std::vector<std::uint8_t> anchor_;
    std::string anchor_state_;
    std::string expected_state_, actual_state_;
    std::uint32_t picture_width_ = 0, picture_height_ = 0;
    std::ofstream file_;
    std::uint64_t next_ = 0, input_cursor_ = 0, clock_cursor_ = 0;
    bool started_ = false, finished_ = false, diverged_ = false, gpu_ = false;
    std::string error_;
    std::vector<std::string> added_defaults_;
    bool taken_over_ = false;
};

} // namespace retro::runner
