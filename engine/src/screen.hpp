// Écran logique 320x200 en couleurs 32 bits.
// Chaque élément est dessiné avec sa propre palette (pas de contrainte matérielle).
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "pack.hpp"

namespace maupiti {

constexpr int kScreenW = 320;
constexpr int kScreenH = 200;

uint32_t st_to_argb(uint16_t st);   // 0x0RGB (3 bits/composante) -> 0xAARRGGBB

class Screen {
public:
    Screen() : pixels_(kScreenW * kScreenH, 0xFF000000u) {}

    void clear(uint32_t argb = 0xFF000000u);
    // Dessine une image à sa position d'origine (+ décalage) avec la palette donnée.
    void draw(const Image& img, const Palette& pal, int dx = 0, int dy = 0);
    // Écrit une chaîne d'indices de glyphes ; renvoie la largeur dessinée.
    int text(const Font& f, std::span<const uint8_t> glyphs, int x, int y, uint32_t argb, int max_width = kScreenW);
    void fill(int x, int y, int w, int h, uint32_t argb);
    void line(int x0, int y0, int x1, int y1, uint32_t argb);
    // Texte avec retour à la ligne dans une largeur donnée (comme le service 9 de maup.exe) :
    // centré ou non, interligne = hauteur de la police. Renvoie le nombre de lignes.
    int text_box(const Font& f, std::span<const uint8_t> glyphs, int x, int y, int w, bool centered, uint32_t argb);

    const std::vector<uint32_t>& pixels() const { return pixels_; }
    void save_bmp(const std::filesystem::path& p) const;

private:
    std::vector<uint32_t> pixels_;
    void set(int x, int y, uint32_t c) {
        if (x >= 0 && y >= 0 && x < kScreenW && y < kScreenH) pixels_[y * kScreenW + x] = c;
    }
};

}  // namespace maupiti
