#include <algorithm>
#include "pack.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace maupiti {
namespace fs = std::filesystem;

std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("fichier introuvable : " + p.string());
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

namespace {

uint16_t le16(const std::vector<uint8_t>& d, size_t o) {
    if (o + 1 >= d.size()) throw std::runtime_error("fichier tronqué");
    return static_cast<uint16_t>(d[o] | (d[o + 1] << 8));
}

uint16_t be16(const std::vector<uint8_t>& d, size_t o) {
    if (o + 1 >= d.size()) throw std::runtime_error("fichier tronqué");
    return static_cast<uint16_t>((d[o] << 8) | d[o + 1]);
}

Font load_font(const fs::path& p) {
    const auto d = read_file(p);
    if (d.size() < 10 || std::string(d.begin(), d.begin() + 4) != "FNT1") throw std::runtime_error("police invalide");
    Font f;
    f.height = le16(d, 4);
    const int count = le16(d, 6);
    f.atlas_width = le16(d, 8);
    size_t o = 10;
    for (int i = 0; i < count; ++i, o += 6) f.glyphs.push_back({le16(d, o), le16(d, o + 2), le16(d, o + 4)});
    const size_t n = static_cast<size_t>(f.atlas_width) * f.height;
    if (o + n > d.size()) throw std::runtime_error("police tronquée");
    f.bitmap.assign(d.begin() + o, d.begin() + o + n);
    return f;
}

// Animations d'un lieu : entrée 3 de son archive Atari (tools/maupiti/atari/anims.py, docs/architecture.md
// §13). Enregistrements de type 0 : descripteurs (position écran, taille, décalages des images) et une bande
// d'images au format Lankhor.
std::vector<std::vector<Image>> load_anims(const std::vector<uint8_t>& d) {
    std::vector<std::vector<Image>> out;
    auto w = [&](size_t o) -> unsigned { return o + 1 < d.size() ? (d[o] << 8) | d[o + 1] : 0xFFFF; };
    std::vector<size_t> records;
    for (size_t o = 0; o + 1 < d.size(); o += 2) {
        const unsigned v = w(o);
        if (v == 0) break;
        records.push_back(v - 2);
    }
    for (size_t r : records) {
        if (r + 8 > d.size() || w(r) != 0) continue;
        size_t o = r + 6;
        std::vector<size_t> descs;
        while (o + 1 < d.size()) {
            const unsigned v = w(o);
            o += 2;
            if (v & 0x8000) break;
            descs.push_back(o - 2 + v);
        }
        const size_t img_at = o + w(o);
        if (img_at >= d.size()) continue;
        Image strip;
        try {
            strip = decode_image(std::span<const uint8_t>(d).subspan(img_at));
        } catch (const std::exception&) {
            continue;
        }
        // espace de travail du jeu : comme l'écran, 160 octets par ligne ; chaque emplacement reçoit d'abord le
        // décor, puis la bande est décodée par-dessus à sa position (strip.x, strip.y)
        constexpr int row_bytes = 160;
        for (size_t p : descs) {
            while (p + 7 < d.size() && w(p) != 0xFFFF) p += 8;                     // programme
            p += 2;
            while (p + 1 < d.size() && !(w(p) == 0xFFFF && w(p - 2) == 0xFFFF)) p += 2;   // tables
            p += 2;
            if (p + 5 >= d.size()) continue;
            const unsigned scr = w(p), wb = w(p + 2), h = w(p + 4);
            p += 6;
            std::vector<Image> frames;
            for (; p + 1 < d.size() && !(w(p) & 0x8000); p += 2) {
                const unsigned off = w(p);
                Image f;
                f.x = static_cast<int>((scr % 160) / 8 * 16);
                f.y = static_cast<int>(scr / 160);
                f.width = static_cast<int>((wb + 1) * 16);
                f.height = static_cast<int>(h + 1);
                f.transparent = strip.transparent;
                f.pixels.assign(static_cast<size_t>(f.width) * f.height, 16);
                const int fx = static_cast<int>(off % row_bytes) / 8 * 16 - strip.x;
                const int fy = static_cast<int>(off / row_bytes) - strip.y;
                // emplacement hors de la bande : tampon de sauvegarde du fond, pas une image à montrer
                // (l'enchaînement exact est donné par le programme de l'animation, à décoder avec la logique)
                if (fy >= strip.height || fx >= strip.width || fy + static_cast<int>(h) < 0) continue;
                for (int y = 0; y < f.height; ++y)
                    for (int x = 0; x < f.width; ++x) {
                        const int sx = fx + x, sy = fy + y;
                        if (sx >= 0 && sy >= 0 && sx < strip.width && sy < strip.height)
                            f.pixels[static_cast<size_t>(y) * f.width + x] =
                                strip.pixels[static_cast<size_t>(sy) * strip.width + sx];
                    }
                frames.push_back(std::move(f));
            }
            if (!frames.empty()) out.push_back(std::move(frames));
        }
    }
    return out;
}

}  // namespace

Pack::Pack(const fs::path& dir) {
    std::ifstream list(dir / "rooms.txt");
    if (!list) throw std::runtime_error("paquet introuvable : " + (dir / "rooms.txt").string());
    std::string line;
    while (std::getline(list, line)) {
        std::istringstream ss(line);
        Room r;
        if (!(ss >> r.id >> r.atari_archive >> r.pc_node)) continue;
        char name[32];
        std::snprintf(name, sizeof name, "%02d_room.bin", r.id);
        const auto d = read_file(dir / "rooms" / name);
        for (int i = 0; i < 16; ++i) {
            r.pal_a[i] = be16(d, 2 * i);
            r.pal_b[i] = be16(d, 32 + 2 * i);
        }
        r.image = decode_image(std::span<const uint8_t>(d).subspan(64));
        std::snprintf(name, sizeof name, "%02d_sprites.bin", r.id);
        if (fs::exists(dir / "rooms" / name)) {
            const auto s = read_file(dir / "rooms" / name);
            r.sprites = decode_image_run(s);
        }
        std::snprintf(name, sizeof name, "%02d_patch.bin", r.id);
        if (fs::exists(dir / "rooms" / name)) {
            const auto p = read_file(dir / "rooms" / name);
            if (p.size() >= 6 && std::equal(p.begin(), p.begin() + 4, "PTC1")) {
                size_t pos = 6;
                const int n = p[4] | (p[5] << 8);
                for (int k = 0; k < n && pos + 8 <= p.size(); ++k) {
                    auto u16 = [&](size_t o) { return p[o] | (p[o + 1] << 8); };
                    Image im;
                    im.x = r.image.x + u16(pos);
                    im.y = r.image.y + u16(pos + 2);
                    im.width = u16(pos + 4);
                    im.height = u16(pos + 6);
                    pos += 8;
                    const size_t len = static_cast<size_t>(im.width) * im.height;
                    if (pos + len > p.size()) break;
                    im.pixels.assign(p.begin() + pos, p.begin() + pos + len);
                    pos += len;
                    r.patches.push_back(std::move(im));
                }
            }
        }
        std::snprintf(name, sizeof name, "arc%02d/entry003.bin", r.atari_archive);
        if (fs::exists(dir / "atari" / name)) r.anims = load_anims(read_file(dir / "atari" / name));
        rooms_.push_back(std::move(r));
    }
    frame_ = decode_image_run(read_file(dir / "ui" / "frame.bin"));
    menubar_ = decode_image(read_file(dir / "ui" / "menubar.bin"));
    title_ = decode_image(read_file(dir / "ui" / "title.bin"));
    if (std::ifstream lf(dir / "lieux.txt"); lf) {
        int res = 0, room = 0;
        while (lf >> res >> room) lieux_.emplace_back(res, room);
    }
    if (fs::exists(dir / "start.sav")) start_ = read_file(dir / "start.sav");
    font8_ = load_font(dir / "font8.fnt");
    font7_ = load_font(dir / "font7.fnt");
    const auto t = read_file(dir / "texts.bin");
    if (t.size() < 6 || std::string(t.begin(), t.begin() + 4) != "TXT1") throw std::runtime_error("textes invalides");
    const int n = le16(t, 4);
    size_t o = 6;
    for (int i = 0; i < n; ++i) {
        const int len = le16(t, o);
        o += 2;
        if (o + len > t.size()) throw std::runtime_error("textes tronqués");
        texts_.emplace_back(t.begin() + o, t.begin() + o + len);
        o += len;
    }
}

int Pack::room_for_resource(int res) const {
    for (const auto& [r, room] : lieux_)
        if (r == res) return room;
    return -1;
}

}  // namespace maupiti
