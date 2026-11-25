// Navigateur de médias de la visionneuse : sons numérisés, sprites des lieux, personnages, objets, documents
// (archives Atari extraites, pack/atari). Les pages 1 à 3 (lieux, musiques, bruitages) sont ailleurs.
#pragma once

#include <SDL.h>

#include <filesystem>
#include <string>
#include <vector>

#include "jukebox.hpp"
#include "pack.hpp"
#include "screen.hpp"

namespace maupiti {

class MediaBrowser {
public:
    MediaBrowser(const Pack& pack, const std::filesystem::path& atari_dir, AtariMusic& music);
    // pages : 3 sons numérisés, 4 sprites, 5 personnages, 6 objets, 7 documents
    static bool handles(int page) { return page >= 3 && page <= 7; }
    void draw(Screen& scr, const Font& f, int page) const;
    bool key(SDL_Keycode k, int page);                      // vrai si l'affichage change
    bool click(int x, int y, int page);
    bool tick(int page);

private:
    struct Group {
        std::string name;
        std::vector<Image> images;
        std::vector<std::string> labels;
        int room = -1;                                      // palette propre au groupe (sprites d'un lieu)
    };
    struct ImagePage {
        std::vector<Group> groups;
        int group = 0, item = 0;
        int pal_room = 0, pal_var = 0;                      // palette choisie : lieu, A/B/C
        bool own_palette = true;                            // palette intégrée à l'image si elle en a une
        bool on_room = false;                               // sprites : affichés sur le décor
    };
    struct Layout {
        int x, y;
    };

    std::vector<Image> load_entry(int arc, int entry) const;
    Palette palette_for(const ImagePage& p, const Group& g, const Image& im, std::string* label) const;
    std::vector<Layout> layout(const Group& g) const;
    ImagePage* page_data(int page);
    const ImagePage* page_data(int page) const;

    const Pack& pack_;
    std::filesystem::path dir_;
    AtariMusic& music_;
    ImagePage sprites_, characters_, objects_, documents_;
    // sons numérisés
    std::vector<int> samples_;
    int sample_sel_ = 0, sample_top_ = 0, sample_rate_ = 9600, sample_playing_ = -1;
    int last_tick_ = -1;
};

}  // namespace maupiti
