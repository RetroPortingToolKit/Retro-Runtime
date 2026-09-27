// retro-overlay-test -- the overlay (overlay/overlay.hpp) and the savestate
// envelope (state/state_envelope.hpp), without a window or a core.
//
//   retro-overlay-test <scratch dir>             run the checks
//   retro-overlay-test <scratch dir> --dump DIR  also write each layer, drawn
//                                                over a grey picture, as PPM
//                                                files to look at

#include "overlay.hpp"
#include "state_envelope.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using namespace retro;
using namespace retro::overlay;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

constexpr std::uint64_t kMs = 1000000ull;

const Layer* find(const std::vector<Layer>& v, std::uint32_t id) {
    for (const Layer& l : v)
        if (l.id == id) return &l;
    return nullptr;
}

// Alpha of the pixel at (x, y).
unsigned alpha_at(const Image& img, std::uint32_t x, std::uint32_t y) {
    return img.rgba[(std::size_t(y) * img.width + x) * 4 + 3];
}

void test_osd() {
    Osd osd;
    std::uint64_t t = 1000 * kMs;
    CHECK(osd.layers(t).empty());

    // FPS: 60 Hz frames read 60, whether or not the readout is showing.
    osd.set_fps_visible(true);
    for (int i = 0; i < 100; ++i) osd.note_frame(t += 16666667ull);
    CHECK(osd.fps() > 59.9 && osd.fps() < 60.1);
    const Layer* st = find(osd.layers(t), kLayerStatus);
    CHECK(st && st->anchor == Anchor::TopLeft && st->image->width > 0);
    // "60 FPS": six glyphs at 2x plus padding.
    CHECK(st && st->image->width == 2 * 4 + 6 * 16);
    const std::uint64_t rev = st ? st->revision : 0;
    osd.note_frame(t += 16666667ull);
    st = find(osd.layers(t), kLayerStatus);
    CHECK(st && st->revision == rev); // same text, no redraw

    // Uneven arrival -- 15 ms, 25 ms, ... -- is still about 50 frames a second
    // (63 intervals in the window, so 49.8 or 50.2); a mean of 1/dt says 53.3.
    for (int i = 0; i < 64; ++i) osd.note_frame(t += (i & 1) ? 25 * kMs : 15 * kMs);
    CHECK(osd.fps() > 49.5 && osd.fps() < 50.5);
    // Under turbo frames come faster than presents: the readout follows them,
    // from the moment turbo starts.
    osd.set_turbo(true);
    for (int i = 0; i < 10; ++i) osd.note_frame(t += 4166667ull);
    CHECK(osd.fps() > 239.0 && osd.fps() < 241.0);
    osd.set_turbo(false);
    CHECK(osd.fps() == 0.0);
    osd.note_frame(t += 20 * kMs);
    osd.note_frame(t += 20 * kMs);
    CHECK(osd.fps() > 49.9 && osd.fps() < 50.1);
    // A pause is a break, not a slow frame.
    osd.note_frame(t += 2000 * kMs);
    CHECK(osd.fps() == 0.0);
    osd.note_frame(t += 20 * kMs);
    CHECK(osd.fps() > 49.9 && osd.fps() < 50.1);

    // TURBO sits alone in the top right.
    osd.set_turbo(true);
    const Layer* tb = find(osd.layers(t), kLayerTurbo);
    CHECK(tb && tb->anchor == Anchor::TopRight);
    osd.set_turbo(false);
    CHECK(!find(osd.layers(t), kLayerTurbo));

    // Volume: shown for 1.5 s, clamped.
    osd.show_volume(140, t);
    const Layer* vol = find(osd.layers(t), kLayerVolume);
    CHECK(vol && vol->anchor == Anchor::RightMiddle);
    const std::uint64_t vrev = vol ? vol->revision : 0;
    osd.show_volume(90, t + 100 * kMs);
    vol = find(osd.layers(t + 100 * kMs), kLayerVolume);
    CHECK(vol && vol->revision != vrev);
    CHECK(find(osd.layers(t + 1500 * kMs), kLayerVolume));
    CHECK(!find(osd.layers(t + 1601 * kMs), kLayerVolume));

    // A toast adds a row under the FPS line, then goes.
    const std::uint32_t one_row = find(osd.layers(t), kLayerStatus)->image->height;
    osd.toast("Slot 3 saved", t, 2000);
    const Layer* two = find(osd.layers(t), kLayerStatus);
    CHECK(two && two->image->height > one_row);
    CHECK(find(osd.layers(t + 2001 * kMs), kLayerStatus)->image->height == one_row);
    osd.set_fps_visible(false);
    CHECK(!find(osd.layers(t + 2001 * kMs), kLayerStatus));
    osd.toast("Slot 1 loaded", t, 500);
    CHECK(find(osd.layers(t), kLayerStatus)); // a toast shows without the FPS line
}

void test_place() {
    Image small;
    small.resize(100, 20, 0);
    Layer l{kLayerStatus, &small, 1, Anchor::TopLeft};
    Rect r = place(l, 1920, 1080);
    CHECK(r.x == 8 && r.y == 8 && r.w == 100 && r.h == 20);
    r = place(l, 3840, 2160); // 4K: 2x, inset 16
    CHECK(r.x == 16 && r.y == 16 && r.w == 200 && r.h == 40);
    l.anchor = Anchor::TopRight;
    r = place(l, 1920, 1080);
    CHECK(r.x == 1920 - 100 - 8 && r.y == 8);
    l.anchor = Anchor::RightMiddle;
    r = place(l, 1920, 1080);
    CHECK(r.x == 1920 - 100 - 8 && r.y == (1080 - 20) / 2);

    Image panel;
    panel.resize(640, 480, 0);
    Layer p{kLayerSavestates, &panel, 1, Anchor::Center};
    r = place(p, 1920, 1080); // 2x fits 90% of 1080
    CHECK(r.w == 1280 && r.h == 960 && r.x == 320 && r.y == 60);
    r = place(p, 640, 480); // smaller than 90%: shrinks
    CHECK(r.w < 640 && r.h < 480);
}

void write_slot(const fs::path& path, const std::string& core_id, const std::string& sha,
                std::uint64_t frame) {
    state::StateHeader h;
    h.identity.core_id = core_id;
    h.identity.core_sha256 = sha;
    h.frame_number = frame;
    h.saved_unix = 1790000000;
    std::vector<std::uint8_t> px(64 * 48 * 4, 0x80);
    for (std::size_t i = 0; i < 64 * 48; ++i) px[i * 4] = static_cast<std::uint8_t>(i % 64 * 4);
    const auto thumb = state::make_thumbnail(px.data(), 64, 48, 64 * 4);
    const char bytes[] = "state";
    std::string err;
    CHECK(state::write_state(path, h, thumb, bytes, sizeof bytes, &err));
}

void test_menu(const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    SavestateMenu m;
    m.configure(dir, "fake", "aaaa");
    CHECK(m.slot_path(0) == dir / "slot01.rstate");
    CHECK(m.slot_path(11) == dir / "slot12.rstate");

    // The chord opens on its edge only, from either order.
    CHECK(!m.poll_pad(RCORE_PAD_SELECT, 0, 0));
    CHECK(m.poll_pad(RCORE_PAD_SELECT | RCORE_PAD_R1, 0, 10));
    CHECK(m.is_open() && m.layer());
    CHECK(!m.poll_pad(RCORE_PAD_SELECT | RCORE_PAD_R1, 0, 20)); // held: nothing
    m.poll_pad(0, 0, 30);

    // Loading an empty slot asks nothing.
    m.poll_pad(RCORE_PAD_SOUTH, 0, 40);
    CHECK(m.take_request().kind == SavestateMenu::Request::None);
    m.poll_pad(0, 0, 50);

    // Save: one request, then nothing until finish().
    m.poll_pad(RCORE_PAD_NORTH, 0, 60);
    SavestateMenu::Request r = m.take_request();
    CHECK(r.kind == SavestateMenu::Request::Save && r.slot == 0 && r.path == m.slot_path(0));
    CHECK(m.pending());
    m.poll_pad(0, 0, 70);
    m.poll_pad(RCORE_PAD_NORTH, 0, 80);
    CHECK(m.take_request().kind == SavestateMenu::Request::None);
    write_slot(r.path, "fake", "aaaa", 1234); // what the runner does
    m.finish(true, "");
    CHECK(m.is_open() && !m.pending());       // a save stays open
    CHECK(m.take_toast() == "Slot 1 saved");

    // Down with repeat: one step at once, then after 350 ms every 90 ms.
    m.poll_pad(0, 0, 100);
    m.poll_pad(RCORE_PAD_DPAD_DOWN, 0, 200);  // -> slot 2
    m.poll_pad(RCORE_PAD_DPAD_DOWN, 0, 500);  // not yet
    m.poll_pad(RCORE_PAD_DPAD_DOWN, 0, 551);  // -> slot 3
    m.poll_pad(RCORE_PAD_DPAD_DOWN, 0, 600);  // not yet
    m.poll_pad(RCORE_PAD_DPAD_DOWN, 0, 641);  // -> slot 4
    m.poll_pad(0, -30000, 700);               // stick down: new direction? no, same
    m.poll_pad(0, 0, 710);
    m.poll_pad(RCORE_PAD_NORTH, 0, 720);
    r = m.take_request();
    CHECK(r.kind == SavestateMenu::Request::Save && r.slot == 3);
    m.finish(false, "the core could not size its state");
    CHECK(m.is_open() && !m.pending());

    // Keys: jump, load; a refused load stays open, a good one closes.
    m.jump(0);
    m.key(SavestateMenu::Key::Load);
    r = m.take_request();
    CHECK(r.kind == SavestateMenu::Request::Load && r.slot == 0);
    m.finish(false, "refused: core build (SHA-256; this core binds states to its exact file) "
                    "differs: the state has 0123456789abcdef0123456789abcdef0123456789abcdef0123"
                    "456789abcdef, this session has fedcba9876543210fedcba9876543210fedcba98765432"
                    "10fedcba9876543210");
    CHECK(m.is_open() && m.layer());
    m.key(SavestateMenu::Key::Load);
    r = m.take_request();
    m.finish(true, "");
    CHECK(!m.is_open() && !m.layer());
    CHECK(m.take_toast() == "Slot 1 loaded");

    // Another build's slot is marked; the runner still decides.
    write_slot(m.slot_path(1), "fake", "bbbb", 5);
    write_slot(m.slot_path(2), "other", "aaaa", 5);
    m.open();
    CHECK(m.layer());
    m.key(SavestateMenu::Key::Back);
    CHECK(!m.is_open());

    // East and Start close too.
    m.open();
    m.poll_pad(0, 0, 1000);
    m.poll_pad(RCORE_PAD_EAST, 0, 1010);
    CHECK(!m.is_open());
}

void test_guard() {
    InputGuard g;
    rcore_pad pads[2]{};
    pads[0].buttons = RCORE_PAD_START | RCORE_PAD_R1;
    g.arm(pads, 2);
    pads[0].buttons = RCORE_PAD_START | RCORE_PAD_SOUTH;
    g.apply(pads, 2);
    CHECK(pads[0].buttons == RCORE_PAD_SOUTH); // START still held: kept out
    pads[0].buttons = RCORE_PAD_START;         // R1 was released in between
    g.apply(pads, 2);
    CHECK(pads[0].buttons == 0);               // START never let go
    pads[0].buttons = RCORE_PAD_R1;            // R1 pressed again: it is free
    g.apply(pads, 2);
    CHECK(pads[0].buttons == RCORE_PAD_R1);
    pads[0].buttons = 0;
    g.apply(pads, 2);
    pads[0].buttons = RCORE_PAD_START;
    g.apply(pads, 2);
    CHECK(pads[0].buttons == RCORE_PAD_START);
}

void test_envelope(const fs::path& dir) {
    state::StateIdentity id;
    id.abi_major = 0;
    id.core_id = "fake";
    id.core_sha256 = "c0";
    id.package_sha256 = "p0";
    id.content_sha256 = "r0";
    id.accessories.push_back({0, 0, "n64.transfer_pak", "g0"});
    id.sim_options["cpu.overclock"] = "1";
    id.sim_options["region"] = std::nullopt;
    id.sim_options["note"] = "a=b\nc\\d"; // escaping survives
    state::StateHeader h;
    h.identity = id;
    h.frame_number = 77;
    h.saved_unix = 1790000000;
    const std::vector<std::uint8_t> bytes = {1, 2, 3, 4, 5};
    const fs::path p = dir / "env.rstate";
    std::string err;
    CHECK(state::write_state(p, h, {}, bytes.data(), bytes.size(), &err));
    CHECK(state::is_envelope(p));
    state::StateHeader back;
    std::vector<std::uint8_t> got;
    CHECK(state::read_state(p, back, got, &err));
    CHECK(got == bytes && back.frame_number == 77 && back.thumb_w == 0);
    CHECK(back.identity.sim_options == id.sim_options);
    CHECK(back.identity.accessories.size() == 1 && back.identity.accessories[0] == id.accessories[0]);
    CHECK(state::check_state(back, id, got).empty());

    auto refused = [&](state::StateIdentity running, const std::string& want) {
        const std::string why = state::check_state(back, running, got);
        if (why.find(want) != 0) {
            std::fprintf(stderr, "  wanted \"%s...\", got \"%s\"\n", want.c_str(), why.c_str());
            ++g_failures;
        }
    };
    state::StateIdentity r = id;
    r.abi_major = 1;
    refused(r, "rcore ABI major differs");
    r = id;
    r.core_id = "other";
    refused(r, "core id differs: the state has fake, this session has other");
    r = id;
    r.core_sha256 = "c1";
    refused(r, "core build");
    r = id;
    r.state_compat_id = "v1"; // the running core promises; the state predates it
    refused(r, "state_compat_id differs");
    r = id;
    r.package_sha256 = "p1";
    refused(r, "game package SHA-256 differs");
    r = id;
    r.content_sha256 = "r1";
    refused(r, "content SHA-256 differs");
    r = id;
    r.accessories.clear();
    refused(r, "accessories differ");
    r = id;
    r.sim_options["cpu.overclock"] = "0";
    refused(r, "option cpu.overclock differs: the state has 1, this session has 0");
    r = id;
    r.sim_options.erase("region");
    refused(r, "option region differs");
    std::vector<std::uint8_t> corrupt = got;
    corrupt[2] ^= 1;
    CHECK(state::check_state(back, id, corrupt).find("corrupt") != std::string::npos);

    // The compat id, when both carry it, replaces the file hash.
    state::StateHeader promised = back;
    promised.identity.state_compat_id = "v1";
    r = id;
    r.state_compat_id = "v1";
    r.core_sha256 = "a rebuild";
    CHECK(state::check_state(promised, r, got).empty());

    // A bare state is not an envelope; a truncated one is refused, not trusted.
    const fs::path bare = dir / "bare.state";
    std::ofstream(bare, std::ios::binary) << "raw core bytes";
    CHECK(!state::is_envelope(bare));
    CHECK(!state::read_state_header(bare, back, nullptr, &err));
    fs::resize_file(p, fs::file_size(p) - 2);
    CHECK(!state::read_state(p, back, got, &err));
}

// Each layer over a mid-grey 640x480 "game", written as PPM.
void dump(const fs::path& out) {
    fs::create_directories(out);
    auto write = [&](const std::string& name, const std::vector<Layer>& layers, int w, int h) {
        std::vector<std::uint8_t> rgb(std::size_t(w) * h * 3);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::uint8_t g = ((x / 32 + y / 32) & 1) ? 0x60 : 0x70;
                std::memset(&rgb[(std::size_t(y) * w + x) * 3], g, 3);
            }
        for (const Layer& l : layers) {
            const Rect r = place(l, float(w), float(h));
            for (int y = 0; y < int(r.h); ++y)
                for (int x = 0; x < int(r.w); ++x) {
                    const int dx = int(r.x) + x, dy = int(r.y) + y;
                    if (dx < 0 || dy < 0 || dx >= w || dy >= h) continue;
                    const std::uint32_t sx = std::uint32_t(x * l.image->width / r.w);
                    const std::uint32_t sy = std::uint32_t(y * l.image->height / r.h);
                    const std::uint8_t* s = &l.image->rgba[(std::size_t(sy) * l.image->width + sx) * 4];
                    std::uint8_t* d = &rgb[(std::size_t(dy) * w + dx) * 3];
                    for (int c = 0; c < 3; ++c) d[c] = std::uint8_t((s[c] * s[3] + d[c] * (255 - s[3])) / 255);
                }
        }
        std::ofstream f(out / (name + ".ppm"), std::ios::binary);
        f << "P6\n" << w << ' ' << h << "\n255\n";
        f.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
    };
    Osd osd;
    std::uint64_t t = 1000 * kMs;
    osd.set_fps_visible(true);
    for (int i = 0; i < 70; ++i) osd.note_frame(t += 16683350ull);
    osd.set_turbo(true);
    osd.show_volume(70, t);
    osd.toast("Slot 3 saved", t);
    write("osd", osd.layers(t), 1280, 720);

    SavestateMenu m;
    const fs::path dir = out / "slots";
    fs::remove_all(dir);
    write_slot(dir / "slot01.rstate", "fake", "aaaa", 1234);
    write_slot(dir / "slot02.rstate", "fake", "bbbb", 99);
    m.configure(dir, "fake", "aaaa");
    m.set_hint("SELECT+R1 or F7");
    m.open();
    std::vector<Layer> layers;
    layers.push_back(*m.layer());
    write("savestates", layers, 1280, 720);
    m.key(SavestateMenu::Key::Load);
    m.take_request();
    m.finish(false, "refused: core build (SHA-256; this core binds states to its exact file) "
                    "differs: the state has 0123456789abcdef0123456789abcdef0123456789abcdef0123"
                    "456789abcdef, this session has fedcba9876543210fedcba9876543210fedcba98765432"
                    "10fedcba9876543210");
    layers[0] = *m.layer();
    write("savestates_refused", layers, 1280, 720);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: retro-overlay-test <scratch dir> [--dump DIR]\n");
        return 2;
    }
    const fs::path scratch = argv[1];
    fs::create_directories(scratch);
    test_osd();
    test_place();
    test_menu(scratch / "slots");
    test_guard();
    test_envelope(scratch);
    if (argc >= 4 && std::string(argv[2]) == "--dump") dump(argv[3]);
    // alpha_at keeps the image layout honest: the OSD box is translucent,
    // its text opaque.
    {
        Osd osd;
        osd.set_turbo(true);
        const Layer* tb = find(osd.layers(0), kLayerTurbo);
        CHECK(tb && alpha_at(*tb->image, 0, 0) == 0xC0);
    }
    std::printf("overlay-test: %s (%d failure(s))\n", g_failures ? "FAILED" : "ok", g_failures);
    return g_failures ? 1 : 0;
}
