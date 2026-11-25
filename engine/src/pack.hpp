// Lecture du paquet de données produit par tools/build_pack.py.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "lankhor_image.hpp"

namespace maupiti {

using Palette = std::array<uint16_t, 16>;   // mots ST 0x0RGB

struct Room {
    int id = 0;
    int atari_archive = 0;
    std::string pc_node;                     // lieu PC correspondant ("-" si aucun)
    Image image;                             // décor ; sa palette intégrée = palette C
    Palette pal_a{}, pal_b{};                // palettes A (vérifiée sur capture Atari) et B
    std::vector<Image> sprites;
    std::vector<Image> patches;              // zones réservées Atari remplies avec le décor PC (rooms/NN_patch.bin)
    // animations du lieu (entrée 3 de l'archive Atari, pack/atari) : pour chacune, ses images, déjà placées
    std::vector<std::vector<Image>> anims;
};

struct Glyph {
    int x = 0, w = 0, advance = 0;
};

struct Font {
    int height = 0;
    int atlas_width = 0;
    std::vector<Glyph> glyphs;               // glyphe n° i+1
    std::vector<uint8_t> bitmap;             // atlas_width*height, 0/1
};

class Pack {
public:
    explicit Pack(const std::filesystem::path& dir);

    const std::vector<Room>& rooms() const { return rooms_; }
    const Font& font8() const { return font8_; }
    const Font& font7() const { return font7_; }
    const std::vector<std::vector<uint8_t>>& texts() const { return texts_; }   // indices de glyphes
    const std::vector<Image>& frame() const { return frame_; }      // morceaux du cadre
    const Image& menubar() const { return menubar_; }
    const Image& title() const { return title_; }
    // lieu du paquet pour un n° de ressource décor de la logique (lieu logique - 1), -1 si inconnu
    int room_for_resource(int res) const;
    const std::vector<uint8_t>& start_state() const { return start_; }   // SAUP.MAU : nouvelle partie

private:
    std::vector<Room> rooms_;
    Font font8_, font7_;
    std::vector<std::vector<uint8_t>> texts_;
    std::vector<Image> frame_;
    Image menubar_, title_;
    std::vector<std::pair<int, int>> lieux_;   // (ressource, lieu du paquet)
    std::vector<uint8_t> start_;
};

std::vector<uint8_t> read_file(const std::filesystem::path& p);

}  // namespace maupiti
