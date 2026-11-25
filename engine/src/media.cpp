#include "media.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "ui.hpp"

namespace maupiti {

namespace {
constexpr uint32_t kBg = 0xFF101828, kBar = 0xFF243048, kInk = 0xFFE0E0E0, kDim = 0xFF8890A0,
                   kSel = 0xFF3050A0, kHi = 0xFFFFD060, kBox = 0xFF2A3346;
constexpr int kAreaTop = 13, kAreaBottom = 166, kGap = 4;
constexpr int kListTop = 16, kRow = 9, kRows = 16;
const char* kCharacters[9] = {"Maguy", "Sue", "Anita", "Bruce", "Roy", "Chris", "Bob", "Anton", "Juste"};

bool has_palette(const Image& im) {
    return im.palette && std::any_of(im.palette->begin() + 1, im.palette->end(), [](uint16_t w) { return w != 0; });
}
}  // namespace

std::vector<Image> MediaBrowser::load_entry(int arc, int entry) const {
    char name[64];
    std::snprintf(name, sizeof name, "arc%02d/entry%03d.bin", arc, entry);
    std::ifstream f(dir_ / name, std::ios::binary);
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.empty()) return {};
    try {
        return decode_image_run(d);
    } catch (const std::exception&) {
        return {};
    }
}

MediaBrowser::MediaBrowser(const Pack& pack, const std::filesystem::path& atari_dir, AtariMusic& music)
    : pack_(pack), dir_(atari_dir), music_(music) {
    // palette par défaut des médias sans palette propre (portraits, objets) : ils prennent celle du lieu où ils
    // s'affichent ; celle du lieu de l'archive 24 donne des teintes de peau justes (à confirmer en jeu)
    int base = 0;
    for (size_t i = 0; i < pack.rooms().size(); ++i)
        if (pack.rooms()[i].atari_archive == 24) base = static_cast<int>(i);
    for (ImagePage* p : {&sprites_, &characters_, &objects_, &documents_}) p->pal_room = base;

    // sprites de chaque lieu (banque de l'archive du lieu), dans la palette du lieu
    for (size_t i = 0; i < pack.rooms().size(); ++i) {
        const Room& r = pack.rooms()[i];
        Group g{"Lieu " + std::to_string(i + 1) + " (archive " + std::to_string(r.atari_archive) + ")", r.sprites, {},
                static_cast<int>(i)};
        for (size_t k = 0; k < r.sprites.size(); ++k) g.labels.push_back("sprite " + std::to_string(k));
        for (size_t a = 0; a < r.anims.size(); ++a)               // animations du lieu (entrée 3)
            for (size_t k = 0; k < r.anims[a].size(); ++k) {
                g.images.push_back(r.anims[a][k]);
                g.labels.push_back("animation " + std::to_string(a + 1) + " image " + std::to_string(k + 1));
            }
        sprites_.groups.push_back(std::move(g));
    }
    // personnages : petit portrait (archive 14, entrées 1-9), grand portrait d'interrogatoire et bandes
    // d'animation (archive 15, 3 entrées par personnage ; images en plusieurs morceaux)
    for (int c = 0; c < 9; ++c) {
        Group g{kCharacters[c], {}, {}, -1};
        auto add = [&](int arc, int entry, const std::string& what) {
            for (auto& im : load_entry(arc, entry)) {
                g.images.push_back(std::move(im));
                g.labels.push_back(what + " (arc. " + std::to_string(arc) + "/" + std::to_string(entry) + ")");
            }
        };
        add(14, c + 1, "portrait");
        for (int k = 1; k <= 3; ++k) add(15, 3 * c + k, k == 1 ? "grand portrait" : "animation");
        characters_.groups.push_back(std::move(g));
    }
    // objets de l'inventaire (archive 22)
    {
        Group g{"Objets (archive 22)", {}, {}, -1};
        for (int e = 0; e < 128; ++e)
            for (auto& im : load_entry(22, e)) {
                g.images.push_back(std::move(im));
                g.labels.push_back("objet " + std::to_string(e));
            }
        objects_.groups.push_back(std::move(g));
    }
    // documents, scènes, carte, interface
    struct Src {
        const char* name;
        int arc, first, last;
    };
    for (const Src& s : {Src{"Documents (archive 21)", 21, 0, 7}, Src{"Scènes et éléments (archive 20)", 20, 0, 13},
                         Src{"Carte de l'île (archive 14)", 14, 0, 0}, Src{"Introduction (archive 15)", 15, 0, 0},
                         Src{"Interface (archive 10)", 10, 0, 8}}) {
        Group g{s.name, {}, {}, -1};
        for (int e = s.first; e <= s.last; ++e) {
            const auto ims = load_entry(s.arc, e);
            for (size_t k = 0; k < ims.size(); ++k) {
                g.images.push_back(ims[k]);
                g.labels.push_back("entrée " + std::to_string(e) + (ims.size() > 1 ? "." + std::to_string(k) : ""));
            }
        }
        if (!g.images.empty()) documents_.groups.push_back(std::move(g));
    }
    // sons numérisés présents (archive 61)
    for (int e = 0; e < 160; ++e) {
        char name[64];
        std::snprintf(name, sizeof name, "arc61/entry%03d.bin", e);
        if (std::filesystem::exists(dir_ / name)) samples_.push_back(e);
    }
}

MediaBrowser::ImagePage* MediaBrowser::page_data(int page) {
    switch (page) {
    case 4: return &sprites_;
    case 5: return &characters_;
    case 6: return &objects_;
    case 7: return &documents_;
    default: return nullptr;
    }
}
const MediaBrowser::ImagePage* MediaBrowser::page_data(int page) const {
    return const_cast<MediaBrowser*>(this)->page_data(page);
}

Palette MediaBrowser::palette_for(const ImagePage& p, const Group& g, const Image& im, std::string* label) const {
    if (p.own_palette && has_palette(im)) {
        if (label) *label = "palette de l'image";
        return *im.palette;
    }
    const auto& rooms = pack_.rooms();
    const int r = std::clamp(g.room >= 0 ? g.room : p.pal_room, 0, static_cast<int>(rooms.size()) - 1);
    if (label) *label = "palette du lieu " + std::to_string(r + 1) + " " + "ABC"[p.pal_var];
    const Room& room = rooms[r];
    if (p.pal_var == 1) return room.pal_b;
    if (p.pal_var == 2 && room.image.palette) return *room.image.palette;
    return room.pal_a;
}

std::vector<MediaBrowser::Layout> MediaBrowser::layout(const Group& g) const {
    // images à leur taille réelle, rangées en lignes de 312 pixels
    std::vector<Layout> out;
    int x = kGap, y = 0, row_h = 0;
    for (const Image& im : g.images) {
        if (x + im.width > kScreenW - kGap && x > kGap) {
            x = kGap;
            y += row_h + kGap;
            row_h = 0;
        }
        out.push_back({x, y});
        x += im.width + kGap;
        row_h = std::max(row_h, im.height);
    }
    return out;
}

void MediaBrowser::draw(Screen& scr, const Font& f, int page) const {
    scr.clear(kBg);
    if (page == 3) {                                        // sons numérisés : liste
        for (int r = 0; r < kRows; ++r) {
            const int i = sample_top_ + r;
            if (i >= static_cast<int>(samples_.size())) break;
            const int y = kListTop + r * kRow;
            if (i == sample_sel_) scr.fill(0, y - 1, kScreenW, kRow, kSel);
            const bool on = samples_[i] == sample_playing_ && music_.sample_playing();
            scr.text(f, glyphs_from_utf8(std::string(on ? "> " : "  ") + "Son " + std::to_string(samples_[i]) +
                                         " (archive 61)"), 4, y, on ? kHi : kInk);
        }
        draw_page_header(scr, f, page);
        scr.fill(0, 167, kScreenW, 33, kBar);
        scr.text(f, glyphs_from_utf8("Fréquence de lecture : " + std::to_string(sample_rate_) + " Hz (+/-)"), 4, 169, kHi);
        scr.text(f, glyphs_from_utf8("Haut/Bas choisir - Entrée jouer - Espace stop"), 4, 180, kDim);
        scr.text(f, glyphs_from_utf8("Hauteur réelle : fixée par la note du jeu"), 4, 190, kDim);
        return;
    }
    const ImagePage* p = page_data(page);
    if (!p || p->groups.empty()) {
        draw_page_header(scr, f, page);
        scr.text(f, glyphs_from_utf8("Aucune image (pack/atari incomplet ?)"), 4, 40, kHi);
        return;
    }
    const Group& g = p->groups[p->group];
    std::string pal_label;
    if (g.images.empty()) {
        scr.text(f, glyphs_from_utf8("Aucune image dans ce groupe"), 4, 40, kDim);
    } else if (p->on_room && g.room >= 0) {                 // sprite sur le décor, à sa position d'origine
        const Room& room = pack_.rooms()[g.room];
        const Palette pal = palette_for(*p, g, g.images[p->item], &pal_label);
        scr.draw(room.image, pal, 4 - room.image.x, kAreaTop - room.image.y);
        const Image& im = g.images[p->item];
        scr.draw(im, pal, 4, kAreaTop);
        const int x0 = im.x + 4, y0 = im.y + kAreaTop;
        scr.line(x0 - 1, y0 - 1, x0 + im.width, y0 - 1, kHi);
        scr.line(x0 - 1, y0 + im.height, x0 + im.width, y0 + im.height, kHi);
        scr.line(x0 - 1, y0 - 1, x0 - 1, y0 + im.height, kHi);
        scr.line(x0 + im.width, y0 - 1, x0 + im.width, y0 + im.height, kHi);
    } else {                                                // planche : toutes les images du groupe
        const auto pos = layout(g);
        const Image& sel = g.images[p->item];
        // défilement : la ligne de l'image choisie reste visible
        int scroll = 0;
        const int sel_bottom = pos[p->item].y + sel.height;
        if (sel_bottom > kAreaBottom - kAreaTop) scroll = sel_bottom - (kAreaBottom - kAreaTop);
        if (pos[p->item].y - scroll < 0) scroll = pos[p->item].y;
        for (size_t k = 0; k < g.images.size(); ++k) {
            const Image& im = g.images[k];
            const int x = pos[k].x, y = kAreaTop + pos[k].y - scroll;
            if (y > kAreaBottom || y + im.height < kAreaTop) continue;
            scr.fill(x, y, im.width, im.height, kBox);
            scr.draw(im, palette_for(*p, g, im, k == static_cast<size_t>(p->item) ? &pal_label : nullptr),
                     x - im.x, y - im.y);
            if (k == static_cast<size_t>(p->item)) {
                scr.line(x - 2, y - 2, x + im.width + 1, y - 2, kHi);
                scr.line(x - 2, y + im.height + 1, x + im.width + 1, y + im.height + 1, kHi);
                scr.line(x - 2, y - 2, x - 2, y + im.height + 1, kHi);
                scr.line(x + im.width + 1, y - 2, x + im.width + 1, y + im.height + 1, kHi);
            }
        }
    }
    draw_page_header(scr, f, page);
    scr.fill(0, 167, kScreenW, 33, kBar);
    std::string line1 = g.name + " (" + std::to_string(p->group + 1) + "/" + std::to_string(p->groups.size()) + ")";
    scr.text(f, glyphs_from_utf8(line1), 4, 169, kHi);
    if (!g.images.empty()) {
        const Image& im = g.images[p->item];
        const std::string line2 = g.labels[p->item] + " - " + std::to_string(p->item + 1) + "/" +
                                  std::to_string(g.images.size()) + " - " + std::to_string(im.width) + "x" +
                                  std::to_string(im.height) + " en " + std::to_string(im.x) + "," + std::to_string(im.y);
        scr.text(f, glyphs_from_utf8(line2), 4, 179, kInk);
    }
    std::string help = pal_label + " - P/O lieu, 1-3, I";
    if (page == 4) help += " - Entrée décor";
    scr.text(f, glyphs_from_utf8(help), 4, 189, kDim);
}

bool MediaBrowser::key(SDL_Keycode k, int page) {
    if (page == 3) {
        const int n = static_cast<int>(samples_.size());
        if (!n) return false;
        switch (k) {
        case SDLK_UP: sample_sel_ = (sample_sel_ + n - 1) % n; break;
        case SDLK_DOWN: sample_sel_ = (sample_sel_ + 1) % n; break;
        case SDLK_PAGEUP: sample_sel_ = std::max(0, sample_sel_ - kRows); break;
        case SDLK_PAGEDOWN: sample_sel_ = std::min(n - 1, sample_sel_ + kRows); break;
        case SDLK_RETURN: case SDLK_KP_ENTER:
            sample_playing_ = samples_[sample_sel_];
            music_.sample(sample_playing_, sample_rate_);
            break;
        case SDLK_SPACE: music_.stop(); sample_playing_ = -1; break;
        case SDLK_PLUS: case SDLK_KP_PLUS: case SDLK_EQUALS: sample_rate_ = std::min(24000, sample_rate_ + 400); break;
        case SDLK_MINUS: case SDLK_KP_MINUS: sample_rate_ = std::max(2000, sample_rate_ - 400); break;
        default: return false;
        }
        if (sample_sel_ < sample_top_) sample_top_ = sample_sel_;
        if (sample_sel_ >= sample_top_ + kRows) sample_top_ = sample_sel_ - kRows + 1;
        return true;
    }
    ImagePage* p = page_data(page);
    if (!p || p->groups.empty()) return false;
    const int ng = static_cast<int>(p->groups.size());
    auto items = [&]() { return static_cast<int>(p->groups[p->group].images.size()); };
    const int nr = static_cast<int>(pack_.rooms().size());
    switch (k) {
    case SDLK_LEFT: if (items()) p->item = (p->item + items() - 1) % items(); break;
    case SDLK_RIGHT: if (items()) p->item = (p->item + 1) % items(); break;
    case SDLK_UP: p->group = (p->group + ng - 1) % ng; p->item = 0; break;
    case SDLK_DOWN: p->group = (p->group + 1) % ng; p->item = 0; break;
    case SDLK_HOME: p->item = 0; break;
    case SDLK_END: if (items()) p->item = items() - 1; break;
    case SDLK_p: p->pal_room = (p->pal_room + 1) % nr; break;
    case SDLK_o: p->pal_room = (p->pal_room + nr - 1) % nr; break;
    case SDLK_1: p->pal_var = 0; break;
    case SDLK_2: p->pal_var = 1; break;
    case SDLK_3: p->pal_var = 2; break;
    case SDLK_i: p->own_palette = !p->own_palette; break;
    case SDLK_RETURN: case SDLK_KP_ENTER: if (page == 4) p->on_room = !p->on_room; break;
    default: return false;
    }
    return true;
}

bool MediaBrowser::click(int x, int y, int page) {
    if (page == 3) {
        const int r = (y - kListTop + 1) / kRow;
        if (y < kListTop - 1 || r < 0 || r >= kRows) return false;
        const int i = sample_top_ + r;
        if (i >= static_cast<int>(samples_.size())) return false;
        sample_sel_ = i;
        sample_playing_ = samples_[i];
        music_.sample(sample_playing_, sample_rate_);
        return true;
    }
    ImagePage* p = page_data(page);
    if (!p || p->groups.empty() || p->on_room) return false;
    const Group& g = p->groups[p->group];
    if (g.images.empty() || y < kAreaTop || y > kAreaBottom) return false;
    const auto pos = layout(g);
    int scroll = 0;                                         // même défilement que draw()
    const int sel_bottom = pos[p->item].y + g.images[p->item].height;
    if (sel_bottom > kAreaBottom - kAreaTop) scroll = sel_bottom - (kAreaBottom - kAreaTop);
    if (pos[p->item].y - scroll < 0) scroll = pos[p->item].y;
    for (size_t k = 0; k < g.images.size(); ++k) {
        const int ix = pos[k].x, iy = kAreaTop + pos[k].y - scroll;
        if (x >= ix && x < ix + g.images[k].width && y >= iy && y < iy + g.images[k].height) {
            p->item = static_cast<int>(k);
            return true;
        }
    }
    return false;
}

bool MediaBrowser::tick(int page) {
    if (page != 3) return false;
    const int state = music_.sample_playing() ? 1 : 0;      // fin de lecture : marque retirée
    if (state == last_tick_) return false;
    last_tick_ = state;
    return true;
}

}  // namespace maupiti
