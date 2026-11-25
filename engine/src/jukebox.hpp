// Musique Atari dans le moteur (sortie audio continue) et juke-box : écoute de toutes les musiques et de
// tous les bruitages, avec le pilote son d'origine (src/atari_sound.hpp).
#pragma once

#include <SDL.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pack.hpp"
#include "screen.hpp"

namespace maupiti {

class AtariSound;

// Rendu hors ligne vers un WAV (contrôle) : what = "titre", "N" (musique, banque N) ou "bN" (bruitage N).
int render_atari_wav(const std::filesystem::path& sound_dir, const std::string& what,
                     const std::filesystem::path& out, double seconds);

// Lecteur : le pilote Atari sur sa propre sortie audio (fil audio de SDL).
class AtariMusic {
public:
    AtariMusic(const std::filesystem::path& sound_dir, const Pack& pack);
    ~AtariMusic();
    AtariMusic(const AtariMusic&) = delete;
    AtariMusic& operator=(const AtariMusic&) = delete;

    bool ok() const { return dev_ != 0; }
    const std::string& error() const { return error_; }
    void title();
    void music(int bank);
    void effect(int bank, unsigned call = 0x148, int duration = -1);
    void room(int bank, int room_archive);       // décor : banque de sons et ambiance du lieu
    void call(unsigned vector, unsigned d0, unsigned poke_addr = 0, unsigned poke_value = 0);
    // octet de la bibliothèque du moteur Atari (archive 0, chargée en $B66C) ; -1 hors de l'archive
    int lib(unsigned addr) const {
        return addr >= 0xB66C && addr - 0xB66C < lib_.size() ? lib_[addr - 0xB66C] : -1;
    }
    void sample(int n, int rate);
    void ambience(int room_archive);
    // lieux qui ont une ambiance sonore (entrée 2 de leur archive) : (index du paquet, n° d'archive)
    const std::vector<std::pair<int, int>>& ambiences() const { return ambiences_; }
    bool sample_playing() const;
    void stop();
    bool busy(int channel) const;
    double seconds() const;
    // Banque musicale du lieu (index du paquet) d'après la table du jeu (archive 0, $BAF0), -1 si aucune.
    int bank_for_room(int room) const;
    // Table brute du moteur Atari ($BAF0), indexée par le n° de requête du décor (= ressource PC du lieu) :
    // banque musicale, ou $FF / $FE (cycles de musiques), $FD / $FC (rien) ; -1 hors table
    int music_code(int request) const;
    // Lieux (index du paquet) qui jouent la banque n.
    std::vector<int> rooms_for_bank(int bank) const;

private:
    static void callback(void* self, Uint8* stream, int len);
    std::unique_ptr<AtariSound> snd_;
    SDL_AudioDeviceID dev_ = 0;
    std::string error_;
    std::vector<int> room_bank_;        // index du paquet -> banque
    std::vector<uint8_t> music_table_;  // $BAF0 : n° de requête -> code
    std::vector<uint8_t> lib_;          // archive 0
    std::vector<std::pair<int, int>> ambiences_;
};

// En-tête commun aux pages de la visionneuse (F1 à F8) ; renvoie la hauteur occupée.
int draw_page_header(Screen& scr, const Font& f, int page);
constexpr int kPageCount = 8;

// Pages du juke-box dans la visionneuse : 1 = musiques, 2 = bruitages.
class Jukebox {
public:
    explicit Jukebox(AtariMusic& music);
    void draw(Screen& scr, const Font& f, int page) const;
    bool key(SDL_Keycode k, int page);              // renvoie vrai si l'affichage change
    bool click(int x, int y, int page);
    bool tick();                                    // vrai quand l'affichage du temps doit être rafraîchi

private:
    struct Entry {
        std::string label;
        int kind;                                   // 0 titre, 1 musique, 2 bruitage, 3 ambiance d'un lieu
        int bank;
    };
    const std::vector<Entry>& list(int page) const { return page == 2 ? effects_ : musics_; }
    void play(const Entry& e);

    AtariMusic& music_;
    std::vector<Entry> musics_, effects_;
    int sel_[3] = {0, 0, 0};
    int top_[3] = {0, 0, 0};
    std::string playing_;
    int playing_kind_ = -1;
    int last_second_ = -1;
};

}  // namespace maupiti
