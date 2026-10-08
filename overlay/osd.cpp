// The OSD: frame-rate readout, TURBO marker, volume meter, toasts.
//
// Carried from snesrecomp's snes_osd.c (itself psxrecomp's host_osd.c): the
// same font at 2x, the same boxes, the same volume meter, rates counted on
// emulated frames. What changed is where the pieces sit -- TURBO has its own
// corner instead of trailing the FPS line -- that the time comes from the
// host instead of SDL, and that the one FPS number became three (FPS, VI,
// ms/VI: n64lle's own host showed the same three, host_present.rs).

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
constexpr std::uint64_t kRateWindowNs = 1000 * kNsPerMs; // rates span the last second
constexpr std::uint64_t kReadoutEveryNs = 250 * kNsPerMs; // digits a player can read
constexpr int kLabelCols = 6;                              // "ms/VI " and the value after it
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
    readout_.clear();
}

// The readout keeps the text it last showed until the new window can say
// something, then refreshes at once.
void Osd::restart_fps() {
    pos_ = count_ = 0;
    readout_stale_ = true;
}

void Osd::note_counters(std::uint64_t now_ns, const FrameCounters& c) {
    if (c.game) game_counted_ = true;
    if (count_) {
        const Sample& last = samples_[(pos_ + kSamples - 1) % kSamples];
        if (c.frames == last.c.frames) return; // nothing ran since
        const bool back =
            c.frames < last.c.frames || c.game < last.c.game || c.work_ns < last.c.work_ns;
        if (back || now_ns < last.ns || now_ns - last.ns > kFpsGapNs) restart_fps();
    }
    samples_[pos_] = {now_ns, c};
    pos_ = (pos_ + 1) % kSamples;
    if (count_ < kSamples) ++count_;
}

void Osd::note_frame(std::uint64_t now_ns) {
    // Counted on from the last sample, so a gap restarts the window but the
    // total never goes backwards.
    note_counters(now_ns, {++own_frames_, 0, 0});
}

FrameRates Osd::rates() const {
    FrameRates r;
    if (count_ < 2) return r;
    auto at = [&](int i) -> const Sample& { return samples_[(pos_ + kSamples - count_ + i) % kSamples]; };
    const Sample& n = at(count_ - 1);
    int i = 0;
    while (i < count_ - 2 && n.ns - at(i).ns > kRateWindowNs) ++i;
    const Sample& o = at(i);
    if (n.ns <= o.ns) return r;
    const double dt = double(n.ns - o.ns);
    const std::uint64_t frames = n.c.frames - o.c.frames;
    r.vi = double(frames) * 1e9 / dt;
    // FPS spans the game's own pictures: from the first sample in the window
    // where the count moved to the last, so a window edge falling between two
    // pictures does not read 29.5 or 30.5 for a game drawing at 30.
    r.has_fps = game_counted_;
    if (r.has_fps) {
        int first = -1, last = -1;
        for (int k = i > 0 ? i : 1; k < count_; ++k) {
            if (at(k).c.game == at(k - 1).c.game) continue;
            if (first < 0) first = k;
            last = k;
        }
        if (first >= 0 && last > first)
            r.fps = double(at(last).c.game - at(first).c.game) * 1e9 /
                    double(at(last).ns - at(first).ns);
    }
    const std::uint64_t work = n.c.work_ns - o.c.work_ns;
    r.has_ms = frames && work;
    if (r.has_ms) r.ms_per_vi = double(work) / 1e6 / double(frames);
    return r;
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

// "FPS    30.0" / "VI     60.0" / "ms/VI  4.21", or "--" where the reading
// is not available.
std::string Osd::format_readout(const FrameRates& r, bool have_window) {
    char fps[16] = "--", vi[16] = "--", ms[16] = "--";
    if (have_window) std::snprintf(vi, sizeof vi, "%.1f", r.vi);
    if (have_window && r.has_fps) std::snprintf(fps, sizeof fps, "%.1f", r.fps);
    if (have_window && r.has_ms) std::snprintf(ms, sizeof ms, "%.2f", r.ms_per_vi);
    char buf[96];
    std::snprintf(buf, sizeof buf, "%-*s%5s\n%-*s%5s\n%-*s%5s", kLabelCols, "FPS", fps, kLabelCols,
                  "VI", vi, kLabelCols, "ms/VI", ms);
    return buf;
}

void Osd::rasterize_status() {
    // status_drawn_ holds the rows, '\n'-separated: the readout's three (if
    // showing; labels dimmer than their values), then the toast (if any).
    std::vector<std::string> rows;
    for (std::size_t at = 0;;) {
        const auto nl = status_drawn_.find('\n', at);
        rows.push_back(status_drawn_.substr(at, nl == std::string::npos ? nl : nl - at));
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    std::size_t widest = 0;
    for (const std::string& r : rows) widest = std::max(widest, r.size());
    const int n = int(rows.size());
    status_.resize(std::uint32_t(kPad * 2 + int(widest) * kGlyph * kScale),
                   std::uint32_t(kPad * 2 + n * kGlyph * kScale + (n - 1) * kRowGap), color::kPanel);
    int y = kPad;
    for (int i = 0; i < n; ++i, y += (kGlyph + 1) * kScale) {
        if (i < status_readout_rows_ && int(rows[i].size()) > kLabelCols) {
            text(status_, kPad, y, rows[i].substr(0, kLabelCols), color::kSub, kScale);
            text(status_, kPad + kLabelCols * kGlyph * kScale, y, rows[i].substr(kLabelCols),
                 color::kText, kScale);
        } else {
            text(status_, kPad, y, rows[i], color::kText, kScale);
        }
    }
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

    if (fps_visible_) {
        // Refreshed four times a second, and at once when a fresh window
        // first has two samples; until then the last reading stands.
        const bool have_window = count_ >= 2;
        if (readout_.empty() ||
            (have_window && (readout_stale_ || now_ns - readout_ns_ >= kReadoutEveryNs))) {
            readout_ = format_readout(rates(), have_window);
            readout_ns_ = now_ns;
            readout_stale_ = !have_window;
        }
    }
    const std::string fps_rows = fps_visible_ ? readout_ : std::string();
    if (!toast_.empty() && now_ns >= toast_until_ns_) toast_.clear();
    const std::string want = toast_.empty()    ? fps_rows
                             : fps_rows.empty() ? toast_
                                                : fps_rows + "\n" + toast_;
    if (!want.empty()) {
        const int readout_rows = fps_rows.empty() ? 0 : 3;
        if (want != status_drawn_ || readout_rows != status_readout_rows_) {
            status_drawn_ = want;
            status_readout_rows_ = readout_rows;
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
