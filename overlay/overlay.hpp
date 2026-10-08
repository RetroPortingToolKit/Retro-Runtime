#pragma once

// retro_overlay -- what a host draws over a running core: the frame-rate
// readout (top left), the TURBO marker (top right), the volume meter (right edge),
// toasts, and the save-state browser (centre). docs/OVERLAY.md is the design.
//
// It lives here, in the runtime, and not in a core or a host, so that it looks
// and behaves the same whichever core is running and whichever host is
// drawing. Cores never draw it (a core owns no window: rcore.h); hosts never
// re-implement it. A host feeds it events and draws the images it hands back.
//
// It depends on nothing but the C++ library and rcore.h: no SDL, no GL, no
// clock. The host passes the time in and composites the images however it
// draws anything else -- one texture per Layer::id, re-uploaded when
// Layer::revision changes, drawn at place(), nearest-filtered.
//
// Nothing here touches the machine. The overlay is composited after the core
// has produced its picture; it is not in the frame the core submitted, not in
// a savestate and not in anything a netplay peer sees.

#include "rcore/rcore.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace retro::overlay {

namespace fs = std::filesystem;

// 0xAARRGGBB.
using Argb = std::uint32_t;

// RGBA8, bytes R,G,B,A in memory order (the layout of an RCORE_PIXEL_RGBA8
// frame), rows packed, straight alpha.
struct Image {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
    void resize(std::uint32_t w, std::uint32_t h, Argb fill);
};

// Where a layer sits in the WINDOW. Overlay is host chrome: it does not scale
// or move with the game's letterboxed picture.
enum class Anchor : std::uint32_t {
    TopLeft,     // FPS readout and toasts
    TopRight,    // TURBO
    RightMiddle, // volume meter
    Center,      // the save-state browser
};

enum LayerId : std::uint32_t {
    kLayerStatus = 1, // FPS + toasts
    kLayerTurbo = 2,
    kLayerVolume = 3,
    kLayerSavestates = 4,
};

struct Layer {
    std::uint32_t id = 0;        // LayerId: one texture per id is enough
    const Image* image = nullptr;
    std::uint64_t revision = 0;  // changes whenever the pixels do
    Anchor anchor = Anchor::TopLeft;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};

// Where to draw `layer` in a window `win_w` x `win_h` (in whatever units the
// host draws in). Every host places layers with this, so the overlay sits in
// the same place everywhere:
//   - corner and edge layers draw at an integer scale that grows with the
//     window (1 below 1800 units tall, 2 below 3600, ...), inset 8 units per
//     step of scale from the edges they are anchored to;
//   - the save-state browser draws at the largest integer scale that fits 90%
//     of the window (or shrinks to fit a window smaller than it).
Rect place(const Layer& layer, float win_w, float win_h);

// ---- OSD: frame rates, TURBO, volume, toasts -----------------------------------

// The readout's inputs, as running totals since the core loaded
// (corelink::FrameStats carries the same three over the link).
struct FrameCounters {
    std::uint64_t frames = 0;  // frames the machine has run: VI fields on an N64
    std::uint64_t game = 0;    // pictures the GAME has finished (rcore_frame::game_frame);
                               // 0 = the core does not tell them apart
    std::uint64_t work_ns = 0; // time the core took to run those frames; 0 = unmeasured
};

// Three rates, each over the last second of samples.
struct FrameRates {
    double fps = 0;       // the game's pictures per second (valid when has_fps)
    double vi = 0;        // machine frames per second: what is presented
    double ms_per_vi = 0; // emulation time per machine frame (valid when has_ms)
    bool has_fps = false, has_ms = false;
};

class Osd {
public:
    Osd();

    // The frame-rate readout, top left. The host's "show FPS" setting and its
    // hotkey both set this; the OSD only draws what it is told. Three rows:
    //   FPS    the rate the game draws at (an N64 game at 30 reads 30 here)
    //   VI     the rate the machine runs and presents frames at (60 there)
    //   ms/VI  how long the core took to emulate one of them: the headroom,
    //          against the 16.7 ms a 60 Hz frame allows
    // A reading the core or the link cannot supply shows "--".
    void set_fps_visible(bool on);
    bool fps_visible() const { return fps_visible_; }

    // The totals as they stand, whenever the host has looked (once a host
    // frame is plenty; calls where nothing moved are ignored). Rates are the
    // change over the last second of these, counted on EMULATED frames, not
    // presents: under turbo the core runs several frames per present, and the
    // reading should say how fast the machine is running. A total that goes
    // backwards (a reset) or half a second without a frame starts it afresh
    // rather than averaging the break in.
    void note_counters(std::uint64_t now_ns, const FrameCounters& c);
    // One more machine frame and nothing else known: for a host with no
    // counters. FPS and ms/VI then read "--".
    void note_frame(std::uint64_t now_ns);
    FrameRates rates() const;
    // The rows as last drawn (or about to be): "FPS    30.0\nVI     60.0\n...".
    const std::string& readout() const { return readout_; }
    // Start the reading afresh: a host calls it when play resumes after a
    // pause or a menu. Turbo starting or stopping does it too.
    void restart_fps();

    // TURBO marker, top right, while the host runs the core faster than its
    // own rate.
    void set_turbo(bool on);
    bool turbo() const { return turbo_; }

    // Volume meter, right edge, for 1.5 s after the player changes the volume.
    void show_volume(int percent, std::uint64_t now_ns);

    // A timed line under the FPS readout: something that happened.
    void toast(const std::string& text, std::uint64_t now_ns, std::uint32_t duration_ms = 2000);

    // What is showing at `now_ns`, rasterized where it changed. Valid until
    // the next call on this object.
    const std::vector<Layer>& layers(std::uint64_t now_ns);

private:
    void rasterize_status();
    static std::string format_readout(const FrameRates& r, bool have_window);
    void rasterize_volume();

    bool fps_visible_ = false;
    bool turbo_ = false;

    struct Sample {
        std::uint64_t ns;
        FrameCounters c;
    };
    static constexpr int kSamples = 256; // a second of samples, even under turbo
    Sample samples_[kSamples]{};
    int pos_ = 0, count_ = 0;
    bool game_counted_ = false;    // the core has said a nonzero game count
    std::uint64_t own_frames_ = 0; // note_frame's running total
    std::string readout_;          // the rows last formatted
    std::uint64_t readout_ns_ = 0; // when; refreshed 4 times a second
    bool readout_stale_ = true;    // refresh as soon as the window allows

    std::string toast_;
    std::uint64_t toast_until_ns_ = 0;

    int volume_ = -1, volume_drawn_ = -1;
    std::uint64_t volume_until_ns_ = 0;

    std::string status_drawn_;
    int status_readout_rows_ = 0; // how many of its rows are the readout's
    Image status_, turbo_img_, volume_img_;
    std::uint64_t status_rev_ = 0, volume_rev_ = 0;
    std::vector<Layer> layers_;
};

// ---- Save-state browser --------------------------------------------------------

// Twelve slots of a title, each an envelope file (state/state_envelope.hpp)
// the RUNNER writes and checks: this class lists them, draws the browser and
// turns the player's input into requests. The host carries a request out --
// over its link to the runner -- and reports back with finish().
//
// Input, the same for every core:
//   open / close    SELECT + R1 on any pad (the gesture psxrecomp, snesrecomp
//                   and n64lle share), or the host's hotkey (open())
//   choose a slot   D-pad or left stick (held: repeats), keys Up / Down,
//                   1-9, 0, -, = jump to slots 1-12
//   load            SOUTH face button, Enter
//   save            NORTH face button, S
//   back            EAST face button, Start, Escape, Backspace
// Face buttons are read by POSITION, from the physical pad, and the legend
// draws them as positions -- never as one console's letters.
class SavestateMenu {
public:
    static constexpr int kSlots = 12;

    SavestateMenu();

    // Slots are `dir`/slot01.rstate .. slot12.rstate. `core_id` and
    // `core_sha256` are the running core's (the link's Hello): a slot saved by
    // another core or another build is marked in the list. That mark is a
    // hint; the runner's load rule is the judge.
    void configure(const fs::path& dir, const std::string& core_id, const std::string& core_sha256);
    // The header's right-hand text: how to open and close it (e.g.
    // "SELECT+R1 OR F7").
    void set_hint(const std::string& hint);
    fs::path slot_path(int slot) const; // slot 0..11

    bool is_open() const { return open_; }
    void open();
    void close();

    // The physical pads, positionally (RCORE_PAD_* bits: SOUTH is the bottom
    // face button whatever it is labelled), OR-ed over every pad the host
    // has, and the left stick's Y (rcore sign: positive = up). Call once per
    // host frame whether or not the browser is open. Returns true when this
    // call opened it (the SELECT + R1 chord).
    bool poll_pad(std::uint32_t buttons, std::int16_t stick_y, std::uint64_t now_ms);

    enum class Key { Up, Down, Load, Save, Back };
    void key(Key k);
    void jump(int slot); // 0..11

    struct Request {
        enum Kind { None, Save, Load } kind = None;
        int slot = -1;
        fs::path path;
    };
    // What the player asked for since the last call; None if nothing. While
    // one is outstanding (until finish()) the browser shows it and takes no
    // other request.
    Request take_request();
    bool pending() const { return pending_.kind != Request::None; }
    // How the outstanding request went. On a refusal `detail` is the runner's
    // reason and the browser shows it; a successful load closes the browser.
    void finish(bool ok, const std::string& detail);

    // A line for the OSD once the browser has something to announce ("Slot 3
    // loaded"); empty when there is none.
    std::string take_toast();

    // The browser while open, else nullptr.
    const Layer* layer();

private:
    struct Slot {
        bool exists = false, readable = false;
        std::int64_t saved_unix = 0;
        std::uint64_t frame = 0;
        std::string mark;              // "OTHER CORE", "OTHER BUILD", "UNREADABLE"
        std::vector<std::uint8_t> thumb; // kThumbWidth x kThumbHeight RGBA8, or empty
    };
    void scan();
    void scan_slot(int i);
    void move(int delta);
    void request(Request::Kind kind);
    void rasterize();

    fs::path dir_;
    std::string core_id_, core_sha_, hint_ = "SELECT+R1";
    bool open_ = false;
    int selected_ = 0;
    Slot slots_[kSlots];
    Request pending_, ready_;
    std::string status_, detail_, toast_;

    std::uint32_t prev_buttons_ = 0;
    int repeat_dir_ = 0;
    std::uint64_t repeat_next_ms_ = 0;

    bool dirty_ = true;
    Image panel_;
    std::uint64_t rev_ = 0;
    Layer layer_;
};

// ---- Input guard -----------------------------------------------------------------

// Buttons held when an overlay closed stay out of the game until released:
// the chord that closed the browser, or the Enter that loaded a slot, must not
// also press Start in the game. Applied to the pads a host grants.
class InputGuard {
public:
    void arm(const rcore_pad* pads, std::size_t count);
    void apply(rcore_pad* pads, std::size_t count);

private:
    std::uint32_t mask_ = 0;
};

} // namespace retro::overlay
