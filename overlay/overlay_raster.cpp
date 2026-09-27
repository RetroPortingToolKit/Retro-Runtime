#include "overlay_raster.hpp"

#include <algorithm>

namespace retro::overlay {

namespace {

void put(Image& img, int x, int y, Argb c) {
    if (x < 0 || y < 0 || std::uint32_t(x) >= img.width || std::uint32_t(y) >= img.height) return;
    std::uint8_t* p = img.rgba.data() + (std::size_t(y) * img.width + std::size_t(x)) * 4;
    p[0] = static_cast<std::uint8_t>(c >> 16);
    p[1] = static_cast<std::uint8_t>(c >> 8);
    p[2] = static_cast<std::uint8_t>(c);
    p[3] = static_cast<std::uint8_t>(c >> 24);
}

} // namespace

void Image::resize(std::uint32_t w, std::uint32_t h, Argb c) {
    width = w;
    height = h;
    rgba.resize(std::size_t(w) * h * 4);
    for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
        rgba[i * 4 + 0] = static_cast<std::uint8_t>(c >> 16);
        rgba[i * 4 + 1] = static_cast<std::uint8_t>(c >> 8);
        rgba[i * 4 + 2] = static_cast<std::uint8_t>(c);
        rgba[i * 4 + 3] = static_cast<std::uint8_t>(c >> 24);
    }
}

Rect place(const Layer& layer, float win_w, float win_h) {
    Rect r;
    if (!layer.image || !layer.image->width || !layer.image->height) return r;
    const float iw = float(layer.image->width), ih = float(layer.image->height);
    if (layer.anchor == Anchor::Center) {
        float s = std::min(win_w * 0.9f / iw, win_h * 0.9f / ih);
        if (s >= 1.0f) s = float(int(s));
        r.w = iw * s;
        r.h = ih * s;
        r.x = float(int((win_w - r.w) * 0.5f));
        r.y = float(int((win_h - r.h) * 0.5f));
        return r;
    }
    const float scale = float(1 + int(win_h) / 1800);
    const float margin = 8.0f * scale;
    r.w = iw * scale;
    r.h = ih * scale;
    switch (layer.anchor) {
        case Anchor::TopLeft:
            r.x = margin;
            r.y = margin;
            break;
        case Anchor::TopRight:
            r.x = win_w - r.w - margin;
            r.y = margin;
            break;
        case Anchor::RightMiddle:
        default:
            r.x = win_w - r.w - margin;
            r.y = float(int((win_h - r.h) * 0.5f));
            break;
    }
    return r;
}

namespace raster {

void fill(Image& img, int x, int y, int w, int h, Argb c) {
    const int x0 = std::max(x, 0), y0 = std::max(y, 0);
    const int x1 = std::min<int>(x + w, int(img.width)), y1 = std::min<int>(y + h, int(img.height));
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) put(img, xx, yy, c);
}

void stroke(Image& img, int x, int y, int w, int h, Argb c) {
    fill(img, x, y, w, 1, c);
    fill(img, x, y + h - 1, w, 1, c);
    fill(img, x, y, 1, h, c);
    fill(img, x + w - 1, y, 1, h, c);
}

void text(Image& img, int x, int y, const std::string& s, Argb c, int scale) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(s[i]);
        if (ch < 32 || ch > 126) ch = '?';
        const std::uint8_t* g = kFont8x8[ch - 32];
        const int ox = x + int(i) * kGlyph * scale;
        for (int row = 0; row < kGlyph; ++row) {
            for (int col = 0; col < kGlyph; ++col) {
                if (!(g[row] & (1u << col))) continue;
                fill(img, ox + col * scale, y + row * scale, scale, scale, c);
            }
        }
    }
}

int text_width(const std::string& s, int scale) { return int(s.size()) * kGlyph * scale; }

void blit(Image& img, int x, int y, int w, int h, const std::uint8_t* rgba, std::uint32_t src_w,
          std::uint32_t src_h) {
    if (!rgba || !src_w || !src_h || w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; ++yy) {
        const std::uint32_t sy = std::uint32_t(yy) * src_h / std::uint32_t(h);
        for (int xx = 0; xx < w; ++xx) {
            const std::uint32_t sx = std::uint32_t(xx) * src_w / std::uint32_t(w);
            const std::uint8_t* p = rgba + (std::size_t(sy) * src_w + sx) * 4;
            put(img, x + xx, y + yy,
                0xFF000000u | Argb(p[0]) << 16 | Argb(p[1]) << 8 | Argb(p[2]));
        }
    }
}

} // namespace raster
} // namespace retro::overlay
