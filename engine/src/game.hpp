// Partie en cours : la logique d'origine (src/logic) pilote l'écran par les services du moteur, réécrits ici
// avec les graphismes Atari (docs/architecture.md §14, §15).
#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "logic/host.hpp"
#include "pack.hpp"
#include "screen.hpp"

namespace maupiti {

class AtariMusic;

class Game {
public:
    Game(const Pack& pack, const std::filesystem::path& pack_dir, AtariMusic* music);
    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }

    bool start();                                   // démarre la logique (jusqu'à sa boucle principale)
    void update(uint32_t elapsed_ms);               // horloge (18,2 Hz) et une tranche d'exécution
    void click(int x, int y, int button = 1);
    void render(Screen& scr) const;                 // écran de travail avec la palette courante
    bool changed() { const bool c = changed_; changed_ = false; return c; }
    // état du jeu (DS:0x166, mots little-endian) : lieu, heure (depuis le 30.01 à 12 h), tranches de 10 min, forme
    std::array<int, 4> state() {
        std::array<int, 4> s{};
        for (int i = 0; i < 4; ++i) s[i] = host_.cpu().rw(host_.zone() + 2 * i);
        return s;
    }
    // voix : réplique k (texte 1138 + k) avec les paramètres du service 30 (hauteur…)
    std::function<void(int, const std::vector<uint16_t>&)> on_voice;
    std::function<bool()> voice_busy;               // vrai tant que la réplique est jouée
    // sauvegarde (services 35 et 11) : fichier ; load_save = reprendre la partie sauvegardée au démarrage
    std::filesystem::path save_file = "sauvegardes/partie.mau";
    bool load_save = true;
    // trace des services non encore écrits (diagnostic)
    std::map<int, int> unknown_services;
    logic::Host& host() { return host_; }           // diagnostic
    bool blocked() const { return markers_moving_ || speaking_; }
    int time_scale = 1;                             // tests : accélération de l'horloge du jeu

private:
    uint32_t service(int n, const std::vector<uint16_t>& w);
    void fill(int x1, int y1, int x2, int y2, uint8_t c);
    void line(int x0, int y0, int x1, int y1, uint8_t c);
    void plot(int x, int y, uint8_t c) {
        if (x >= 0 && y >= 0 && x < kScreenW && y < kScreenH) {
            work_[static_cast<size_t>(y) * kScreenW + x] = c;
            dirty_ = {std::min(dirty_[0], x), std::min(dirty_[1], y), std::max(dirty_[2], x), std::max(dirty_[3], y)};
        }
    }
    std::array<int, 4> dirty_{};
    // couleur passée par la logique : n° de couleur de maup.exe, dans son ordre VGA (4 bits inversés,
    // table 0A42:0EAF) -> n° de couleur Atari de nos images et palettes
    static uint8_t vga_color(int c) {
        static constexpr uint8_t kRev[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
        return kRev[c & 15];
    }                    // rectangle modifié (x1, y1, x2, y2)
    void draw_image(const Image& im, int dx, int dy);
    void text(int id, int x, int y, int width, bool centered, uint8_t color, int height);
    // glyphes donnés (texte n° id, ou nombre) ; box > 0 : centré verticalement dans une hauteur de box points
    void glyph_text(const std::vector<uint8_t>& glyphs, int x, int y, int width, bool centered, uint8_t color,
                    int height, int box = 0);
    std::array<int, 4> last_fill_{-1, -1, -1, -1};
    std::array<int, 4> last_panel_{-1, -1, -1, -1};  // dernier panneau (service 10) : x1, y1, x2, y2  // dernier rectangle effacé (service 13) : x1, y1, x2, y2
    uint32_t ptr(const std::vector<uint16_t>& w, size_t i) const {
        return i + 1 < w.size() ? (((static_cast<uint32_t>(w[i + 1]) << 16) | w[i]) & 0xFFFFF) : 0;
    }
    // blocs chargés en mémoire par la logique : adresse -> (ressource, sous-bloc, taille)
    struct Loaded {
        int id, sub;
        uint32_t size;
    };
    const Loaded* loaded_at(uint32_t addr, uint32_t* base) const;

    const Pack& pack_;
    logic::Host host_;
    AtariMusic* music_;
    std::filesystem::path atari_dir_;
    bool ok_ = false;
    std::string error_;
    std::array<uint8_t, kScreenW * kScreenH> work_{};
    // écran affiché : maup.exe dessine dans l'écran de travail et n'en recopie que des rectangles (service 18,
    // ou services 8, 13, 14, 36, 39 appelés sans tampon) ; les états intermédiaires ne se voient pas
    std::array<uint8_t, kScreenW * kScreenH> screen_{};
    void flip(int x1, int y1, int x2, int y2);
    std::array<uint8_t, kScreenW * 64> skin_{};     // planche des tuiles de menus (64 premières lignes, service 23)
    uint32_t ticks_ = 0;                            // compteur d'interruptions (choix des tuiles de bord)
    int text_width(int id, int height) const;
    void menu_bar(int x, int y, int width, int style, int tile);
    Palette pal_{};
    std::array<Palette, 3> pal_set_{};              // palettes A, B, C de l'écran courant (choisies selon l'heure)
    bool pal_set_ok_ = false;
    int hour_variant(int half_hours) const;         // 0 = A (8-18 h), 1 = B (6-8 h, 18-20 h), 2 = C (nuit)
    std::vector<Image> atari_entry(int arc, int entry) const;
    // image Atari équivalente à une image PC chargée par la logique : ressource PC n -> archive Atari map[n]
    // (map.bin), même sous-bloc, même rang dans le bloc ; nullptr si aucune (ou géométrie trop différente)
    const Image* atari_equivalent(uint32_t desc, const Image& pc);
    std::vector<uint8_t> atari_map_;
    std::map<std::pair<int, int>, std::vector<Image>> atari_cache_;
    // marqueurs de la carte (service 17) : formes 7 et 8 du curseur, dessinées à l'écran par-dessus l'image
    struct Marker {
        bool shown = false, pending = false;
        double x = 0, y = 0;
        int tx = 0, ty = 0;
    };
    std::array<Marker, 2> markers_{};
    bool markers_moving_ = false;
    bool changed_ = true;
    double tick_acc_ = 0;
    std::map<uint32_t, Loaded> loaded_;
    std::map<int, int> room_of_resource_;           // ressource décor -> lieu du paquet (lieux.txt)
    int current_room_ = -1;
    int current_room_res_ = -1;                     // ressource PC du décor courant (service 24)
    // musique (services 45, 47, 48) : comme le moteur Atari ($BA48, $B88C)
    int music_context_ = -1;                        // [0x66B] : ressource du lieu, 0x1F carte, 0x20 interrogatoire
    bool cabin_started_ = false, cabin_jingle_ = false;
    int cabin_toggle_ = 0, map_cycle_ = 0, ff_cycle_ = 0, jingle_cycle_ = 0;
    void context_music();
    // service 46 (heure, à l'entrée dans un lieu) = Atari $BB12 : séquence de la banque du lieu selon l'heure,
    // bruitages des heures marquantes, bruitages d'ambiance au hasard
    void hour_sound(int half_hours);
    int room_code_ = -1;                            // banque de sons du lieu (table $BAF0, après -0x14), -1 : aucune
    std::array<bool, 5> special_done_{};            // heures marquantes déjà sonnées ($10E97)
    bool sample83_done_ = false;                    // $10E96
    int random_fx_ = -1;                            // $BC5C
    bool search_sound_ = false;                     // bruitage de fouille en cours (entre les deux effacements du cadran)
    bool jingle_on_ = false;                        // jingle (lettre…) en cours : coupé au retour au décor
    void stop_effects();                            // Atari $17E : arrête les bruitages, garde l'ambiance
    uint32_t rng_ = 12345;
    bool speaking_ = false;
    uint64_t clock_ms_ = 0, speak_start_ = 0, speak_min_ = 1500;
    int last_res_ = -1;                             // dernière ressource chargée
    uint32_t pal_mem_ = 0;                          // palette courante de la logique (adresse, service 42)
    uint32_t text_table_ = 0;                       // adresse de la table des longueurs des textes
    int text_base(uint32_t entry) const {           // n° du premier texte du groupe désigné par une entrée
        return text_table_ && entry >= text_table_ ? static_cast<int>((entry - text_table_) / 2) : 0;
    }
public:
    int interlocutor_ = -1;                         // personnage interrogé (service 26)
private:
};

}  // namespace maupiti
