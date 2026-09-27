#pragma once

// retro_overlay -- what a host draws over a running core: the FPS readout
// (top left), the TURBO marker (top right), the volume meter (right edge),
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

// ---- OSD: FPS, TURBO, volume, toasts ------------------------------------------

class Osd {
public:
    Osd();

    // FPS readout, top left. The host's "show FPS" setting and its hotkey
    // both set this; the OSD only draws what it is told.
    void set_fps_visible(bool on);
    bool fps_visible() const { return fps_visible_; }

    // Once per EMULATED frame -- each frame the core finished -- not per
    // present. Under turbo the core runs several frames per present, and the
    // readout should say how fast the machine is running, which is what a
    // player holding turbo is asking. Frames over the time they span, for the
    // last 64 -- not a mean of 1/dt, which reads high whenever frames arrive
    // unevenly. A gap of more than half a second starts it afresh rather than
    // averaging the gap in.
    void note_frame(std::uint64_t now_ns);
    double fps() const;
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
    void rasterize_volume();

    bool fps_visible_ = false;
    bool turbo_ = false;

    static constexpr int kFpsWindow = 64;
    std::uint64_t stamps_[kFpsWindow]{}; // when each of the last frames finished
    int pos_ = 0, count_ = 0;

    std::string toast_;
    std::uint64_t toast_until_ns_ = 0;

    int volume_ = -1, volume_drawn_ = -1;
    std::uint64_t volume_until_ns_ = 0;

    std::string status_drawn_;
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
