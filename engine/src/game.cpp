#include "game.hpp"

#include <optional>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "jukebox.hpp"

namespace maupiti {

namespace {
constexpr uint32_t kDescriptor1 = 0x9FF00;     // descripteurs rendus par les services 1 et 6 (zone libre)
constexpr uint32_t kDescriptor6 = 0x9FF10;
constexpr double kTickMs = 1000.0 / 18.2065;   // horloge du PC (interruption 1Ch)
constexpr uint64_t kInstrPerMs = 4000;         // vitesse d'exécution de la logique
// couleur transparente de la planche des menus et du curseur : 2 pour maup.exe, dans son ordre VGA (bits
// inversés) ; c'est la couleur 4 dans l'ordre Atari de nos images
constexpr uint8_t kSkinClear = 4;
}  // namespace

Game::Game(const Pack& pack, const std::filesystem::path& pack_dir, AtariMusic* music)
    : pack_(pack), host_(pack_dir / "pc"), music_(music), atari_dir_(pack_dir / "atari") {
    {   // ressource PC n -> archive Atari (table du chargeur Atari, copiée par build_pack.py)
        std::ifstream f(atari_dir_ / "map.bin", std::ios::binary);
        atari_map_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (!host_.ok()) { error_ = host_.error(); return; }
    host_.on_service = [this](int n, const std::vector<uint16_t>& w) {
        if (const char* t = std::getenv("MAUPITI_SONS");                 // diagnostic : services du son (all : tous)
            t && ((n >= 43 && n <= 51) || (t[0] == 'a' && n != 3 && n != 4 && n != 5 && n != 22 && n != 37))) {
            std::fprintf(stderr, "son %d :", n);
            for (uint16_t x : w) std::fprintf(stderr, " %04X", x);
            std::fputc(10, stderr);
        }
        if (n != 2) return service(n, w);
        // menus : maup.exe les dessine aussi directement à l'écran (pas de recopie ensuite)
        dirty_ = {kScreenW, kScreenH, -1, -1};
        const uint32_t r = service(n, w);
        if (dirty_[2] >= 0) flip(dirty_[0], dirty_[1], dirty_[2], dirty_[3]);
        return r;
    };
    host_.on_load = [this](int id, int sub, uint32_t dest, uint32_t size) {
        // un nouveau bloc remplace ceux qu'il recouvre
        for (auto it = loaded_.begin(); it != loaded_.end();)
            it = (it->first < dest + size && dest < it->first + it->second.size) ? loaded_.erase(it) : std::next(it);
        loaded_[dest] = {id, sub, size};
        if (id == 0x28 && sub == 0) text_table_ = dest;   // table des longueurs des textes (base des n°)
        last_res_ = id;                                     // [0x65F] de maup.exe : dernière ressource chargée
    };
    host_.tick_per_read = false;
    ok_ = true;
}

bool Game::start() {
    if (!ok_) return false;
    if (!host_.boot()) { error_ = host_.error(); return false; }
    host_.run_slice(20'000'000);                    // jusqu'à l'écran de départ
    return true;
}

void Game::update(uint32_t elapsed_ms) {
    if (!ok_) return;
    tick_acc_ += static_cast<double>(elapsed_ms) * time_scale;
    while (tick_acc_ >= kTickMs) { tick_acc_ -= kTickMs; host_.tick(); ++ticks_; }
    // vitesse d'un PC de l'époque (environ 4 millions d'instructions par seconde) : la logique compte aussi
    // sur la durée de ses boucles
    clock_ms_ += elapsed_ms;
    if (markers_moving_) {                          // déplacement des marqueurs : la logique attend la fin
        // maup.exe avance chaque marqueur d'un point par pas (axe principal), environ 8 ms par pas
        const double step = elapsed_ms / 8.0;
        bool done = true;
        for (auto& m : markers_) {
            const double dx = m.tx - m.x, dy = m.ty - m.y;
            const double d = std::max(std::abs(dx), std::abs(dy));
            if (d <= step) { m.x = m.tx; m.y = m.ty; }
            else { m.x += dx * step / d; m.y += dy * step / d; done = false; }
        }
        changed_ = true;
        if (!done) return;
        markers_moving_ = false;
        for (auto& m : markers_) m.pending = false;
    }
    if (speaking_) {                                // réplique en cours : la logique attend (au moins 1 s)
        const bool busy = voice_busy ? voice_busy() : false;
        if (busy || clock_ms_ - speak_start_ < speak_min_) return;
        speaking_ = false;
    }
    host_.run_slice(static_cast<uint64_t>(std::min<uint32_t>(elapsed_ms, 100)) * kInstrPerMs);
}

void Game::click(int x, int y, int button) { host_.click(x, y, button); }

const Game::Loaded* Game::loaded_at(uint32_t addr, uint32_t* base) const {
    auto it = loaded_.upper_bound(addr);
    if (it == loaded_.begin()) return nullptr;
    --it;
    if (addr >= it->first + it->second.size) return nullptr;
    if (base) *base = it->first;
    return &it->second;
}

void Game::render(Screen& scr) const {
    auto& px = const_cast<std::vector<uint32_t>&>(scr.pixels());
    for (size_t i = 0; i < screen_.size(); ++i) px[i] = st_to_argb(pal_[screen_[i] & 15]);
    for (size_t k = 0; k < markers_.size(); ++k) {  // formes 7 et 8 de la planche (128,36) et (176,36), 16x12
        const Marker& m = markers_[k];
        if (!m.shown) continue;
        const int sx = k == 0 ? 128 : 176, sy = 36, x0 = static_cast<int>(m.x), y0 = static_cast<int>(m.y);
        // couleur transparente : 2 pour maup.exe, mais la planche PC n'en a pas (fond 4) : celle du coin
        const uint8_t clear = skin_[static_cast<size_t>(sy) * kScreenW + sx];
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 16; ++x) {
                const uint8_t v = skin_[static_cast<size_t>(sy + y) * kScreenW + sx + x];
                const int X = x0 + x, Y = y0 + y;
                if (v == kSkinClear || v == clear || X < 0 || Y < 0 || X >= kScreenW || Y >= kScreenH) continue;
                px[static_cast<size_t>(Y) * kScreenW + X] = st_to_argb(pal_[v & 15]);
            }
    }
}

void Game::context_music() {
    // moteur Atari $BA48 : code de la table $BAF0 pour le contexte ; la cabine (0x19) joue sa banque la
    // première fois, puis alterne les banques 0x1E / 0x21 ; $FE : banques 0x22-0x26 à tour de rôle ;
    // $FF : banques 0x27, 0x1F, 0x28, 0x29 ; $FD, $FC : la musique en cours continue
    cabin_jingle_ = false;
    room_code_ = -1;
    jingle_on_ = search_sound_ = false;
    if (!music_ || music_context_ < 0) return;
    if (music_context_ == 0x19 && cabin_started_) {
        static constexpr int kCabin[2] = {0x21, 0x1E};
        cabin_toggle_ ^= 1;
        cabin_jingle_ = true;
        music_->effect(kCabin[cabin_toggle_], 0x148, 0x44);
        return;
    }
    if (music_context_ == 0x19) cabin_started_ = true;
    const int code = music_->music_code(music_context_);
    if (code < 0) return;
    if (code == 0xFE) {
        map_cycle_ = map_cycle_ >= 4 ? 0 : map_cycle_ + 1;
        music_->effect(0x22 + map_cycle_);
    } else if (code == 0xFF) {
        static constexpr int kCycle[4] = {0x27, 0x1F, 0x28, 0x29};
        ff_cycle_ = ff_cycle_ >= 3 ? 0 : ff_cycle_ + 1;
        music_->effect(kCycle[ff_cycle_]);
    } else if (code < 0x80) {
        // pas de morceau : la banque n'est qu'un jeu de sons pour l'ambiance du lieu
        const int arc = music_context_ < 30 && music_context_ < static_cast<int>(atari_map_.size())
                            ? atari_map_[music_context_] : -1;
        room_code_ = code >= 0x14 ? code - 0x14 : code;
        music_->room(room_code_, arc);
    }
}

const Image* Game::atari_equivalent(uint32_t desc, const Image& pc) {
    if (std::getenv("MAUPITI_PC")) return nullptr;     // diagnostic : graphismes PC
    uint32_t base = 0;
    const Loaded* l = loaded_at(desc, &base);
    if (!l || l->id < 0 || l->id >= static_cast<int>(atari_map_.size())) return nullptr;
    const int arc = atari_map_[l->id];
    auto key = std::make_pair(arc, l->sub);
    auto it = atari_cache_.find(key);
    if (it == atari_cache_.end()) it = atari_cache_.emplace(key, atari_entry(arc, l->sub)).first;
    const auto& list = it->second;
    // rang de l'image dans le bloc PC
    auto& cpu = host_.cpu();
    size_t index = 0;
    for (uint32_t a = base; a < desc; ++index) {
        if (index >= list.size()) return nullptr;
        std::vector<uint8_t> s(cpu.mem.begin() + a, cpu.mem.begin() + std::min<uint32_t>(base + l->size, x86::Cpu::kMemSize));
        size_t sz = 0;
        try { sz = image_size(s); } catch (const std::exception&) {}
        if (sz == 0) return nullptr;
        a += static_cast<uint32_t>(sz);
        if (a > desc) return nullptr;
    }
    if (index >= list.size()) return nullptr;
    const Image& at = list[index];
    // même image ? (positions et tailles voisines ; les versions diffèrent de quelques points)
    if (std::abs(at.x - pc.x) > 12 || std::abs(at.y - pc.y) > 12 || std::abs(at.width - pc.width) > 16 ||
        std::abs(at.height - pc.height) > 16)
        return nullptr;
    return &at;
}

void Game::stop_effects() {
    jingle_on_ = false;
    if (music_) music_->call(0x17E, 0);
}

void Game::hour_sound(int h) {
    // Atari $BB12 (h = demi-heures depuis le 30.01 à 12 h, comme le service 46 de la version PC)
    if (!music_ || music_->lib(0xBB8E) < 0) return;
    const int ctx = music_context_;
    const int code = music_->music_code(ctx);
    if (code < 0 || code >= 0x80) return;           // carte, interrogatoire… ($BC84)
    // séquence de la banque du lieu : table $BB8E[code] -> liste $BB9C de couples (seuil, séquence)
    if (room_code_ >= 0 && room_code_ < 14) {
        const int off = music_->lib(0xBB8E + room_code_);
        const unsigned list = (static_cast<unsigned>(music_->lib(0xBB9C + off)) << 24 |
                               music_->lib(0xBB9D + off) << 16 | music_->lib(0xBB9E + off) << 8 |
                               music_->lib(0xBB9F + off));
        const int hh = h >= 0x30 ? h - 0x30 : h;
        unsigned a = list;
        for (int guard = 0; guard < 32 && music_->lib(a) >= 0 && hh >= music_->lib(a); ++guard) a += 2;
        const int seq = music_->lib(a + 1);
        if (seq >= 0 && seq < 0x80) music_->call(0x164, static_cast<unsigned>(seq));
    }
    // heures marquantes ($BBFC : 5 couples heure, bruitage - 14), une seule fois : bruitage dans les lieux 0-6, 12
    bool special = false;
    for (int k = 0; k < 5; ++k)
        if (music_->lib(0xBBFC + 2 * k) == h) {
            special = true;
            if (!special_done_[k] && (ctx <= 6 || ctx == 12) && ctx >= 0) {
                special_done_[k] = true;
                const int level = music_->lib(0xB99E + ctx);
                music_->call(0x148, static_cast<unsigned>(music_->lib(0xBBFD + 2 * k) + 14), 0x17A,
                             static_cast<unsigned>(level & 0xFF));
            }
        }
    // sinon, une fois sur 8 : bruitage d'ambiance suivant de la liste $BC5E
    if (!special) {
        rng_ = rng_ * 1103515245u + 12345u;
        if ((rng_ >> 16) % 8 == 0) {
            random_fx_ = random_fx_ >= 5 ? 0 : random_fx_ + 1;
            music_->call(0x144, static_cast<unsigned>(music_->lib(0xBC5E + random_fx_)));
        }
    }
    // échantillon 0x83 ($B906 : volume de la table $B92A du lieu), une seule fois, à l'heure 0x45
    if (h == 0x45 && !sample83_done_ && ctx >= 0 && ctx < 34) {
        sample83_done_ = true;
        if (const int v = music_->lib(0xB92A + ctx); v > 0)
            music_->call(0x14C, 0, 0x17A, static_cast<unsigned>(v & 0xFF));
    }
}

int Game::hour_variant(int half_hours) const {
    const int t = (half_hours >> 1) % 24;
    return (t >= 8 && t < 18) ? 0 : ((t >= 6 && t < 8) || (t >= 18 && t < 20)) ? 1 : 2;
}

std::vector<Image> Game::atari_entry(int arc, int entry) const {
    char name[64];
    std::snprintf(name, sizeof name, "arc%02d/entry%03d.bin", arc, entry);
    std::ifstream f(atari_dir_ / name, std::ios::binary);
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    try { return d.empty() ? std::vector<Image>{} : decode_image_run(d); } catch (const std::exception&) { return {}; }
}

void Game::flip(int x1, int y1, int x2, int y2) {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(x1, 0); y1 = std::max(y1, 0);
    x2 = std::min(x2, kScreenW - 1); y2 = std::min(y2, kScreenH - 1);
    for (int y = y1; y <= y2; ++y)
        for (int x = x1; x <= x2; ++x) {
            const size_t o = static_cast<size_t>(y) * kScreenW + x;
            screen_[o] = work_[o];
        }
    changed_ = true;
}

void Game::fill(int x1, int y1, int x2, int y2, uint8_t c) {
    for (int y = y1; y <= y2; ++y)
        for (int x = x1; x <= x2; ++x) plot(x, y, c);
    changed_ = true;
}

void Game::line(int x0, int y0, int x1, int y1, uint8_t c) {
    const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    changed_ = true;
}

void Game::draw_image(const Image& im, int dx, int dy) {
    for (int y = 0; y < im.height; ++y)
        for (int x = 0; x < im.width; ++x) {
            const uint8_t v = im.pixels[static_cast<size_t>(y) * im.width + x];
            if (v == im.transparent || v > 15) continue;
            plot(im.x + dx + x, im.y + dy + y, v);
        }
    changed_ = true;
}

int Game::text_width(int id, int height) const {
    if (id < 0 || id >= static_cast<int>(pack_.texts().size())) return 0;
    const Font& f = height <= 7 ? pack_.font7() : pack_.font8();
    int wsum = 0;
    for (uint8_t g : pack_.texts()[id])
        if (g > 0 && g <= f.glyphs.size()) wsum += f.glyphs[g - 1].advance;
    return wsum;
}

// Barre d'une ligne de menu (maup.exe 0159:015B) : bord gauche (tuile 16x12 n° `tile` de la planche, ligne 0
// ou 24 selon le style), milieux (tuile en x = 128), bord droit (ligne 12 ou 36) ; couleur transparente : kSkinClear.
void Game::menu_bar(int x, int y, int width, int style, int tile) {
    auto blit = [&](int sx, int sy, int wdt, int dx) {
        for (int j = 0; j < 12; ++j)
            for (int i = 0; i < wdt; ++i) {
                const uint8_t v = skin_[static_cast<size_t>(sy + j) * kScreenW + sx + i];
                if (v != kSkinClear) plot(dx + i, y + j, v);
            }
    };
    const int row = style ? 24 : 0;
    int cx = x;
    blit(tile * 16, row, 16, cx);
    cx += 16;
    const int mid = std::max(0, width - 6);
    for (int k = 0; k < mid / 16; ++k, cx += 16) blit(128, row, 16, cx);
    if (mid % 16) { blit(128, row, mid % 16, cx); cx += mid % 16; }
    blit(tile * 16, row + 12, 16, cx);
    changed_ = true;
}

void Game::text(int id, int x, int y, int width, bool centered, uint8_t color, int height) {
    if (id < 0 || id >= static_cast<int>(pack_.texts().size())) return;
    // texte posé en haut d'un rectangle qui vient d'être effacé (barre du bas…) : centré verticalement dans ce
    // rectangle, comme la version Atari
    int box = 0;
    if (y == last_fill_[1] && x >= last_fill_[0] && x <= last_fill_[2]) box = last_fill_[3] - last_fill_[1] + 1;
    // dans un panneau (service 10), une ligne un peu trop longue pour la largeur demandée tient sur une ligne
    // jusqu'aux bords intérieurs du panneau (même centre) : sinon elle recouvrirait la ligne suivante, que la
    // logique place juste dessous (le nombre du pourcentage d'indices)
    if (centered && y >= last_panel_[1] && y <= last_panel_[3] && x >= last_panel_[0] && x + width <= last_panel_[2] + 1) {
        const Font& f = height <= 7 ? pack_.font7() : pack_.font8();
        int total = 0;
        auto t = pack_.texts()[id];
        while (!t.empty() && t.back() == 0x4C) t.pop_back();                     // espaces finales
        for (uint8_t g : t)
            if (g > 0 && g <= f.glyphs.size() && g != 0x65) total += f.glyphs[g - 1].advance;
        const int centre = x + width / 2;
        const int half = std::min(centre - last_panel_[0], last_panel_[2] - centre) - 2;
        if (total > width && total <= 2 * half) { x = centre - half; width = 2 * half; }
        if (std::getenv("MAUPITI_DEBUG"))
            std::fprintf(stderr, "panneau : texte %d total %d largeur %d demi %d\n", id, total, width, half);
    }
    glyph_text(pack_.texts()[id], x, y, width, centered, color, height, box);
}

void Game::glyph_text(const std::vector<uint8_t>& glyphs, int x, int y, int width, bool centered, uint8_t color,
                      int height, int box) {
    const Font& f = height <= 7 ? pack_.font7() : pack_.font8();
    constexpr uint8_t kSpace = 0x4C, kNewline = 0x65;
    auto adv = [&](uint8_t g) { return (g > 0 && g <= f.glyphs.size()) ? f.glyphs[g - 1].advance : 0; };
    // mise en page comme le service 9 d'origine (0493:0FC6) : somme des avances des glyphes ; au-delà de la largeur, le dernier espace devient un retour à la ligne et la somme repart
    // de zéro (les caractères déjà passés de la nouvelle ligne ne sont pas recomptés, comme l'original)
    std::vector<uint8_t> g(glyphs.begin(), glyphs.end());
    {
        int cx = 0;
        size_t last_space = std::string::npos;
        for (size_t k = 0; k < g.size(); ++k) {
            const uint8_t c = g[k];
            if (c == kNewline) { cx = 0; last_space = std::string::npos; continue; }
            if (c == kSpace) last_space = k;
            if (c > 0 && c <= f.glyphs.size()) cx += f.glyphs[c - 1].advance;   // table +0x66 (avances)
            if (cx > width && last_space != std::string::npos) {
                g[last_space] = kNewline;
                last_space = std::string::npos;
                cx = 0;
            }
        }
    }
    std::vector<std::vector<uint8_t>> lines(1);
    for (uint8_t c : g) {
        if (c == kNewline) lines.emplace_back();
        else lines.back().push_back(c);
    }
    if (std::getenv("MAUPITI_DEBUG"))
        std::fprintf(stderr, "texte : %zu lignes, largeur %d, glyphes %zu\n", lines.size(), width, glyphs.size());
    int ly = y;
    if (box > 0) ly += std::max(0, (box - static_cast<int>(lines.size()) * f.height) / 2);
    for (auto& l : lines) {
        while (!l.empty() && l.back() == kSpace) l.pop_back();
        int lw = 0;
        for (uint8_t g : l) lw += adv(g);
        int gx = centered ? x + (width - lw) / 2 : x;
        for (uint8_t g : l) {
            if (g == 0 || g > f.glyphs.size()) continue;
            const Glyph& gl = f.glyphs[g - 1];
            for (int jy = 0; jy < f.height; ++jy)
                for (int ix = 0; ix < gl.w; ++ix)
                    if (f.bitmap[static_cast<size_t>(jy) * f.atlas_width + gl.x + ix]) plot(gx + ix, ly + jy, color);
            gx += gl.advance;
        }
        ly += f.height;
    }
    changed_ = true;
}

uint32_t Game::service(int n, const std::vector<uint16_t>& w) {
    auto& cpu = host_.cpu();
    if (std::getenv("MAUPITI_TRACE") && n != 3 && n != 4 && n != 5 && n != 15 && n != 31 && n != 32 && n != 33) {
        std::fprintf(stderr, "[%d] t=%llu", n, static_cast<unsigned long long>(clock_ms_));
        for (size_t i = 0; i < w.size() && i < 32; ++i) std::fprintf(stderr, " %04X", w[i]);
        std::fputc(10, stderr);
    }
    auto arg = [&](size_t i) -> int { return i < w.size() ? static_cast<int16_t>(w[i]) : 0; };
    switch (n) {
    case 1: case 6: {                               // emplacement du n-ième élément d'une table de tailles
        const int count = arg(0);
        uint32_t a = ptr(w, 2), sum = 0;
        const uint32_t d = n == 1 ? kDescriptor1 : kDescriptor6;
        if (n == 1) {                               // tailles sur un octet ; descripteur de 4 octets
            for (int k = 0; k < count; ++k) sum += cpu.rb(a++);
            cpu.wb(d, count ? (sum >> 8) & 0xFF : 0);
            cpu.wb(d + 1, count ? sum & 0xFF : 0);
            cpu.wb(d + 2, 0);
            cpu.wb(d + 3, cpu.rb(a));
        } else {                                    // tailles sur un mot ; descripteur de 8 octets
            for (int k = 0; k < count; ++k, a += 2) sum += host_.be16(a);
            cpu.wb(d, 0);
            cpu.wb(d + 1, count ? (sum >> 16) & 0xFF : 0);
            host_.set_be16(d + 2, count ? sum & 0xFFFF : 0);
            host_.set_be16(d + 4, 0);
            cpu.wb(d + 6, cpu.rb(a));
            cpu.wb(d + 7, cpu.rb(a + 1));
        }
        return d;
    }
    case 2: {                                       // liste de textes (menus), comme maup.exe 0493:1108
        // n° des textes : octets à w14:w15 + 1, pas de 2 ; nombre w16 ; x w20 ; y w18 ; couleur w24 ; style w12 ;
        // les w22 premiers éléments sont alignés à droite (finissent en x), les suivants partent de x ;
        // chaque ligne est une barre de tuiles de 12 pixels (planche du service 23), texte 2 pixels plus bas
        const uint32_t list = ptr(w, 14);
        const int count = arg(16), x = arg(20), y = arg(18), right = std::max(0, arg(22));
        const int style = arg(12) ? 1 : 0;
        const uint8_t color = static_cast<uint8_t>(arg(24) & 15);
        for (int k = 0; k < count; ++k) {
            const int id = text_base(ptr(w, 6)) + cpu.rb(list + 1 + 2 * k) - 1;
            const int tw = text_width(id, 8);
            const bool left_col = k < right;
            const int ly = y + 12 * (left_col ? k : k - right);
            const int tile = static_cast<int>((ticks_ + static_cast<uint32_t>(x + ly)) & 7);
            if (left_col) {
                menu_bar(x - 0x1A - tw, ly, tw, style, tile);
                text(id, x - 0x10 - tw, ly + 2, 320, false, color, 8);
            } else {
                menu_bar(x, ly, tw, style, tile);
                text(id, x + 0x0A, ly + 2, 320, false, color, 8);
            }
        }
        return 0;
    }
    case 9:                                         // texte (paramètres sur 32 bits : n° à partir de 1)
        if (std::getenv("MAUPITI_FONT")) {
            uint32_t base = 0;
            const Loaded* l = loaded_at(ptr(w, 16), &base);
            std::fprintf(stderr, "police %05X : bloc %05X ressource %d/%d taille %u\n", ptr(w, 16), base, l ? l->id : -1,
                         l ? l->sub : -1, l ? l->size : 0);
        }
        if (ptr(w, 22) == 0xFFFFF) {                // sans table de textes : nombre p0 (pourcentage d'indices…)
            std::vector<uint8_t> digits;
            for (const char c : std::to_string((static_cast<uint32_t>(w[1]) << 16) | w[0]))
                digits.push_back(static_cast<uint8_t>(0x35 + (c - '0')));             // glyphes '0'…'9'
            glyph_text(digits, arg(14), arg(12), arg(10), arg(4) != 0, vga_color(arg(6)), arg(8));
            return 0;
        }
        // le n° est relatif au groupe désigné par p11 (entrée de la table des longueurs) : répliques = 1138…
        text(text_base(ptr(w, 22)) + arg(0) - 1, arg(14), arg(12), arg(10), arg(4) != 0,
             vga_color(arg(6)), arg(8));
        return 0;
    case 10: {                                      // panneau (maup.exe 0159:0490) : tuiles 16x36 de la planche
        // x w4, y w6, largeur w8 ; w10 = 0 : tuiles gauche (160,0), milieu (144,0), droite (176,0) ;
        // sinon (208,0), (192,0), (224,0) ; couleur transparente comme les menus
        const int x0 = arg(4), y0 = arg(6), width = arg(8), mid_sx = arg(10) ? 192 : 144;
        auto blit = [&](int sx, int wdt, int dx) {
            for (int j = 0; j < 36; ++j)
                for (int i = 0; i < wdt; ++i) {
                    const uint8_t v = skin_[static_cast<size_t>(j) * kScreenW + sx + i];
                    if (v != kSkinClear) plot(dx + i, y0 + j, v);
                }
        };
        int cx = x0;
        blit(mid_sx + 16, 16, cx);
        cx += 16;
        const int mid = std::max(0, width - 0x22);
        for (int k = 0; k < mid / 16; ++k, cx += 16) blit(mid_sx, 16, cx);
        if (mid % 16) { blit(mid_sx, mid % 16, cx); cx += mid % 16; }
        blit(mid_sx + 32, 16, cx);
        flip(x0, y0, cx + 15, y0 + 35);
        last_panel_ = {x0, y0, cx + 15, y0 + 35};
        return 0;
    }
    case 12: case 22: case 37:                      // curseur de la souris (forme, cacher, montrer)
        return 0;
    case 13:                                        // rectangle plein, couleur 0
        fill(arg(6), arg(4), arg(2), std::min(arg(0), 199), 0);
        last_fill_ = {arg(6), arg(4), arg(2), std::min(arg(0), 199)};
        if (!ptr(w, 8)) flip(arg(6), arg(4), arg(2), std::min(arg(0), 199));   // sans tampon : à l'écran
        // fouille fructueuse : la logique efface le cadran de l'horloge pour y montrer le temps passé ; à ce
        // moment, la version Atari (programme principal $22F32) joue au hasard le bruitage 67 ou 68 par $144
        // (le 2e effacement, à la fin de la fouille, remet le décor : le bruitage s'arrête)
        if (music_ && arg(6) == 0x108 && arg(4) == 0x6C && arg(2) == 0x13C && arg(0) == 0xC3) {
            if (!search_sound_) music_->call(0x144, 0x43 + static_cast<unsigned>(clock_ms_ & 1));
            else stop_effects();
            search_sound_ = !search_sound_;
        }
        return 0;
    case 14: {                                      // image
        // w6:w7 = l'image (format Lankhor), calculée par la logique à l'aide du service 6 ; w2, w4 = décalage
        const uint32_t block = ptr(w, 0), desc = ptr(w, 6);
        const uint32_t at = desc;
        std::vector<uint8_t> data(cpu.mem.begin() + std::min<uint32_t>(at, x86::Cpu::kMemSize),
                                  cpu.mem.begin() + std::min<uint32_t>(at + 65000, x86::Cpu::kMemSize));
        try {
            // décodeur 0493:0000 : (image, écran, x, y) ; le service lui passe x = w4, y = w2
            const Image im = decode_image(data);
            // graphismes Atari : image de même rang dans l'archive équivalente (interface, sprites des décors,
            // objets, documents…)
            const Image* atari = atari_equivalent(desc, im);
            draw_image(atari ? *atari : im, arg(4), arg(2));
            if (std::getenv("MAUPITI_DEBUG")) {
                uint32_t base = 0;
                const Loaded* l = loaded_at(desc, &base);
                std::fprintf(stderr, "svc14 image %05X (ressource %d/%d +%X) en %d,%d %dx%d decal %d,%d pal %d\n", desc,
                             l ? l->id : -1, l ? l->sub : -1, desc - base, im.x, im.y, im.width, im.height, arg(4), arg(2),
                             im.palette ? 1 : 0);
                if (l && l->id >= 0 && l->id < static_cast<int>(atari_map_.size())) {
                    const auto list = atari_entry(atari_map_[l->id], l->sub);
                    std::fprintf(stderr, "    atari %s ; archive %d entrée %d :", atari ? "oui" : "non",
                                 atari_map_[l->id], l->sub);
                    for (const auto& a : list) std::fprintf(stderr, " %dx%d@%d,%d%s", a.width, a.height, a.x, a.y,
                                                            a.palette ? "p" : "");
                    std::fputc(10, stderr);
                }
            }
            // image avec sa propre palette, issue de la ressource 0x21 (documents) : maup.exe la recopie dans
            // la palette courante de la logique, que le service 18 applique ensuite
            const Image& shown = atari ? *atari : im;
            uint32_t doc_base = 0;
            const Loaded* doc = loaded_at(desc, &doc_base);
            if (!data.empty() && data[0] == 0 && doc && doc->id == 0x21 && shown.palette && pal_mem_) {
                for (int k = 0; k < 16; ++k) host_.set_be16(pal_mem_ + 2 * k, (*shown.palette)[k]);
                pal_ = *shown.palette;
            }
        } catch (const std::exception&) {
            ++unknown_services[1014];               // image illisible (diagnostic)
            if (std::getenv("MAUPITI_DEBUG")) {
                uint32_t base = 0;
                const Loaded* l = loaded_at(block, &base);
                std::fprintf(stderr, "svc14 bloc %05X (ressource %d/%d) desc %05X [%02X %02X %02X %02X %02X %02X %02X %02X] x %d y %d w8 %05X w10 %05X : %02X %02X %02X %02X\n",
                             block, l ? l->id : -1, l ? l->sub : -1, desc, cpu.rb(desc), cpu.rb(desc + 1), cpu.rb(desc + 2),
                             cpu.rb(desc + 3), cpu.rb(desc + 4), cpu.rb(desc + 5), cpu.rb(desc + 6), cpu.rb(desc + 7),
                             arg(2), arg(4), ptr(w, 8), ptr(w, 10), cpu.rb(at), cpu.rb(at + 1), cpu.rb(at + 2), cpu.rb(at + 3));
            }
        }
        if (!block) flip(0x108, 0x6D, 0x13E, 0xB2);   // sans bloc : maup.exe recopie la zone de l'horloge
        return 0;
    }
    case 26: {                                      // écran d'interrogatoire (ressource 0x22) : fond, visage
        const uint32_t b = ptr(w, 2), a = ptr(w, 4);
        const int who = arg(8);
        auto draw_at = [&](uint32_t at, int dx, int dy) {
            std::vector<uint8_t> data(cpu.mem.begin() + std::min<uint32_t>(at, x86::Cpu::kMemSize),
                                      cpu.mem.begin() + std::min<uint32_t>(at + 65000, x86::Cpu::kMemSize));
            try { draw_image(decode_image(data), dx, dy); } catch (const std::exception&) { ++unknown_services[1026]; }
        };
        // les blocs PC sont chargés comme maup.exe (la logique peut les relire) ; on dessine ceux de l'Atari
        // (archive 15 : entrée 0 = fond, 3 entrées par personnage), mêmes positions, décalage (0x1C, -1)
        auto atari = [&](int entry) { return atari_entry(15, entry); };
        music_context_ = 0x20;
        for (auto& m : markers_) m.shown = false;
        host_.load_resource(0x22, 0, b);
        if (auto bg = atari(0); !bg.empty()) {          // fond : le décor recoloré (service 34) se voit à travers
            draw_image(bg[0], 0, 0);
        } else {
            draw_at(b, 0, 0);
        }
        host_.load_resource(0x22, 3 * who + 1, b);
        if (auto face = atari(3 * who + 1); !face.empty()) draw_image(face[0], 0x1C, -1); else draw_at(b, 0x1C, -1);
        host_.load_resource(0x22, 3 * who + 3, a);
        interlocutor_ = who;
        return 0;
    }
    case 27: {                                      // carte de l'île (filature) : ressource 0x23 (maup.exe 0159:0FF3)
        // bloc 0 = carte (avec sa palette, mise dans les trois palettes A, B, C), bloc w4+1 = portrait de la
        // personne suivie, dessiné au décalage (-82, -26) ; équivalent Atari : archive 14 (mêmes rangs)
        const uint32_t dst = ptr(w, 2);
        const int who = arg(4);
        auto draw_from_memory = [&](int dx, int dy) -> std::optional<Image> {
            std::vector<uint8_t> data(cpu.mem.begin() + std::min<uint32_t>(dst, x86::Cpu::kMemSize),
                                      cpu.mem.begin() + std::min<uint32_t>(dst + 65000, x86::Cpu::kMemSize));
            try { Image im = decode_image(data); draw_image(im, dx, dy); return im; }
            catch (const std::exception&) { ++unknown_services[1027]; return std::nullopt; }
        };
        for (auto& m : markers_) m = Marker{};
        music_context_ = 0x1F;
        host_.load_resource(0x23, 0, dst);
        std::optional<Image> map;
        if (auto a = atari_entry(14, 0); !a.empty()) { a[0].transparent = 16; draw_image(a[0], 0, 0); map = a[0]; }
        else map = draw_from_memory(0, 0);
        if (map && map->palette) {
            pal_set_ = {*map->palette, *map->palette, *map->palette};
            pal_set_ok_ = true;
        }
        host_.load_resource(0x23, who + 1, dst);
        if (auto a = atari_entry(14, who + 1); !a.empty()) draw_image(a[0], -82, -26);
        else draw_from_memory(-82, -26);
        return (0xC4u << 16) | 0x0B;
    }
    case 17: {                                      // marqueurs de la carte (maup.exe 02DB:0B16)
        // w10 : marqueur (0 = forme 7, 1 = forme 8) ; w8 non nul : l'effacer ; sinon cible (x w2, y w0), départ
        // (x w6, y w4) ou, si w4 = -1, l'ancienne cible : le déplacement part quand les deux marqueurs en ont un
        Marker& m = markers_[arg(10) ? 1 : 0];
        if (arg(8)) { m.shown = false; changed_ = true; return 0; }
        if (arg(4) == -1) { m.x = m.tx; m.y = m.ty; m.pending = true; }
        else { m.x = arg(6); m.y = arg(4); m.pending = false; }
        m.tx = arg(2);
        m.ty = arg(0);
        m.shown = true;
        changed_ = true;
        if (markers_[0].pending && markers_[1].pending) {
            markers_moving_ = true;
            cpu.stop = true;
        }
        return 0;
    }
    case 47:                                        // musique du contexte (maup.exe 0742:015C ; Atari $BA48)
        context_music();
        return 0;
    case 48:                                        // jingle (pistes PC 17/18 ; Atari $B88C : banques 29 / 32)
        if (music_ && !cabin_jingle_) {
            static constexpr int kJingle[2] = {0x20, 0x1D};
            jingle_cycle_ = jingle_cycle_ >= 1 ? 0 : jingle_cycle_ + 1;
            music_->call(0x148, static_cast<unsigned>(kJingle[jingle_cycle_]));
            jingle_on_ = true;
        }
        return 0;
    case 46:                                        // heure, à l'entrée dans un lieu (Atari $BB12)
        hour_sound(arg(0));
        return 0;
    case 45:                                        // arrêt de la musique
        if (music_) music_->stop();
        return 0;
    case 41: {                                      // changement d'heure : palette A, B ou C de l'écran (fondu)
        if (pal_set_ok_) { pal_ = pal_set_[hour_variant(arg(2))]; changed_ = true; }
        return 0;
    }
    case 18: {                                      // transition d'écran (mode w16) ; palette à w12:w13
        {   // palette courante de la logique (mots ST big-endian) ; toute à zéro = pas encore remplie
            const uint32_t p = ptr(w, 12);
            Palette np{};
            bool any = false;
            for (int k = 0; k < 16; ++k) { np[k] = host_.be16(p + 2 * k); any |= np[k] != 0; }
            if (any && np != pal_) { pal_ = np; changed_ = true; }
        }
        {   // modes 2 et 3 : tout l'écran (effets) ; 4 et 5 : rectangle (x arrondis à 16) ; 0, 1, 6 : palette seule
            const int mode = arg(16);
            if (mode == 2 || mode == 3) flip(0, 0, kScreenW - 1, kScreenH - 1);
            else if (mode != 0 && mode != 1 && mode != 6)
                flip(arg(6) & 0xFFF0, arg(4), (arg(2) & 0xFFF0) + 15, std::min(arg(0), 199));
        }
        if (std::getenv("MAUPITI_DEBUG")) {
            const uint32_t p = ptr(w, 12);
            std::fprintf(stderr, "svc18 mode %d palette %05X :", arg(16), p);
            for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %03X", host_.be16(p + 2 * k));
            std::fputc(10, stderr);
        }
        return 0;
    }
    case 42: {                                      // palette selon l'heure (maup.exe 02DB:1A13)
        // t = (w6 / 2) mod 24 ; 8-18 h : palette A (+0) ; 6-8 h et 18-20 h : B (+0x20) ; nuit : C (+0x40)
        const int variant = hour_variant(arg(6));
        if (pal_set_ok_) {
            pal_ = pal_set_[variant];
            changed_ = true;
        } else if (current_room_ >= 0) {
            const Room& r = pack_.rooms()[current_room_];
            pal_ = variant == 0 ? r.pal_a : variant == 1 ? r.pal_b : (r.image.palette ? *r.image.palette : r.pal_a);
            changed_ = true;
        }
        const uint32_t dst = ptr(w, 2);                 // copie dans la palette courante de la logique
        pal_mem_ = dst;
        for (int k = 0; k < 16; ++k) host_.set_be16(dst + 2 * k, pal_[k]);
        return 0;
    }
    case 35: {                                      // sauvegarder l'état du jeu (taille w0, adresse w2:w3)
        const uint32_t size = w.empty() ? 0 : w[0], src = ptr(w, 2);
        std::error_code ec;
        std::filesystem::create_directories(save_file.parent_path(), ec);
        std::ofstream f(save_file, std::ios::binary);
        for (uint32_t i = 0; i < size; ++i) f.put(static_cast<char>(cpu.rb(src + i)));
        return f ? 1 : 0;
    }
    case 11: {                                      // reprendre une partie sauvegardée (taille w0, adresse w2:w3)
        if (!load_save || !std::filesystem::exists(save_file)) return 0;
        std::ifstream f(save_file, std::ios::binary);
        const std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const uint32_t size = w.empty() ? 0 : w[0], dst = ptr(w, 2);
        for (uint32_t i = 0; i < size && i < d.size(); ++i) cpu.wb(dst + i, static_cast<uint8_t>(d[i]));
        return 1;
    }
    case 23:                                        // souris et horloge : enregistrement d'entrée de la logique
        // planche des tuiles de menus : maup.exe copie les 64 premières lignes de l'écran de travail
        std::copy(work_.begin(), work_.begin() + skin_.size(), skin_.begin());
        host_.input = ptr(w, 0);
        host_.set_be16(host_.input + 2, 0);
        host_.set_be16(host_.input + 4, 0xFFFC);
        cpu.wb(host_.input + 0x1A, 0);
        cpu.wb(host_.input + 0x1B, 0);
        cpu.wb(host_.input + 0x1C, 0);
        return 0;
    case 24: {                                      // décor : blocs 0 (palettes + image) et 1 (sprites)
        const int id = arg(0);
        host_.load_resource(id, 0, ptr(w, 4));
        if (ptr(w, 2)) host_.load_resource(id, 1, ptr(w, 2));
        const int room = pack_.room_for_resource(id);
        if (room >= 0 && room < static_cast<int>(pack_.rooms().size())) {   // décor Atari, palette A
            const Room& r = pack_.rooms()[room];
            pal_ = r.pal_a;
            pal_set_ = {r.pal_a, r.pal_b, r.image.palette ? *r.image.palette : r.pal_a};
            pal_set_ok_ = true;
            for (auto& m : markers_) m.shown = false;
            draw_image(r.image, 0, 0);
            // zones animées du décor Atari (vides dans l'image) : première image en attendant les animations
            for (const auto& a : r.anims)
                if (!a.empty()) draw_image(a[0], 0, 0);
            if (std::getenv("MAUPITI_DEBUG"))
                std::fprintf(stderr, "svc24 ressource %d -> lieu %d (%dx%d en %d,%d)\n", id, room + 1, r.image.width,
                             r.image.height, r.image.x, r.image.y);
            current_room_ = room;
            current_room_res_ = id;
            music_context_ = id;
            if (jingle_on_ || search_sound_) stop_effects();
            search_sound_ = false;
        }
        return 0;
    }
    case 36: {                                      // sauvegarde / restauration d'un rectangle (4 bits par pixel)
        const int y2 = std::min(arg(0), 199), y1 = arg(4);
        const int x1 = arg(6) & 0x1F0, x2 = (arg(2) & 0x1F0) + 15;
        const bool restore = arg(12) != 0;
        uint32_t m = restore ? ptr(w, 10) : ptr(w, 8);
        const uint32_t start = m;
        for (int y = y1; y <= y2; ++y)
            for (int x = x1; x <= x2; x += 2) {
                const size_t o = static_cast<size_t>(y) * kScreenW + x;
                if (o + 1 >= work_.size()) { ++m; continue; }
                if (restore) {
                    const uint8_t b = cpu.rb(m++);
                    work_[o] = b >> 4;
                    if (x + 1 <= x2) work_[o + 1] = b & 15;
                } else {
                    cpu.wb(m++, static_cast<uint8_t>((work_[o] << 4) | (x + 1 <= x2 ? (work_[o + 1] & 15) : 0)));
                }
            }
        if (restore) changed_ = true;
        if (!ptr(w, 8)) flip(x1, y1, x2, y2);       // sans tampon de sauvegarde : recopie à l'écran
        return m - start;
    }
    case 30:                                        // voix : réplique u (à partir de 1) du script phonétique
        if (!w.empty() && w[0] >= 1) {
            if (on_voice) on_voice(w[0] - 1, w);
            // la voix d'origine est bloquante : la logique attend la fin de la réplique (texte affiché)
            speaking_ = true;
            speak_start_ = clock_ms_;
            {   // durée minimale d'affichage : 60 ms par caractère, au moins 1,5 s (voix absente ou courte)
                const int tid = 1137 + w[0];
                const size_t len = tid < static_cast<int>(pack_.texts().size()) ? pack_.texts()[tid].size() : 20;
                speak_min_ = std::max<uint64_t>(1500, 60 * len);
            }
            host_.cpu().stop = true;
        }
        return 0;
    case 39:                                        // trait
        line(arg(4), arg(2), arg(8), arg(6), vga_color(arg(0)));
        if (!ptr(w, 10)) flip(arg(4), arg(2), arg(8), arg(6));
        return 0;
    case 34: {                                      // recoloriage d'un rectangle par table (maup.exe 0493:0E10)
        // x1 w4, y1 w6, x2 w8, y2 w10 ; table de 16 octets à w12:w13. maup.exe encadre la table d'une inversion
        // des 4 bits (son ordre des couleurs VGA) ; nos indices sont ceux de l'Atari : table appliquée telle quelle.
        // Sert avant l'interrogatoire : le décor passe en gris, et se voit à travers le fond (archive 15)
        const uint32_t table = ptr(w, 12);
        for (int y = std::max(arg(6), 0); y <= std::min(arg(10), kScreenH - 1); ++y)
            for (int x = std::max(arg(4), 0); x <= std::min(arg(8), kScreenW - 1); ++x) {
                uint8_t& v = work_[static_cast<size_t>(y) * kScreenW + x];
                v = cpu.rb(table + (v & 15)) & 15;
            }
        if (std::getenv("MAUPITI_DEBUG")) {
            std::fprintf(stderr, "svc34 table :");
            for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %d", cpu.rb(table + k));
            std::fputc(10, stderr);
        }
        return 0;
    }
    case 8:                                         // point 2x2 (couleur w0, y w2, x w4) ; sans tampon : à l'écran
        fill(arg(4), arg(2), arg(4) + 1, arg(2) + 1, vga_color(arg(0)));
        if (!ptr(w, 6)) flip(arg(4), arg(2), arg(4) + 1, arg(2) + 1);
        return 0;
    case 29:                                        // recopie d'un rectangle à l'écran (y2, x2, y1, x1)
        flip(arg(6), arg(4), arg(2), std::min(arg(0), 199));
        return 0;
    case 19:                                        // recopie d'une case 32x32 à l'écran (position : table de maup.exe)
        flip(0, 0, kScreenW - 1, kScreenH - 1);
        return 0;
    default:
        ++unknown_services[n];
        if (std::getenv("MAUPITI_DEBUG")) {
            std::fprintf(stderr, "service %d :", n);
            for (uint16_t x : w) std::fprintf(stderr, " %04X", x);
            std::fputc(10, stderr);
        }
        return 0;
    }
}

}  // namespace maupiti
