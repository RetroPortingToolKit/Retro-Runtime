// The OSD: FPS readout, TURBO marker, volume meter, toasts.
//
// Carried from snesrecomp's snes_osd.c (itself psxrecomp's host_osd.c): the
// same font at 2x, the same boxes, the same volume meter, the same 64-frame
// FPS mean counted on emulated frames. What changed is where the pieces sit
// -- TURBO has its own corner instead of trailing the FPS line -- and that
// the time comes from the host instead of SDL.

#include "overlay.hpp"
#include "overlay_raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace retro::overlay {

namespace {

using namespace raster;

constexpr int kScale = 2;               // OSD text is the font at 2x
constexpr int kPad = 2 * kScale;        // inside every box
constexpr int kRowGap = 1 * kScale;
constexpr std::uint64_t kNsPerMs = 1000000ull;
constexpr std::uint64_t kFpsGapNs = 500 * kNsPerMs; // longer than this is a break
constexpr std::uint64_t kVolumeShowNs = 1500 * kNsPerMs;

// The volume meter, in font pixels (x kScale in the image).
constexpr int kVolBarW = 8;
constexpr int kVolBarH = 64;
constexpr int kVolPanelW = 2 * 2 + 4 * kGlyph; // "100%" fits
constexpr int kVolPanelH = 2 * 2 + kVolBarH + 3 + kGlyph + 1;

} // namespace

Osd::Osd() {
    // The marker never changes: draw it once.
    const std::string t = ">> TURBO";
    turbo_img_.resize(std::uint32_t(kPad * 2 + text_width(t, kScale)),
                      std::uint32_t(kPad * 2 + kGlyph * kScale), color::kPanel);
    text(turbo_img_, kPad, kPad, t, color::kGold, kScale);
}

void Osd::set_fps_visible(bool on) {
    if (on == fps_visible_) return;
    fps_visible_ = on;
    // Start from a clean window: an average built partly from before the
    // readout opened reports frames the player never watched.
    restart_fps();
}

void Osd::restart_fps() { pos_ = count_ = 0; }

void Osd::note_frame(std::uint64_t now_ns) {
    if (count_) {
        const std::uint64_t last = stamps_[(pos_ + kFpsWindow - 1) % kFpsWindow];
        if (now_ns < last || now_ns - last > kFpsGapNs) restart_fps();
    }
    stamps_[pos_] = now_ns;
    pos_ = (pos_ + 1) % kFpsWindow;
    if (count_ < kFpsWindow) ++count_;
}

double Osd::fps() const {
    if (count_ < 2) return 0.0;
    const std::uint64_t newest = stamps_[(pos_ + kFpsWindow - 1) % kFpsWindow];
    const std::uint64_t oldest = stamps_[(pos_ + kFpsWindow - count_) % kFpsWindow];
    return newest > oldest ? double(count_ - 1) * 1e9 / double(newest - oldest) : 0.0;
}

void Osd::set_turbo(bool on) {
    if (on == turbo_) return;
    turbo_ = on;
    restart_fps(); // the reading is of the speed the player is now asking for
}

void Osd::show_volume(int percent, std::uint64_t now_ns) {
    volume_ = std::clamp(percent, 0, 100);
    volume_until_ns_ = now_ns + kVolumeShowNs;
}

void Osd::toast(const std::string& text_in, std::uint64_t now_ns, std::uint32_t duration_ms) {
    if (text_in.empty()) return;
    toast_ = text_in.substr(0, 64);
    toast_until_ns_ = now_ns + std::uint64_t(duration_ms ? duration_ms : 2000) * kNsPerMs;
}

void Osd::rasterize_status() {
    // status_drawn_ holds "<fps line>\n<toast>"; either may be empty.
    const auto nl = status_drawn_.find('\n');
    const std::string fps_line = status_drawn_.substr(0, nl);
    const std::string toast_line = nl == std::string::npos ? "" : status_drawn_.substr(nl + 1);
    const int rows = (fps_line.empty() ? 0 : 1) + (toast_line.empty() ? 0 : 1);
    const std::size_t widest = std::max(fps_line.size(), toast_line.size());
    status_.resize(std::uint32_t(kPad * 2 + int(widest) * kGlyph * kScale),
                   std::uint32_t(kPad * 2 + rows * kGlyph * kScale + (rows - 1) * kRowGap),
                   color::kPanel);
    int y = kPad;
    if (!fps_line.empty()) {
        text(status_, kPad, y, fps_line, color::kText, kScale);
        y += (kGlyph + 1) * kScale;
    }
    if (!toast_line.empty()) text(status_, kPad, y, toast_line, color::kText, kScale);
    ++status_rev_;
}

void Osd::rasterize_volume() {
    constexpr int S = kScale;
    volume_img_.resize(kVolPanelW * S, kVolPanelH * S, color::kPanel);
    const int bar_x = (kVolPanelW - kVolBarW) / 2 * S;
    const int bar_y = 2 * S;
    // Trough, the level from the bottom, and a tick either side every 25%.
    fill(volume_img_, bar_x, bar_y, kVolBarW * S, kVolBarH * S, color::kTrough);
    const int level = (kVolBarH * volume_ + 50) / 100;
    fill(volume_img_, bar_x, bar_y + (kVolBarH - level) * S, kVolBarW * S, level * S,
         volume_ ? color::kText : color::kMuted);
    for (int q = 1; q < 4; ++q) {
        const int ty = bar_y + (kVolBarH * q / 4) * S;
        fill(volume_img_, bar_x - S, ty, S, S, color::kTick);
        fill(volume_img_, bar_x + kVolBarW * S, ty, S, S, color::kTick);
    }
    char pct[8];
    std::snprintf(pct, sizeof pct, "%d%%", volume_);
    const int text_x = (kVolPanelW - int(std::string(pct).size()) * kGlyph) / 2;
    text(volume_img_, text_x * S, (2 + kVolBarH + 3) * S, pct, color::kText, S);
    volume_drawn_ = volume_;
    ++volume_rev_;
}

const std::vector<Layer>& Osd::layers(std::uint64_t now_ns) {
    layers_.clear();

    std::string fps_line;
    if (fps_visible_) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%d FPS", int(std::lround(fps())));
        fps_line = buf;
    }
    if (!toast_.empty() && now_ns >= toast_until_ns_) toast_.clear();
    const std::string want = toast_.empty() ? fps_line : fps_line + "\n" + toast_;
    if (!want.empty()) {
        if (want != status_drawn_) {
            status_drawn_ = want;
            rasterize_status();
        }
        layers_.push_back({kLayerStatus, &status_, status_rev_, Anchor::TopLeft});
    } else {
        status_drawn_.clear();
    }

    if (turbo_) layers_.push_back({kLayerTurbo, &turbo_img_, 1, Anchor::TopRight});

    if (volume_ >= 0 && now_ns < volume_until_ns_) {
        if (volume_ != volume_drawn_) rasterize_volume();
        layers_.push_back({kLayerVolume, &volume_img_, volume_rev_, Anchor::RightMiddle});
    }
    return layers_;
}

} // namespace retro::overlay
