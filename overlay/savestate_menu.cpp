// The save-state browser.
//
// Carried from n64lle's host_savestate_menu.rs, which carried psxrecomp's
// psx_savestate_menu.c: the same 640x480 panel, three rows of twelve slots
// with a thumbnail each, the same colours and the same footer legend. What
// changed:
//   - It does not save or load anything. It asks, and the host carries the
//     request to the runner, which writes and checks the envelope
//     (state/state_envelope.hpp). A refusal comes back as the runner's
//     reason, and the panel shows it.
//   - The thumbnail is inside the envelope, not a sidecar file.
//   - Face buttons are read and drawn by POSITION, so the legend is the same
//     for every console: n64lle drew the N64's A / Z / B.

#include "overlay.hpp"
#include "overlay_raster.hpp"

#include "state_envelope.hpp"

#include <cctype>
#include <cstdio>
#include <ctime>

namespace retro::overlay {

namespace {

using namespace raster;

constexpr int kW = 640, kH = 480;
constexpr int kVisibleRows = 3;
constexpr int kRowsX = 28, kRowsY = 62, kRowsW = 584, kRowH = 112, kRowGap = 8;
constexpr int kTileW = 136, kTileH = 102;
constexpr int kFooterY = 418; // below the third row (62 + 3 * 112 + 2 * 8 = 414)

// A held direction waits this long, then repeats this often.
constexpr std::uint64_t kRepeatDelayMs = 350;
constexpr std::uint64_t kRepeatRateMs = 90;
constexpr int kStickDeadzone = 16000;

constexpr std::uint32_t kChord = RCORE_PAD_SELECT | RCORE_PAD_R1;

std::string two_digits(int n) {
    char b[8];
    std::snprintf(b, sizeof b, "%02d", n);
    return b;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string local_time(std::int64_t unix_s) {
    const std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
    return b;
}

// Runs of 16+ hex digits (the runner names hashes in full) shortened to 12,
// so a refusal fits the panel. The whole sentence is in the runner's log.
std::string shorten_hashes(const std::string& s) {
    std::string o;
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t j = i;
        while (j < s.size() && std::isxdigit(static_cast<unsigned char>(s[j]))) ++j;
        if (j - i >= 16) {
            o += s.substr(i, 12) + "..";
            i = j;
        } else if (j > i) {
            o += s.substr(i, j - i);
            i = j;
        } else {
            o += s[i++];
        }
    }
    return o;
}

// Word-wrapped to `cols` characters, at most `max_lines` lines.
std::vector<std::string> wrap(const std::string& s, std::size_t cols, std::size_t max_lines) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flush_word = [&] {
        if (word.empty()) return;
        if (!line.empty() && line.size() + 1 + word.size() > cols) {
            lines.push_back(line);
            line.clear();
        }
        while (word.size() > cols) {
            lines.push_back(word.substr(0, cols));
            word.erase(0, cols);
        }
        line += (line.empty() ? "" : " ") + word;
        word.clear();
    };
    for (char c : s) {
        if (c == ' ') flush_word();
        else word += c;
    }
    flush_word();
    if (!line.empty()) lines.push_back(line);
    if (lines.size() > max_lines) {
        lines.resize(max_lines);
        std::string& last = lines.back();
        if (last.size() + 2 > cols) last.resize(cols - 2);
        last += "..";
    }
    return lines;
}

// A face button by position: four dots in a diamond, the one meant filled.
void face_button(Image& img, int x, int y, std::uint32_t which, Argb c) {
    struct Dot {
        std::uint32_t bit;
        int dx, dy;
    };
    static constexpr Dot kDots[] = {
        {RCORE_PAD_NORTH, 6, 0},
        {RCORE_PAD_WEST, 0, 6},
        {RCORE_PAD_EAST, 12, 6},
        {RCORE_PAD_SOUTH, 6, 12},
    };
    for (const Dot& d : kDots) {
        if (d.bit == which) fill(img, x + d.dx, y + d.dy, 6, 6, c);
        else stroke(img, x + d.dx, y + d.dy, 6, 6, color::kDim);
    }
}

} // namespace

SavestateMenu::SavestateMenu() { panel_.resize(kW, kH, color::kBrowserBg); }

void SavestateMenu::configure(const fs::path& dir, const std::string& core_id,
                              const std::string& core_sha256) {
    dir_ = dir;
    core_id_ = core_id;
    core_sha_ = core_sha256;
    if (open_) scan();
    dirty_ = true;
}

void SavestateMenu::set_hint(const std::string& hint) {
    hint_ = upper(hint);
    dirty_ = true;
}

fs::path SavestateMenu::slot_path(int slot) const {
    return dir_ / ("slot" + two_digits(slot + 1) + ".rstate");
}

void SavestateMenu::scan_slot(int i) {
    Slot& s = slots_[i];
    s = Slot{};
    const fs::path p = slot_path(i);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return;
    s.exists = true;
    state::StateHeader h;
    std::string err;
    if (!state::read_state_header(p, h, &s.thumb, &err)) {
        s.mark = "UNREADABLE";
        s.thumb.clear();
        return;
    }
    s.readable = true;
    s.saved_unix = h.saved_unix;
    s.frame = h.frame_number;
    if (h.thumb_w != state::kThumbWidth || h.thumb_h != state::kThumbHeight) s.thumb.clear();
    if (!core_id_.empty() && h.identity.core_id != core_id_) {
        s.mark = "OTHER CORE";
    } else if (!h.identity.state_compat_id && !core_sha_.empty() &&
               h.identity.core_sha256 != core_sha_) {
        s.mark = "OTHER BUILD";
    }
}

void SavestateMenu::scan() {
    for (int i = 0; i < kSlots; ++i) scan_slot(i);
}

void SavestateMenu::open() {
    if (open_) return;
    open_ = true;
    status_.clear();
    detail_.clear();
    repeat_dir_ = 0;
    scan();
    dirty_ = true;
}

void SavestateMenu::close() {
    open_ = false;
    repeat_dir_ = 0;
    status_.clear();
    detail_.clear();
    dirty_ = true;
}

void SavestateMenu::move(int delta) {
    selected_ = ((selected_ + delta) % kSlots + kSlots) % kSlots;
    status_.clear();
    detail_.clear();
    dirty_ = true;
}

void SavestateMenu::jump(int slot) {
    if (!open_ || pending() || slot < 0 || slot >= kSlots) return;
    selected_ = slot;
    status_.clear();
    detail_.clear();
    dirty_ = true;
}

void SavestateMenu::request(Request::Kind kind) {
    if (pending()) return;
    const std::string n = two_digits(selected_ + 1);
    detail_.clear();
    if (kind == Request::Load && !slots_[selected_].exists) {
        status_ = "SLOT " + n + " IS EMPTY";
        dirty_ = true;
        return;
    }
    pending_.kind = kind;
    pending_.slot = selected_;
    pending_.path = slot_path(selected_);
    ready_ = pending_;
    status_ = (kind == Request::Save ? "SAVING SLOT " : "LOADING SLOT ") + n + "...";
    dirty_ = true;
}

SavestateMenu::Request SavestateMenu::take_request() {
    Request r = ready_;
    ready_ = Request{};
    return r;
}

void SavestateMenu::finish(bool ok, const std::string& detail) {
    const Request r = pending_;
    pending_ = Request{};
    ready_ = Request{};
    if (r.kind == Request::None) return;
    const std::string n = two_digits(r.slot + 1);
    const std::string plain = std::to_string(r.slot + 1);
    detail_.clear();
    if (r.kind == Request::Save) {
        if (ok) {
            // Stays open: the picture appearing in the row is the player's
            // evidence the state was written.
            status_ = "SAVED SLOT " + n;
            toast_ = "Slot " + plain + " saved";
            scan_slot(r.slot);
        } else {
            status_ = "SAVE FAILED: SLOT " + n;
            detail_ = detail;
        }
    } else if (ok) {
        toast_ = "Slot " + plain + " loaded";
        close();
    } else {
        // A refused load left the machine as it was; the slot stays listed.
        status_ = "LOAD REFUSED: SLOT " + n;
        detail_ = detail;
    }
    dirty_ = true;
}

std::string SavestateMenu::take_toast() {
    std::string t;
    t.swap(toast_);
    return t;
}

bool SavestateMenu::poll_pad(std::uint32_t buttons, std::int16_t stick_y, std::uint64_t now_ms) {
    const std::uint32_t prev = prev_buttons_;
    prev_buttons_ = buttons;
    const std::uint32_t pressed = buttons & ~prev;
    if (!open_) {
        // An edge on the WHOLE chord, not on either button: holding SELECT
        // and then tapping R1 is the gesture, and so is the reverse.
        if ((buttons & kChord) == kChord && (prev & kChord) != kChord) {
            open();
            return true;
        }
        return false;
    }
    if (pending()) return false;
    // The chord closes as well as opens, so an accidental open is undone the
    // same way.
    if ((pressed & kChord) && (buttons & kChord) == kChord) {
        close();
        return false;
    }
    if (pressed & (RCORE_PAD_START | RCORE_PAD_EAST)) {
        close();
        return false;
    }
    if (pressed & RCORE_PAD_SOUTH) {
        request(Request::Load);
        return false;
    }
    if (pressed & RCORE_PAD_NORTH) {
        request(Request::Save);
        return false;
    }
    const int dir = (buttons & RCORE_PAD_DPAD_DOWN) || stick_y < -kStickDeadzone ? 1
                    : (buttons & RCORE_PAD_DPAD_UP) || stick_y > kStickDeadzone  ? -1
                                                                                 : 0;
    if (!dir) {
        repeat_dir_ = 0;
        return false;
    }
    if (dir != repeat_dir_) {
        repeat_dir_ = dir;
        repeat_next_ms_ = now_ms + kRepeatDelayMs;
        move(dir);
    } else if (now_ms >= repeat_next_ms_) {
        repeat_next_ms_ = now_ms + kRepeatRateMs;
        move(dir);
    }
    return false;
}

void SavestateMenu::key(Key k) {
    if (!open_ || pending()) return;
    switch (k) {
        case Key::Up: move(-1); break;
        case Key::Down: move(1); break;
        case Key::Load: request(Request::Load); break;
        case Key::Save: request(Request::Save); break;
        case Key::Back: close(); break;
    }
}

void SavestateMenu::rasterize() {
    Image& p = panel_;
    p.resize(kW, kH, color::kBrowserBg);

    fill(p, 0, 0, kW, 46, color::kBar);
    text(p, 24, 14, "SAVE STATES", color::kGold, 2);
    text(p, kW - 24 - text_width(hint_), 18, hint_, color::kHint);

    int first = selected_ - 1;
    if (first < 0) first = 0;
    if (first > kSlots - kVisibleRows) first = kSlots - kVisibleRows;
    text(p, 24, 48,
         two_digits(first + 1) + "-" + two_digits(first + kVisibleRows) + " / " +
             two_digits(kSlots),
         color::kDim);

    for (int i = first; i < first + kVisibleRows; ++i) {
        const Slot& s = slots_[i];
        const int y = kRowsY + (i - first) * (kRowH + kRowGap);
        const bool sel = i == selected_;
        fill(p, kRowsX, y, kRowsW, kRowH, sel ? color::kRowSelected : color::kRow);
        stroke(p, kRowsX, y, kRowsW, kRowH, sel ? color::kGold : color::kRowEdge);
        text(p, kRowsX + 16, y + 16, "SLOT " + two_digits(i + 1),
             sel ? color::kGold : color::kLabel);

        const int tx = kRowsX + 112, ty = y + 5;
        if (!s.thumb.empty()) {
            blit(p, tx, ty, kTileW, kTileH, s.thumb.data(), state::kThumbWidth,
                 state::kThumbHeight);
        } else {
            fill(p, tx, ty, kTileW, kTileH, color::kTile);
            const std::string what = s.exists ? "SAVED" : "NEW";
            text(p, tx + (kTileW - text_width(what)) / 2, ty + 47, what,
                 s.exists ? color::kSaved : color::kFaint);
        }
        stroke(p, tx, ty, kTileW, kTileH, color::kTileEdge);

        const int cx = kRowsX + 272;
        const Argb sub = sel ? color::kText : color::kSub;
        if (!s.exists) {
            text(p, cx, y + 48, "EMPTY", sub);
        } else if (s.readable) {
            text(p, cx, y + 40, local_time(s.saved_unix), sub);
            text(p, cx, y + 56, "FRAME " + std::to_string(s.frame), color::kDim);
        }
        if (!s.mark.empty()) text(p, cx, y + 72, s.mark, color::kWarn);
    }

    fill(p, 0, kFooterY, kW, kH - kFooterY, color::kBar);
    face_button(p, 28, 424, RCORE_PAD_SOUTH, color::kLoad);
    text(p, 52, 429, "LOAD", color::kLabel);
    face_button(p, 128, 424, RCORE_PAD_NORTH, color::kSave);
    text(p, 152, 429, "SAVE", color::kLabel);
    face_button(p, 228, 424, RCORE_PAD_EAST, color::kBack);
    text(p, 252, 429, "BACK", color::kLabel);
    text(p, 340, 429, "D-PAD OR STICK  SLOT", color::kDim);
    if (detail_.empty()) {
        text(p, 28, 450, "KEYS: UP/DOWN SLOT  ENTER LOAD  S SAVE  ESC BACK  1-0 - = JUMP",
             color::kHint);
        if (!status_.empty()) text(p, 28, 466, status_, color::kGold);
    } else {
        // A refusal: the label, then the runner's reason in two lines.
        text(p, 28, 448, status_, color::kGold);
        int y = 460;
        for (const std::string& l : wrap(shorten_hashes(detail_), 73, 2)) {
            text(p, 28, y, l, color::kWarn);
            y += 10;
        }
    }
    ++rev_;
    dirty_ = false;
}

const Layer* SavestateMenu::layer() {
    if (!open_) return nullptr;
    if (dirty_) rasterize();
    layer_ = {kLayerSavestates, &panel_, rev_, Anchor::Center};
    return &layer_;
}

void InputGuard::arm(const rcore_pad* pads, std::size_t count) {
    std::uint32_t held = 0;
    for (std::size_t i = 0; i < count; ++i) held |= pads[i].buttons;
    mask_ |= held;
}

void InputGuard::apply(rcore_pad* pads, std::size_t count) {
    if (!mask_) return;
    std::uint32_t held = 0;
    for (std::size_t i = 0; i < count; ++i) held |= pads[i].buttons;
    mask_ &= held; // a released button is free again
    for (std::size_t i = 0; i < count; ++i) pads[i].buttons &= ~mask_;
}

} // namespace retro::overlay
