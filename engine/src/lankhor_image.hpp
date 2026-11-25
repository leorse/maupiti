// Décodeur d'images Lankhor (Mortevielle / Maupiti Island).
// Port C++ de tools/maupiti/gfx.py, lui-même porté de ScummVM
// (engines/mortevielle/graphics.cpp) — GPL v3 ou ultérieure.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace maupiti {

struct Image {
    int width = 0;
    int height = 0;
    int x = 0;                     // position d'origine à l'écran
    int y = 0;
    int transparent = 16;          // 16 = aucune couleur transparente
    std::optional<std::array<uint16_t, 16>> palette;   // mots ST 0x0RGB si présente
    std::vector<uint8_t> pixels;   // indices 0..15 (ou 16), width*height
};

// Décode une image ; lève std::runtime_error si les données sont incohérentes.
Image decode_image(std::span<const uint8_t> data);

// Nombre d'octets occupés par une image (pour enchaîner les images d'une banque).
size_t image_size(std::span<const uint8_t> data);

// Décode une suite d'images consécutives (banque de sprites).
std::vector<Image> decode_image_run(std::span<const uint8_t> data);

}  // namespace maupiti
