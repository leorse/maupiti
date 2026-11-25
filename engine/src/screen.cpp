#include "screen.hpp"

#include <cstdlib>

#include <algorithm>
#include <fstream>

namespace maupiti {

uint32_t st_to_argb(uint16_t st) {
    auto c = [](int v) { return static_cast<uint32_t>(v * 255 / 7); };
    return 0xFF000000u | (c((st >> 8) & 7) << 16) | (c((st >> 4) & 7) << 8) | c(st & 7);
}

void Screen::clear(uint32_t argb) { std::fill(pixels_.begin(), pixels_.end(), argb); }

void Screen::fill(int x, int y, int w, int h, uint32_t argb) {
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) set(x + i, y + j, argb);
}

void Screen::draw(const Image& img, const Palette& pal, int dx, int dy) {
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            const uint8_t v = img.pixels[static_cast<size_t>(y) * img.width + x];
            if (v == img.transparent || v > 15) continue;
            set(img.x + dx + x, img.y + dy + y, st_to_argb(pal[v]));
        }
}

int Screen::text(const Font& f, std::span<const uint8_t> glyphs, int x, int y, uint32_t argb, int max_width) {
    const int x0 = x;
    for (uint8_t g : glyphs) {
        if (g == 0x65) {   // saut de ligne
            x = x0;
            y += f.height + 1;
            continue;
        }
        if (g == 0 || g > f.glyphs.size()) continue;
        const Glyph& gl = f.glyphs[g - 1];
        if (x + gl.advance - x0 > max_width) {
            x = x0;
            y += f.height + 1;
        }
        for (int j = 0; j < f.height; ++j)
            for (int i = 0; i < gl.w; ++i)
                if (f.bitmap[static_cast<size_t>(j) * f.atlas_width + gl.x + i]) set(x + i, y + j, argb);
        x += gl.advance;
    }
    return x - x0;
}

void Screen::save_bmp(const std::filesystem::path& p) const {
    std::ofstream f(p, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t row = kScreenW * 3, size = row * kScreenH;
    f.write("BM", 2);
    u32(54 + size); u32(0); u32(54);
    u32(40); u32(kScreenW); u32(kScreenH); u16(1); u16(24); u32(0); u32(size); u32(2835); u32(2835); u32(0); u32(0);
    for (int y = kScreenH - 1; y >= 0; --y)
        for (int x = 0; x < kScreenW; ++x) {
            const uint32_t c = pixels_[y * kScreenW + x];
            const char bgr[3] = {static_cast<char>(c & 0xFF), static_cast<char>((c >> 8) & 0xFF),
                                 static_cast<char>((c >> 16) & 0xFF)};
            f.write(bgr, 3);
        }
}

void Screen::line(int x0, int y0, int x1, int y1, uint32_t argb) {
    const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        set(x0, y0, argb);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

int Screen::text_box(const Font& f, std::span<const uint8_t> glyphs, int x, int y, int w, bool centered,
                     uint32_t argb) {
    constexpr uint8_t kSpace = 0x4C, kNewline = 0x65;
    auto adv = [&](uint8_t g) { return (g > 0 && g <= f.glyphs.size()) ? f.glyphs[g - 1].advance : 0; };
    std::vector<std::vector<uint8_t>> lines(1);
    int cur = 0;
    size_t i = 0;
    while (i < glyphs.size()) {
        if (glyphs[i] == kNewline) { lines.emplace_back(); cur = 0; ++i; continue; }
        size_t j = i;                                    // mot suivant (espace compris)
        int ww = 0;
        while (j < glyphs.size() && glyphs[j] != kSpace && glyphs[j] != kNewline) ww += adv(glyphs[j++]);
        if (cur > 0 && cur + ww > w) {                   // ne tient pas : nouvelle ligne
            while (!lines.back().empty() && lines.back().back() == kSpace) lines.back().pop_back();
            lines.emplace_back();
            cur = 0;
        }
        for (size_t k = i; k < j; ++k) lines.back().push_back(glyphs[k]);
        cur += ww;
        if (j < glyphs.size() && glyphs[j] == kSpace) {
            if (cur > 0) { lines.back().push_back(kSpace); cur += adv(kSpace); }
            ++j;
        }
        i = j;
    }
    int ly = y;
    for (auto& l : lines) {
        while (!l.empty() && l.back() == kSpace) l.pop_back();
        int lw = 0;
        for (uint8_t g : l) lw += adv(g);
        text(f, l, centered ? x + (w - lw) / 2 : x, ly, argb, kScreenW);
        ly += f.height;
    }
    return static_cast<int>(lines.size());
}

}  // namespace maupiti
