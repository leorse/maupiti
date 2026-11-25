// Barre d'outils de la visionneuse (choix du texte, de la voix et du style, lecture).
#pragma once

#include <SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "pack.hpp"
#include "screen.hpp"

namespace maupiti {

// Texte UTF-8 -> indices de glyphes de la police du jeu (caractères absents ignorés).
std::vector<uint8_t> glyphs_from_utf8(const std::string& s);
// Largeur en pixels d'une chaîne de glyphes.
int text_width(const Font& f, const std::vector<uint8_t>& g);

// Une variante de voix générée par tools/tts/variants.py.
struct VoiceVariant {
    std::string dir;     // sous-dossier de pack/voices
    std::string voice;   // libellé de la voix
    std::string style;   // libellé du style d'intonation
};

std::vector<VoiceVariant> load_voice_variants(const std::filesystem::path& pack_dir);

enum class Action { None, TextPrev, TextNext, VoicePrev, VoiceNext, StylePrev, StyleNext, Play };

class Toolbar {
public:
    // Dessine la barre en haut de l'écran et mémorise la position des boutons.
    void draw(Screen& scr, const Font& f, int text_id, const std::string& voice, const std::string& style);
    Action hit(int x, int y) const;   // coordonnées écran 320x200

private:
    struct Button {
        SDL_Rect r;
        Action a;
    };
    std::vector<Button> buttons_;
};

}  // namespace maupiti
