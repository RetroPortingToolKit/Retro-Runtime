#pragma once

// The overlay's drawing primitives: rectangles and 8x8 text into an Image.
// Internal to retro_overlay.

#include "overlay.hpp"

#include <cstdint>
#include <string>

namespace retro::overlay::raster {

constexpr int kGlyph = 8;

extern const std::uint8_t kFont8x8[95][8];

// The palette every overlay piece draws from (0xAARRGGBB). The browser's
// colours are n64lle's save-state browser's, which were psxrecomp's.
namespace color {
constexpr Argb kPanel = 0xC0202020u;     // OSD boxes: translucent dark grey
constexpr Argb kText = 0xFFFFFFFFu;
constexpr Argb kGold = 0xFFFFD24Du;      // TURBO, headings, the selection
constexpr Argb kTrough = 0xFF505050u;    // the volume meter's empty part
constexpr Argb kTick = 0xFFC0C0C0u;
constexpr Argb kMuted = 0xFF808080u;
// The browser.
constexpr Argb kBrowserBg = 0xFF0F1118u;
constexpr Argb kBar = 0xFF171B25u;
constexpr Argb kRow = 0xFF191D27u;
constexpr Argb kRowSelected = 0xFF2B2830u;
constexpr Argb kRowEdge = 0xFF303746u;
constexpr Argb kTile = 0xFF242A35u;
constexpr Argb kTileEdge = 0xFF3A4352u;
constexpr Argb kLabel = 0xFFE2E5EBu;
constexpr Argb kSub = 0xFFB2B8C2u;
constexpr Argb kHint = 0xFFB8BDC8u;
constexpr Argb kDim = 0xFF7F8796u;
constexpr Argb kFaint = 0xFF707887u;
constexpr Argb kSaved = 0xFF9AA3B2u;
constexpr Argb kWarn = 0xFFFF8A65u;
constexpr Argb kLoad = 0xFF6BE06Bu;      // green
constexpr Argb kSave = 0xFFB8BDC8u;      // grey
constexpr Argb kBack = 0xFF5FA8FFu;      // blue
constexpr Argb kButtonInk = 0xFF10131Au;
} // namespace color

void fill(Image& img, int x, int y, int w, int h, Argb c);
void stroke(Image& img, int x, int y, int w, int h, Argb c);
// `scale` pixels per font pixel. Characters outside 32..126 draw as '?'.
void text(Image& img, int x, int y, const std::string& s, Argb c, int scale = 1);
int text_width(const std::string& s, int scale = 1);
// An RGBA8 picture scaled (nearest) into a w x h box, opaque.
void blit(Image& img, int x, int y, int w, int h, const std::uint8_t* rgba, std::uint32_t src_w,
          std::uint32_t src_h);

} // namespace retro::overlay::raster
