// Maupiti Island — squelette du moteur (étape 5).
//
//   maupiti [--pack DIR]                       visionneuse interactive
//   maupiti --pack DIR --screenshot N F.bmp    rend le lieu N (palette A) dans un BMP et quitte
//   maupiti --pack DIR --dump-indices DIR2     écrit les images décodées (tests de non-régression)
//   maupiti --pack DIR --say N [--speaker C]   joue la voix du texte N (personnage C) et quitte
//   maupiti --pack DIR --viewer                visionneuse seule (sans écran titre ni nouvelle partie)
//
// Touches : ←/→ lieu, 1/2/3 palette A/B/C, ↑/↓ texte, S sprites (1er état/dernier état/tous/aucun),
//           A proportions 4:3 / pixels carrés, T écran titre,
//           V voix oui/non, R rejouer la réplique, PgPréc/PgSuiv ±10 textes,
//           Tab/Maj+Tab voix suivante/précédente, E style d'intonation, Espace jouer,
//           H barre d'outils (cliquable : texte, voix, style, JOUER),
//           F11 ou Alt+Entrée plein écran, Échap quitter.
#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "audio.hpp"
#include "jukebox.hpp"
#include "media.hpp"
#include "logic/host.hpp"
#include "game.hpp"
#include "pack.hpp"
#include "screen.hpp"
#include "ui.hpp"

using namespace maupiti;
namespace fs = std::filesystem;

namespace {

struct ViewState {
    int room = 0;
    int lighting = 0;      // palette du lieu : 0 = A (vérifiée), 1 = B (sombre), 2 = C (chaude)
    int text = 35;         // premier texte « parlant »
    int sprites = 1;       // 0 aucun, 1 premier état, 2 dernier état, 3 tous (débogage)
    bool title = false;
    bool aspect = true;    // pixels 1:1,2 comme sur un moniteur 4:3
    bool voice = true;     // lit la réplique du texte affiché (POC synthèse vocale)
    bool toolbar = true;   // barre d'outils voix
    int voice_idx = 0;     // voix choisie (variantes de pack/voices)
    int style_idx = 0;     // style d'intonation choisi
    // Partie en cours (panneau de droite et zone de texte, comme la logique d'origine)
    bool game = false;
    int hour = 20;         // [0x168] : heures depuis le 30.01 à 12 h (20 = 31.01, 8 h)
    int tenth = 0;         // [0x16A] : tranches de 10 minutes
    int fatigue = 75;      // [0x16C] : forme 0..99
    int lamp_text = 35;    // lampe : personnages présents, ou « Vous êtes SEUL »
    int zone_text = -1;    // texte de la zone du bas (-1 : vide)
    bool anims = true;     // animations des lieux (entrée 3 des archives Atari)
    int anim_tick = 0;     // image courante des animations
};

template <class T> int index_of(const std::vector<T>& v, const T& x) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == x) return static_cast<int>(i);
    return -1;
}

// Une banque de sprites contient, pour chaque emplacement, plusieurs états
// (porte ouverte / fermée…). En attendant la logique du jeu (étape 6), la
// visionneuse montre le premier ou le dernier état de chaque emplacement.
// Les images en (0,0) sont des icônes, pas des éléments du décor.
bool sprite_selected(const std::vector<Image>& bank, size_t k, int mode) {
    if (mode == 3) return true;
    if (mode == 0) return false;
    const Image& s = bank[k];
    if (s.x == 0 && s.y == 0) return false;
    auto same = [&](size_t j) {
        return bank[j].x == s.x && bank[j].y == s.y && bank[j].width == s.width && bank[j].height == s.height;
    };
    if (mode == 1) {
        for (size_t j = 0; j < k; ++j)
            if (same(j)) return false;
    } else {
        for (size_t j = k + 1; j < bank.size(); ++j)
            if (same(j)) return false;
    }
    return true;
}

const Palette& room_palette(const Room& r, int lighting) {
    if (lighting == 1) return r.pal_b;
    if (lighting == 2) return *r.image.palette;
    return r.pal_a;
}

// Panneau de droite, relevé sur la logique d'origine (oracle, service 9 : texte, couleur, largeur, x, y) :
// chaque texte est dessiné en noir décalé d'un pixel, puis en couleur 15.
void panel_text(Screen& scr, const Pack& pack, int text, int x, int y, int w, bool centered, const Palette& pal) {
    if (text < 0 || text >= static_cast<int>(pack.texts().size())) return;
    const auto& g = pack.texts()[text];
    scr.text_box(pack.font8(), g, x + 1, y + 1, w, centered, st_to_argb(pal[0]));
    scr.text_box(pack.font8(), g, x, y + 1, w, centered, st_to_argb(pal[0]));
    scr.text_box(pack.font8(), g, x, y, w, centered, st_to_argb(pal[15]));
}

void draw_panel(Screen& scr, const Pack& pack, const ViewState& v, const Palette& pal) {
    panel_text(scr, pack, v.lamp_text, 276, 50, 30, true, pal);                         // lampe
    panel_text(scr, pack, 36 + std::clamp(v.fatigue, 0, 99) / 20, 265, 110, 50, true, pal);  // forme
    // heure : 30.01 à 12 h + v.hour heures ; AM/PM (textes 140/141) et date (228 = 31.01, 229 = 01.02)
    const int abs_h = v.hour + 12;
    const int h24 = abs_h % 24, day = abs_h / 24;          // jour 1 = 31.01
    panel_text(scr, pack, h24 < 12 ? 140 : 141, 270, 164, 20, false, pal);
    panel_text(scr, pack, day <= 1 ? 228 : 229, 286, 164, 30, false, pal);
    // aiguilles (centre 290,141 ; grande 12 px, petite 9 px ; ombre noire décalée)
    const double kPi = 3.14159265358979;
    const double minutes = v.tenth * 10.0;
    const double am = (minutes / 60.0) * 2 * kPi, ah = ((h24 % 12) + minutes / 60.0) / 12.0 * 2 * kPi;
    const int cx = 290, cy = 141;
    const int mx = cx + static_cast<int>(std::lround(12 * std::sin(am))), my = cy - static_cast<int>(std::lround(12 * std::cos(am)));
    const int hx = cx + static_cast<int>(std::lround(9 * std::sin(ah))), hy = cy - static_cast<int>(std::lround(9 * std::cos(ah)));
    scr.line(cx + 1, cy + 1, mx + 1, my + 1, st_to_argb(pal[0]));
    scr.line(cx + 1, cy + 1, hx + 1, hy + 1, st_to_argb(pal[0]));
    scr.line(cx, cy, mx, my, st_to_argb(pal[15]));
    scr.line(cx, cy, hx, hy, st_to_argb(pal[15]));
}

// Nouvelle partie : état de départ SAUP.MAU (mots little-endian à partir de DS:0x166 de la logique).
// Écran relevé sur l'oracle juste après le service 40 : cabine, portes fermées, lampe « Vous êtes SEUL »,
// forme, 31.01 8 h AM, et dans la zone du bas le texte 478 « Ça sent le roussi... ».
void start_game(ViewState& v, const Pack& pack) {
    const auto& st = pack.start_state();
    if (st.size() < 8) throw std::runtime_error("start.sav absent ou incomplet");
    auto word = [&](int i) { return st[2 * i] | (st[2 * i + 1] << 8); };
    if (const int room = pack.room_for_resource(word(0) - 1); room >= 0) v.room = room;
    v.hour = word(1);
    v.tenth = word(2);
    v.fatigue = word(3);
    v.title = false;
    v.game = true;
    v.lighting = 0;
    v.sprites = 2;            // dernier état des emplacements : portes fermées (capture Atari)
    v.lamp_text = 35;
    v.zone_text = 478;
    v.text = 35;
}

void compose(Screen& scr, const Pack& pack, const ViewState& v) {
    scr.clear();
    if (v.title) {
        scr.draw(pack.title(), *pack.title().palette);
        return;
    }
    const Room& r = pack.rooms()[v.room];
    const Palette& pal = room_palette(r, v.lighting);
    scr.draw(r.image, pal);
    // zones réservées : animations Atari, sinon dessin fixe du décor PC là où aucune animation ne passe
    auto covered = [&](const Image& p) {
        for (const auto& a : r.anims)
            if (!a.empty() && p.x < a[0].x + a[0].width && a[0].x < p.x + p.width && p.y < a[0].y + a[0].height &&
                a[0].y < p.y + p.height)
                return true;
        return false;
    };
    for (const auto& p : r.patches)
        if (!v.anims || !covered(p)) scr.draw(p, pal);
    if (v.anims)
        for (const auto& a : r.anims) scr.draw(a[static_cast<size_t>(v.anim_tick) % a.size()], pal);
    // Les sprites utilisent eux aussi la palette du lieu (une seule palette à l'écran sur Atari).
    for (size_t k = 0; k < r.sprites.size(); ++k)
        if (sprite_selected(r.sprites, k, v.sprites)) scr.draw(r.sprites[k], pal);
    // Sur Atari, tout l'écran utilise la palette du décor (cadre compris).
    for (const auto& part : pack.frame()) scr.draw(part, pal);
    scr.draw(pack.menubar(), pal);
    // zone de texte sous le décor
    scr.fill(6, 179, 255, 21, st_to_argb(pal[0]));
    if (v.game) {
        draw_panel(scr, pack, v, pal);
        if (v.zone_text >= 0 && v.zone_text < static_cast<int>(pack.texts().size()))
            scr.text_box(pack.font8(), pack.texts()[v.zone_text], 4, 179, 258, true, st_to_argb(pal[15]));
        return;
    }
    if (v.text >= 0 && v.text < static_cast<int>(pack.texts().size()))
        scr.text(pack.font8(), pack.texts()[v.text], 8, 181, st_to_argb(pal[15]), 250);
}

int dump_indices(const Pack& pack, const fs::path& out) {
    fs::create_directories(out);
    auto write = [&](const fs::path& p, const Image& img) {
        std::ofstream f(p, std::ios::binary);
        const uint16_t hdr[4] = {static_cast<uint16_t>(img.width), static_cast<uint16_t>(img.height),
                                 static_cast<uint16_t>(img.x), static_cast<uint16_t>(img.y)};
        f.write(reinterpret_cast<const char*>(hdr), sizeof hdr);
        f.write(reinterpret_cast<const char*>(img.pixels.data()), static_cast<std::streamsize>(img.pixels.size()));
    };
    int n = 0;
    for (const auto& r : pack.rooms()) {
        char name[64];
        std::snprintf(name, sizeof name, "room_%02d.idx", r.id);
        write(out / name, r.image);
        ++n;
        for (size_t k = 0; k < r.sprites.size(); ++k) {
            std::snprintf(name, sizeof name, "room_%02d_sprite_%03zu.idx", r.id, k);
            write(out / name, r.sprites[k]);
            ++n;
        }
    }
    std::printf("%d images écrites dans %s\n", n, out.string().c_str());
    return 0;
}

}  // namespace

// Voix générées (versionnées dans le dépôt, dossier « voix/ » à côté du lancement) ; à défaut, dans le paquet.
fs::path g_pack_dir = "pack";
fs::path voices_dir() { return fs::exists("voix/personnages") ? fs::path("voix/personnages") : g_pack_dir / "voice_chars"; }
fs::path speakers_file() { return fs::exists("voix/speakers.txt") ? fs::path("voix/speakers.txt") : g_pack_dir / "speakers.txt"; }
fs::path missing_voice() {
    // Audio::play essaie aussi l'autre extension (.ogg <-> .wav)
    return fs::exists("voix/voix manquante.ogg") || fs::exists("voix/voix manquante.wav")
               ? fs::path("voix/voix manquante.ogg") : g_pack_dir / "voix manquante.ogg";
}

int main(int argc, char** argv) {
    fs::path pack_dir = "pack";
    int shot_room = -1;
    fs::path shot_file, dump_dir;
    int say = -1, say_speaker = -1;
    bool viewer_only = false;
    std::string son_what;                       // --son : rendu WAV de la musique Atari (contrôle)
    fs::path son_out;
    double son_seconds = 30;   // --viewer : visionneuse seule, sans écran titre ni nouvelle partie
    size_t shot_variant = 0;   // variante de voix affichée dans la barre de la capture
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--pack" && i + 1 < argc) pack_dir = argv[++i];
        else if (a == "--screenshot" && i + 2 < argc) {
            const std::string what = argv[++i];        // n° de lieu, « title » ou « start » (nouvelle partie)
            shot_room = what == "title" ? -2 : what == "start" ? -3 : what == "musiques" ? -4
                      : what == "bruitages" ? -5
                      : what.rfind("page", 0) == 0 ? -10 - std::stoi(what.substr(4))   // page4 ... page8
                      : what == "partie" ? -6                                          // logique d'origine
                      : std::stoi(what);
            shot_file = argv[++i];
        }
        else if (a == "--dump-indices" && i + 1 < argc) dump_dir = argv[++i];
        else if (a == "--say" && i + 1 < argc) say = std::stoi(argv[++i]);
        else if (a == "--speaker" && i + 1 < argc) say_speaker = std::stoi(argv[++i]);
        else if (a == "--viewer") viewer_only = true;
        else if (a == "--son" && i + 2 < argc) {        // --son titre|N|bN fichier.wav [secondes]
            son_what = argv[++i];
            son_out = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') son_seconds = std::stod(argv[++i]);
        }
        else if (a == "--variant" && i + 1 < argc) shot_variant = std::stoi(argv[++i]);
    }
    g_pack_dir = pack_dir;
    if (!son_what.empty()) return render_atari_wav(pack_dir / "atari", son_what, son_out, son_seconds);
    for (int i = 1; i + 2 < argc; ++i)
        if (std::string(argv[i]) == "--logique") {           // trace des N premiers services de la logique PC
            const long long n = std::stoll(argv[i + 1]);
            std::ofstream out(argv[i + 2]);
            logic::Host host(pack_dir / "pc");
            if (!host.ok()) { std::fprintf(stderr, "%s\n", host.error().c_str()); return 1; }
            host.random_mode = 1;                            // hasard figé : comparable à l'oracle
            long long count = 0;
            host.on_trace = [&](int svc, const std::vector<uint16_t>& w) {
                if (++count > n) { host.cpu().stop = true; return; }
                out << svc;
                for (uint16_t x : w) { char b[8]; std::snprintf(b, sizeof b, " %04X", x); out << b; }
                out << '\n';
            };
            if (!host.boot()) { std::fprintf(stderr, "%s\n", host.error().c_str()); return 1; }
            while (count < n && host.cpu().steps < 400'000'000ULL) {
                host.run(1'000'000);
                if (host.waiting()) break;
            }
            std::printf("%lld services, %llu instructions\n", std::min(count, n),
                        static_cast<unsigned long long>(host.cpu().steps));
            return 0;
        }
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--anims") {             // diagnostic : animations chargées par lieu
            const Pack pk(pack_dir);
            for (size_t r = 0; r < pk.rooms().size(); ++r) {
                std::printf("lieu %zu (archive %d) :", r + 1, pk.rooms()[r].atari_archive);
                for (const auto& a : pk.rooms()[r].anims)
                    std::printf(" [%dx%d en %d,%d, %zu images]", a[0].width, a[0].height, a[0].x, a[0].y, a.size());
                std::printf("\n   sprites :");
                for (const auto& sp : pk.rooms()[r].sprites) std::printf(" %dx%d@%d,%d", sp.width, sp.height, sp.x, sp.y);
                std::printf("\n");
            }
            return 0;
        }
    try {
        const Pack pack(pack_dir);
        if (!dump_dir.empty()) return dump_indices(pack, dump_dir);
        Screen scr;
        ViewState v;
        if (shot_room == -6) {                              // partie : écran dessiné par la logique d'origine
            Game game(pack, pack_dir, nullptr);
            if (const char* acc = std::getenv("MAUPITI_ACCEL")) game.time_scale = std::max(1, std::atoi(acc));
            if (!game.start()) throw std::runtime_error(game.error());
            for (int k = 0; k < 40; ++k) game.update(50);
            // clics simulés : MAUPITI_CLICKS="x,y[,bouton];..." ; une capture par clic (fichier_1.bmp, …)
            if (const char* clicks = std::getenv("MAUPITI_CLICKS")) {
                std::string list = clicks;
                int k = 0;
                for (size_t pos = 0; pos < list.size();) {
                    const size_t end = std::min(list.find(';', pos), list.size());
                    int x = 0, y = 0, b = 1;
                    std::sscanf(list.substr(pos, end - pos).c_str(), "%d,%d,%d", &x, &y, &b);
                    game.click(x, y, b);
                    const char* ms = std::getenv("MAUPITI_CLICK_MS");
                    for (int t = 0; t < (ms ? std::atoi(ms) : 2000) / 50; ++t) game.update(50);
                    game.render(scr);
                    fs::path f = shot_file;
                    f.replace_filename(shot_file.stem().string() + "_" + std::to_string(++k) + ".bmp");
                    scr.save_bmp(f);
                    pos = end + 1;
                }
            }
            if (const char* secs = std::getenv("MAUPITI_ATTENTE")) {   // temps de jeu écoulé pendant N s réelles
                const int n = std::atoi(secs);
                for (int t = 0; t <= n; ++t) {
                    if (t % 10 == 0) {
                        if (std::getenv("MAUPITI_CAPTURES")) {      // une capture toutes les 10 s
                            game.render(scr);
                            fs::path f = shot_file;
                            f.replace_filename(shot_file.stem().string() + "_t" + std::to_string(t) + ".bmp");
                            scr.save_bmp(f);
                        }
                        const auto st = game.state();
                        if (std::getenv("MAUPITI_DEBUG"))
                            std::printf("    cpu %04X:%04X bloqué %d\n", game.host().cpu().s[x86::CS],
                                        game.host().cpu().ip, game.blocked() ? 1 : 0);
                        std::printf("%3d s réelles : lieu %d, heure %d, dizaines de minutes %d, forme %d\n", t, st[0], st[1], st[2], st[3]);
                    }
                    for (int k = 0; k < 20; ++k) game.update(50);
                }
            }
            game.render(scr);
            scr.save_bmp(shot_file);
            for (const auto& [svc, cnt] : game.unknown_services) std::printf("service %d non écrit : %d appels\n", svc, cnt);
            return 0;
        }
        if (shot_room <= -14 && shot_room >= -18) {         // pages du navigateur de médias (F4-F8)
            if (SDL_Init(0) != 0) throw std::runtime_error(SDL_GetError());
            AtariMusic music(pack_dir / "atari", pack);
            MediaBrowser mb(pack, pack_dir / "atari", music);
            mb.draw(scr, pack.font8(), -10 - shot_room - 1);
            scr.save_bmp(shot_file);
            SDL_Quit();
            return 0;
        }
        if (shot_room == -4 || shot_room == -5) {         // pages du juke-box
            if (SDL_Init(0) != 0) throw std::runtime_error(SDL_GetError());
            AtariMusic music(pack_dir / "atari", pack);
            Jukebox jb(music);
            jb.draw(scr, pack.font8(), shot_room == -4 ? 1 : 2);
            scr.save_bmp(shot_file);
            SDL_Quit();
            return 0;
        }
        if (shot_room == -2 || shot_room == -3) {
            if (shot_room == -2) v.title = true;
            else start_game(v, pack);
            compose(scr, pack, v);
            scr.save_bmp(shot_file);
            return 0;
        }
        if (shot_room >= 0) {
            v.room = shot_room;
            compose(scr, pack, v);
            if (const auto vars = load_voice_variants(pack_dir); !vars.empty()) {
                const auto& vv = vars[std::min<size_t>(shot_variant, vars.size() - 1)];
                Toolbar tb;
                tb.draw(scr, pack.font8(), 1379, vv.voice, vv.style);
            }
            scr.save_bmp(shot_file);
            return 0;
        }

        if (say >= 0) {
            if (SDL_Init(0) != 0) throw std::runtime_error(SDL_GetError());
            Audio audio;
            char name[16];
            std::snprintf(name, sizeof name, "%04d.wav", say);
            // voix d'un personnage (--speaker C, défaut : le premier qui peut la dire), sinon ancienne voix, sinon secours
            bool ok = false;
            if (say >= 1138 && say <= 1587 && fs::exists(voices_dir())) {
                static const char* names[9] = {"maguy", "sue", "anita", "bruce", "roy", "chris", "bob", "anton", "juste"};
                int c = say_speaker;
                if (c < 0) {
                    std::ifstream f(speakers_file());
                    std::string line;
                    while (std::getline(f, line))
                        if (std::stoi(line.substr(0, line.find(';'))) == say - 1138) {
                            c = std::stoi(line.substr(line.find(';') + 1));
                            break;
                        }
                }
                if (c >= 0 && c < 9) {
                    char cn[16];
                    std::snprintf(cn, sizeof cn, "%03d.ogg", say - 1138);
                    ok = audio.play(voices_dir() / (std::to_string(c) + "_" + names[c]) / cn);
                }
                if (!ok) ok = audio.play(missing_voice());
            } else {
                ok = audio.play(pack_dir / "voice" / name);
            }
            if (!ok) throw std::runtime_error("pas de voix pour ce texte");
            while (audio.playing()) SDL_Delay(50);
            SDL_Delay(300);
            SDL_Quit();
            return 0;
        }
        if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
        SDL_Window* win = SDL_CreateWindow("Maupiti Island", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                           kScreenW * 3, kScreenH * 3 * 6 / 5, SDL_WINDOW_RESIZABLE);
        SDL_Renderer* ren = SDL_CreateRenderer(win, -1,
                                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
        if (!win || !ren) throw std::runtime_error(SDL_GetError());
        // Mise à l'échelle en deux temps : agrandissement entier sans lissage (pixels nets),
        // puis étirement 4:3 avec lissage léger (évite des lignes de hauteurs inégales).
        constexpr int kPre = 5;
        SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kScreenW, kScreenH);
        SDL_Texture* big = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
                                             kScreenW * kPre, kScreenH * kPre);
        if (!tex || !big) throw std::runtime_error(SDL_GetError());
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
        SDL_SetTextureScaleMode(big, SDL_ScaleModeLinear);

        Audio audio;
        // musique et bruitages Atari : pilote d'origine (pack/atari), sortie audio séparée des voix
        AtariMusic music(pack_dir / "atari", pack);
        if (!music.ok()) std::fprintf(stderr, "Son Atari indisponible : %s\n", music.error().c_str());
        Jukebox jukebox(music);
        MediaBrowser media(pack, pack_dir / "atari", music);
        int page = 0;   // 0 lieux (visionneuse), 1 musiques, 2 bruitages, 3 sons, 4 sprites, 5 personnages, 6 objets, 7 documents
        // Variantes de voix (tools/tts/variants.py) : voix dans l'ordre, et pour chaque voix
        // ses propres styles (ils dépendent du moteur de synthèse).
        const auto variants = load_voice_variants(pack_dir);
        std::vector<std::string> voice_names;
        for (const auto& vv : variants)
            if (index_of(voice_names, vv.voice) < 0) voice_names.push_back(vv.voice);
        auto styles_of = [&](int voice_idx) {
            std::vector<const VoiceVariant*> out;
            for (const auto& vv : variants)
                if (vv.voice == voice_names[voice_idx]) out.push_back(&vv);
            return out;
        };
        auto current = [&]() -> const VoiceVariant* {
            if (voice_names.empty()) return nullptr;
            const auto s = styles_of(v.voice_idx);
            return s[std::min<size_t>(v.style_idx, s.size() - 1)];
        };
        // Voix des personnages (tools/tts/generate_characters.py) : pack/speakers.txt donne, pour chaque
        // réplique (texte 1138 + k), les personnages qui peuvent la dire ; un fichier par couple dans
        // voix/personnages/<n>_<nom>/<k>.ogg (ou .wav). Sans fichier : son de secours « voix manquante ».
        static const char* kNames[9] = {"Maguy", "Sue", "Anita", "Bruce", "Roy", "Chris", "Bob", "Anton", "Juste"};
        std::map<int, std::vector<int>> who;
        {
            std::ifstream f(speakers_file());
            std::string line;
            while (std::getline(f, line)) {
                const auto semi = line.find(';');
                if (semi == std::string::npos) continue;
                std::vector<int> cs;
                std::istringstream ss(line.substr(semi + 1));
                std::string tok;
                while (std::getline(ss, tok, ',')) cs.push_back(std::stoi(tok));
                who[std::stoi(line.substr(0, semi))] = cs;
            }
        }
        bool chars_mode = !who.empty() && fs::exists(voices_dir());
        int speaker_pos = 0, last_text = -1;
        auto speakers_of = [&](int text) -> const std::vector<int>* {
            auto it = who.find(text - 1138);
            return (text >= 1138 && it != who.end()) ? &it->second : nullptr;
        };
        auto char_voice = [&](int text) -> fs::path {      // fichier du personnage choisi pour ce texte
            const auto* sp = speakers_of(text);
            if (!sp || sp->empty()) return {};
            const int c = (*sp)[std::min<size_t>(speaker_pos, sp->size() - 1)];
            std::string dir = std::to_string(c) + "_" + kNames[c];
            for (auto& ch : dir) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            char name[16];
            std::snprintf(name, sizeof name, "%03d.ogg", text - 1138);
            return voices_dir() / dir / name;
        };
        if (chars_mode) v.text = 1138;
        else if (!variants.empty()) v.text = 1379;   // textes de démonstration du POC
        auto speak = [&](int text) {
            if (chars_mode) {
                if (text < 1138 || text > 1587) return false;     // interface, narrateur... : muet
                const fs::path f = char_voice(text);
                if (!f.empty() && audio.play(f)) return true;
                return audio.play(missing_voice());
            }
            char name[16];
            std::snprintf(name, sizeof name, "%04d.wav", text);
            bool ok = current() ? audio.play(pack_dir / "voices" / current()->dir / name)
                                : audio.play(pack_dir / "voice" / name);
            // réplique parlée (textes 1138-1587) sans fichier de voix : son de secours
            if (!ok && text >= 1138 && text <= 1587) ok = audio.play(missing_voice());
            return ok;
        };
        int spoken = -1;
        bool play_now = false;
        Toolbar toolbar;
        SDL_Rect dst{0, 0, 1, 1};
        auto apply = [&](Action a) {
            const int nv = std::max<int>(1, static_cast<int>(voice_names.size()));
            const int ns = voice_names.empty() ? 1 : static_cast<int>(styles_of(v.voice_idx).size());
            const int nt = static_cast<int>(pack.texts().size());
            // en changeant de voix, garder le même style s'il existe pour la nouvelle voix
            auto set_voice = [&](int idx) {
                const std::string style = current() ? current()->style : std::string();
                v.voice_idx = idx;
                const auto s = styles_of(idx);
                v.style_idx = 0;
                for (size_t k = 0; k < s.size(); ++k)
                    if (s[k]->style == style) v.style_idx = static_cast<int>(k);
                play_now = true;
            };
            switch (a) {
            case Action::TextPrev: v.text = (v.text + nt - 1) % nt; break;
            case Action::TextNext: v.text = (v.text + 1) % nt; break;
            case Action::VoicePrev:
            case Action::VoiceNext:
                if (chars_mode) {                                  // personnage suivant/précédent pour ce texte
                    if (const auto* sp = speakers_of(v.text); sp && !sp->empty()) {
                        const int n = static_cast<int>(sp->size());
                        speaker_pos = (speaker_pos + (a == Action::VoiceNext ? 1 : n - 1)) % n;
                        play_now = true;
                    }
                } else if (!voice_names.empty()) {
                    set_voice((v.voice_idx + (a == Action::VoiceNext ? 1 : nv - 1)) % nv);
                }
                break;
            case Action::StylePrev: v.style_idx = (v.style_idx + ns - 1) % ns; play_now = true; break;
            case Action::StyleNext: v.style_idx = (v.style_idx + 1) % ns; play_now = true; break;
            case Action::Play: play_now = true; break;
            case Action::None: break;
            }
        };

        const int nrooms = static_cast<int>(pack.rooms().size());
        const int ntexts = static_cast<int>(pack.texts().size());
        bool running = true, dirty = true, fullscreen = false;

        // Démarrage du jeu (comme l'original : maup.exe passe la main à la logique, qui lance une
        // nouvelle partie — service 40, lecture de SAUP.MAU — dessine le lieu de départ et l'interface,
        // fait un fondu (0x25D5) puis lance la musique du lieu). L'écran titre Atari précède.
        bool waiting_title = !viewer_only && !pack.start_state().empty();
        Uint32 fade_start = 0;                     // 0 = pas de fondu en cours
        if (waiting_title) {
            v.title = true;
            v.toolbar = false;
            music.title();                         // musique du titre, comme l'intro Atari
            spoken = v.text;                       // écran titre : aucune réplique lue
        }
        // partie : la logique d'origine (version PC) pilote l'écran (src/game.cpp) ; à défaut, démarrage simplifié
        std::unique_ptr<Game> game;
        Uint32 game_clock = 0;
        bool fresh_game = false;                    // touche N sur l'écran titre : nouvelle partie
        auto new_game = [&]() {
            game = std::make_unique<Game>(pack, pack_dir, &music);
            game->load_save = !fresh_game;
            // voix des personnages : le service 30 donne la hauteur de voix (w[2], signée) ; on prend le
            // locuteur possible de cette réplique qui a cette hauteur (table de maup.exe, ordre du menu)
            game->on_voice = [&](int k, const std::vector<uint16_t>& w) {
                static const int kPitch[9] = {3, 3, 2, -1, 0, -2, -2, -1, -3};
                const int pitch = w.size() > 2 ? static_cast<int16_t>(w[2]) : 99;
                const auto* sp = speakers_of(1138 + k);
                fs::path f;
                if (sp && !sp->empty()) {
                    int c = (*sp)[0];
                    for (int cand : *sp)
                        if (cand >= 0 && cand < 9 && kPitch[cand] == pitch) { c = cand; break; }
                    // personnage interrogé (service 26) : prioritaire s'il peut dire la réplique
                    for (int cand : *sp)
                        if (cand == game->interlocutor_ && (pitch == 99 || kPitch[cand] == pitch)) c = cand;
                    std::string dir = std::to_string(c) + "_" + kNames[c];
                    for (auto& ch : dir) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    char name[16];
                    std::snprintf(name, sizeof name, "%03d.ogg", k);
                    f = voices_dir() / dir / name;
                }
                if (f.empty() || !audio.play(f)) audio.play(missing_voice());
            };
            game->voice_busy = [&]() { return audio.playing(); };
            if (!game->ok() || !game->start()) {
                std::fprintf(stderr, "Logique d'origine indisponible : %s\n", game->error().c_str());
                game.reset();
                start_game(v, pack);
                if (const int bank = music.bank_for_room(v.room); bank >= 0) music.music(bank);
            }
            v.title = false;
            v.toolbar = false;
            game_clock = SDL_GetTicks();
            waiting_title = false;
            fade_start = SDL_GetTicks() | 1;
            spoken = v.text;                                       // texte d'interface : muet
            dirty = true;
        };
        while (running) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) running = false;
                const bool fkey = e.type == SDL_KEYDOWN && e.key.keysym.sym >= SDLK_F1 && e.key.keysym.sym <= SDLK_F8;
                if (fkey) {                                    // pages du navigateur de médias
                    page = static_cast<int>(e.key.keysym.sym - SDLK_F1);
                    dirty = true;
                    continue;
                }
                if (page > 0 && e.type == SDL_KEYDOWN && e.key.keysym.sym != SDLK_ESCAPE &&
                    e.key.keysym.sym != SDLK_F11) {
                    if (MediaBrowser::handles(page) ? media.key(e.key.keysym.sym, page)
                                                    : jukebox.key(e.key.keysym.sym, page))
                        dirty = true;
                    continue;
                }
                if (waiting_title && page == 0 && e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_n) fresh_game = true;
                if (waiting_title && page == 0 &&
                    ((e.type == SDL_KEYDOWN && e.key.keysym.sym != SDLK_ESCAPE &&
                      e.key.keysym.sym != SDLK_F11 && !(e.key.keysym.mod & KMOD_ALT)) ||
                     e.type == SDL_MOUSEBUTTONDOWN)) {
                    new_game();
                    continue;
                }
                if (game && page == 0) {                       // partie : clics transmis à la logique
                    if (e.type == SDL_MOUSEBUTTONDOWN) {
                        int winw = 1, winh = 1, outw = 1, outh = 1;
                        SDL_GetWindowSize(win, &winw, &winh);
                        SDL_GetRendererOutputSize(ren, &outw, &outh);
                        const double px = e.button.x * static_cast<double>(outw) / winw;
                        const double py = e.button.y * static_cast<double>(outh) / winh;
                        const int sx = static_cast<int>((px - dst.x) * kScreenW / dst.w);
                        const int sy = static_cast<int>((py - dst.y) * kScreenH / dst.h);
                        if (sx >= 0 && sy >= 0 && sx < kScreenW && sy < kScreenH)
                            game->click(sx, sy, e.button.button == SDL_BUTTON_LEFT ? 1 : 2);
                        continue;
                    }
                    if (e.type == SDL_KEYDOWN && e.key.keysym.sym != SDLK_ESCAPE && e.key.keysym.sym != SDLK_F11 &&
                        e.key.keysym.sym != SDLK_RETURN)
                        continue;
                }
                if (e.type == SDL_KEYDOWN) {
                    dirty = true;
                    switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE: running = false; break;
                    case SDLK_RIGHT: v.room = (v.room + 1) % nrooms; break;
                    case SDLK_LEFT: v.room = (v.room + nrooms - 1) % nrooms; break;
                    case SDLK_1: v.lighting = 0; break;
                    case SDLK_2: v.lighting = 1; break;
                    case SDLK_3: v.lighting = 2; break;
                    case SDLK_UP: v.text = (v.text + ntexts - 1) % ntexts; break;
                    case SDLK_DOWN: v.text = (v.text + 1) % ntexts; break;
                    case SDLK_s: v.sprites = (v.sprites + 1) % 4; break;
                    case SDLK_n: v.anims = !v.anims; break;
                    case SDLK_t: v.title = !v.title; break;
                    case SDLK_a: v.aspect = !v.aspect; break;
                    case SDLK_v: v.voice = !v.voice; if (!v.voice) audio.stop(); spoken = -1; break;
                    case SDLK_r: spoken = -1; break;
                    case SDLK_SPACE: apply(Action::Play); break;
                    case SDLK_TAB: apply((e.key.keysym.mod & KMOD_SHIFT) ? Action::VoicePrev : Action::VoiceNext); break;
                    case SDLK_e: apply(Action::StyleNext); break;
                    case SDLK_h: v.toolbar = !v.toolbar; break;
                    case SDLK_b:   // voix des personnages <-> variantes de test (POC)
                        if (!who.empty() && !variants.empty() && fs::exists(voices_dir())) {
                            chars_mode = !chars_mode;
                            v.text = chars_mode ? 1138 : 1379;
                            speaker_pos = 0;
                            spoken = -1;
                        }
                        break;
                    case SDLK_PAGEUP: v.text = (v.text + ntexts - 10) % ntexts; break;
                    case SDLK_PAGEDOWN: v.text = (v.text + 10) % ntexts; break;
                    case SDLK_F11:
                    case SDLK_RETURN:
                        if (e.key.keysym.sym == SDLK_F11 || (e.key.keysym.mod & KMOD_ALT)) {
                            fullscreen = !fullscreen;
                            SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                        }
                        break;
                    default: dirty = false; break;
                    }
                }
                if (e.type == SDL_WINDOWEVENT) dirty = true;
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT &&
                    (page > 0 || (v.toolbar && !v.title))) {
                    // fenêtre -> pixels du rendu -> écran logique 320x200
                    int winw = 1, winh = 1, outw = 1, outh = 1;
                    SDL_GetWindowSize(win, &winw, &winh);
                    SDL_GetRendererOutputSize(ren, &outw, &outh);
                    const double px = e.button.x * static_cast<double>(outw) / winw;
                    const double py = e.button.y * static_cast<double>(outh) / winh;
                    const int sx = static_cast<int>((px - dst.x) * kScreenW / dst.w);
                    const int sy = static_cast<int>((py - dst.y) * kScreenH / dst.h);
                    if (page > 0) {
                        if (MediaBrowser::handles(page) ? media.click(sx, sy, page) : jukebox.click(sx, sy, page))
                            dirty = true;
                        continue;
                    }
                    const Action a = toolbar.hit(sx, sy);
                    if (a != Action::None) { apply(a); dirty = true; }
                }
            }
            if (v.text != last_text) { speaker_pos = 0; last_text = v.text; }
            if (play_now || (v.voice && spoken != v.text)) {
                if (!speak(v.text)) audio.stop();   // pas de voix pour ce texte
                spoken = v.text;
                play_now = false;
            }
            if (page == 0 && v.anims && !v.title && !pack.rooms()[v.room].anims.empty()) {   // animations : 10 images/s
                static Uint32 last_anim = 0;
                if (SDL_GetTicks() - last_anim >= 100) {
                    last_anim = SDL_GetTicks();
                    ++v.anim_tick;
                    dirty = true;
                }
            }
            if (game && page == 0) {                           // partie : horloge, tranche de logique, affichage
                const Uint32 now = SDL_GetTicks();
                game->update(now - game_clock);
                game_clock = now;
                if (game->changed() || dirty) {
                    game->render(scr);
                    SDL_UpdateTexture(tex, nullptr, scr.pixels().data(), kScreenW * 4);
                    SDL_SetWindowTitle(win, "Maupiti Island");
                    dirty = false;
                }
            }
            if (page > 0 && (MediaBrowser::handles(page) ? media.tick(page) : jukebox.tick())) dirty = true;
            if (dirty && page > 0) {
                if (MediaBrowser::handles(page)) media.draw(scr, pack.font8(), page);
                else jukebox.draw(scr, pack.font8(), page);
                SDL_UpdateTexture(tex, nullptr, scr.pixels().data(), kScreenW * 4);
                SDL_SetWindowTitle(win, "Maupiti Island — navigateur de médias (Atari)");
                dirty = false;
            }
            if (dirty && !(game && page == 0)) {
                compose(scr, pack, v);
                std::pair<std::string, std::string> chars_label{current() ? current()->voice : "voix unique",
                                                                current() ? current()->style : "-"};
                if (chars_mode) {
                    const auto* sp = speakers_of(v.text);
                    if (sp && !sp->empty()) {
                        const int c = (*sp)[std::min<size_t>(speaker_pos, sp->size() - 1)];
                        const fs::path f = char_voice(v.text);
                        chars_label = {std::string(kNames[c]) + " (" + std::to_string(speaker_pos + 1) + "/" +
                                           std::to_string(sp->size()) + ")",
                                       fs::exists(f) ? "Qwen" : "manquante"};
                    } else {
                        chars_label = {v.text >= 1138 && v.text <= 1587 ? "sans locuteur" : "muet", "-"};
                    }
                }
                if (v.toolbar && !v.title)
                    toolbar.draw(scr, pack.font8(), v.text,
                                 chars_label.first, chars_label.second);
                SDL_UpdateTexture(tex, nullptr, scr.pixels().data(), kScreenW * 4);
                char title[256];
                const auto& r = pack.rooms()[v.room];
                std::snprintf(title, sizeof title, "Maupiti Island — lieu %d/%d (Atari arc%02d, PC %s) — palette %c — texte %d — voix %s",
                              v.room + 1, nrooms, r.atari_archive, r.pc_node.c_str(), "ABC"[v.lighting], v.text,
                              v.voice ? (audio.ok() ? "oui" : "indisponible") : "non");
                SDL_SetWindowTitle(win, title);
                dirty = false;
            }
            SDL_SetRenderTarget(ren, big);
            SDL_RenderCopy(ren, tex, nullptr, nullptr);
            SDL_SetRenderTarget(ren, nullptr);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            int ww = 0, wh = 0;
            SDL_GetRendererOutputSize(ren, &ww, &wh);
            // proportions affichées : 4:3 (pixels 1:1,2 comme sur moniteur) ou pixels carrés (16:10)
            const double ratio = v.aspect ? 4.0 / 3.0 : static_cast<double>(kScreenW) / kScreenH;
            int dw = ww, dh = static_cast<int>(ww / ratio);
            if (dh > wh) { dh = wh; dw = static_cast<int>(wh * ratio); }
            dst = SDL_Rect{(ww - dw) / 2, (wh - dh) / 2, dw, dh};
            if (fade_start) {                       // fondu d'ouverture (1 s)
                const Uint32 t = SDL_GetTicks() - fade_start;
                const Uint8 k = t >= 1000 ? 255 : static_cast<Uint8>(t * 255 / 1000);
                SDL_SetTextureColorMod(big, k, k, k);
                if (t >= 1000) fade_start = 0;
            }
            SDL_RenderCopy(ren, big, nullptr, &dst);
            SDL_RenderPresent(ren);
        }
        SDL_DestroyTexture(big);
        SDL_DestroyTexture(tex);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Erreur : %s\n", ex.what());
        return 1;
    }
    return 0;
}
