#include "jukebox.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include "atari_sound.hpp"
#include "ui.hpp"

namespace maupiti {

namespace {
void write_wav(const std::filesystem::path& path, const std::vector<int16_t>& pcm, int rate) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(pcm.size() * 2);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    f.write("data", 4); u32(bytes);
    f.write(reinterpret_cast<const char*>(pcm.data()), bytes);
}
}  // namespace

int render_atari_wav(const std::filesystem::path& sound_dir, const std::string& what,
                     const std::filesystem::path& out, double seconds) {
    AtariSound snd(sound_dir);
    if (!snd.ok()) {
        std::fprintf(stderr, "Son Atari indisponible : %s\n", snd.error().c_str());
        return 1;
    }
    if (what == "titre") snd.title();
    else if (!what.empty() && what[0] == 'b') snd.effect(std::stoi(what.substr(1)));
    else if (!what.empty() && what[0] == 's') snd.sample(std::stoi(what.substr(1)), 9600);
    else if (!what.empty() && what[0] == 'a') snd.ambience(std::stoi(what.substr(1)));
    else if (!what.empty() && what[0] == 'r') {        // rB:A : décor (banque B, ambiance de l'archive A)
        const size_t c = what.find(':');
        snd.room(std::stoi(what.substr(1)), c == std::string::npos ? -1 : std::stoi(what.substr(c + 1)));
        if (const size_t c2 = what.find(':', c + 1); c != std::string::npos && c2 != std::string::npos)
            snd.call(0x164, static_cast<unsigned>(std::stoi(what.substr(c2 + 1))));   // rB:A:S : + séquence S
    }
    else if (!what.empty() && what[0] == 'l') snd.effect(std::stoi(what.substr(1)), 0x144);   // lN : par $144
    else snd.music(std::stoi(what));
    constexpr int kRate = 44100;
    std::vector<int16_t> pcm(static_cast<size_t>(seconds * kRate));
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t pos = 0; pos < pcm.size(); pos += 1024)
        snd.render(pcm.data() + pos, static_cast<int>(std::min<size_t>(1024, pcm.size() - pos)), kRate);
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    write_wav(out, pcm, kRate);
    std::printf("%s : %.0f s de son en %.2f s (%.0f fois le temps réel)\n", out.string().c_str(), seconds, dt,
                seconds / std::max(dt, 1e-6));
    return 0;
}

// --- lecteur
AtariMusic::AtariMusic(const std::filesystem::path& sound_dir, const Pack& pack)
    : snd_(std::make_unique<AtariSound>(sound_dir)) {
    if (!snd_->ok()) { error_ = snd_->error(); return; }
    // musique de chaque lieu : table de l'archive 0 en $BAF0, indexée par le n° de requête du décor
    std::ifstream fm(sound_dir / "map.bin", std::ios::binary);
    const std::vector<uint8_t> map((std::istreambuf_iterator<char>(fm)), std::istreambuf_iterator<char>());
    std::ifstream fa(sound_dir / "arc00" / "entry000.bin", std::ios::binary);
    const std::vector<uint8_t> arc0((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
    constexpr size_t kTable = 0xBAF0 - 0xB66C;
    room_bank_.assign(pack.rooms().size(), -1);
    lib_ = arc0;
    for (size_t r = 0; r < 34 && kTable + r < arc0.size(); ++r) music_table_.push_back(arc0[kTable + r]);
    for (size_t i = 0; i < pack.rooms().size(); ++i)
        for (size_t r = 0; r < 32 && r < map.size() && kTable + r < arc0.size(); ++r)
            if (map[r] == pack.rooms()[i].atari_archive && arc0[kTable + r] < 0x80) {
                room_bank_[i] = arc0[kTable + r];
                break;
            }
    for (size_t i = 0; i < pack.rooms().size(); ++i) {
        char name[64];
        std::snprintf(name, sizeof name, "arc%02d/entry002.bin", pack.rooms()[i].atari_archive);
        std::error_code ec;
        if (std::filesystem::file_size(sound_dir / name, ec) > 4 && !ec)
            ambiences_.push_back({static_cast<int>(i), pack.rooms()[i].atari_archive});
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { error_ = SDL_GetError(); return; }
    SDL_AudioSpec want{}, have{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = &AtariMusic::callback;
    want.userdata = this;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev_) { error_ = SDL_GetError(); return; }
    SDL_PauseAudioDevice(dev_, 0);
}

AtariMusic::~AtariMusic() {
    if (dev_) SDL_CloseAudioDevice(dev_);
}

void AtariMusic::callback(void* self, Uint8* stream, int len) {
    auto* m = static_cast<AtariMusic*>(self);
    m->snd_->render(reinterpret_cast<int16_t*>(stream), len / 2, 44100);
}

void AtariMusic::title() { if (ok()) snd_->title(); }
void AtariMusic::music(int bank) { if (ok()) snd_->music(bank); }
void AtariMusic::effect(int bank, unsigned call, int duration) { if (ok()) snd_->effect(bank, call, duration); }
void AtariMusic::room(int bank, int room_archive) { if (ok()) snd_->room(bank, room_archive); }
void AtariMusic::call(unsigned vector, unsigned d0, unsigned poke_addr, unsigned poke_value) {
    if (ok()) snd_->call(vector, d0, poke_addr, poke_value);
}
void AtariMusic::sample(int n, int rate) { if (ok()) snd_->sample(n, rate); }
void AtariMusic::ambience(int room_archive) { if (ok()) snd_->ambience(room_archive); }
bool AtariMusic::sample_playing() const { return ok() && snd_->sample_playing(); }
void AtariMusic::stop() { if (ok()) snd_->stop(); }
bool AtariMusic::busy(int channel) const { return ok() && snd_->channel_busy(channel); }
double AtariMusic::seconds() const { return ok() ? snd_->seconds() : 0; }
int AtariMusic::bank_for_room(int room) const {
    return room >= 0 && room < static_cast<int>(room_bank_.size()) ? room_bank_[room] : -1;
}
int AtariMusic::music_code(int request) const {
    return request >= 0 && request < static_cast<int>(music_table_.size()) ? music_table_[request] : -1;
}
std::vector<int> AtariMusic::rooms_for_bank(int bank) const {
    std::vector<int> out;
    for (size_t i = 0; i < room_bank_.size(); ++i)
        if (room_bank_[i] == bank) out.push_back(static_cast<int>(i));
    return out;
}

// --- juke-box
namespace {
constexpr uint32_t kBg = 0xFF101828, kBar = 0xFF243048, kInk = 0xFFE0E0E0, kDim = 0xFF8890A0,
                   kSel = 0xFF3050A0, kPlay = 0xFFFFD060;
constexpr int kTop = 16, kRow = 9, kRows = 17;
std::string clock(double s) {
    const int t = static_cast<int>(s);
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}
}  // namespace

Jukebox::Jukebox(AtariMusic& music) : music_(music) {
    musics_.push_back({"Titre (archive 19)", 0, 0});
    for (int b = 0; b <= 41; ++b) {
        std::string label = "Banque " + std::to_string(b);
        const auto rooms = music_.rooms_for_bank(b);
        if (!rooms.empty()) {
            label += " - lieux";
            for (size_t k = 0; k < rooms.size() && k < 8; ++k) label += " " + std::to_string(rooms[k] + 1);
            if (rooms.size() > 8) label += "...";
        }
        if (b == 13) label += " (vide)";
        musics_.push_back({label, 1, b});
    }
    for (const auto& [room, arc] : music_.ambiences())
        effects_.push_back({"Ambiance - lieu " + std::to_string(room + 1) + " (archive " + std::to_string(arc) + ")", 3, arc});
    for (int b = 42; b <= 75; ++b) effects_.push_back({"Bruitage " + std::to_string(b), 2, b});
}

void Jukebox::play(const Entry& e) {
    if (e.kind == 0) music_.title();
    else if (e.kind == 1) music_.music(e.bank);
    else if (e.kind == 3) music_.ambience(e.bank);
    else music_.effect(e.bank);
    playing_ = e.label;
    playing_kind_ = e.kind;
}

int draw_page_header(Screen& scr, const Font& f, int page) {
    static const char* names[kPageCount] = {"Lieux", "Musiques", "Bruitages", "Sons numérisés",
                                            "Sprites", "Personnages", "Objets", "Documents"};
    scr.fill(0, 0, kScreenW, 12, kBar);
    int x = 3;
    for (int p = 0; p < kPageCount; ++p) {                  // F1 à F8 : numéros, page courante en surbrillance
        const auto g = glyphs_from_utf8(std::to_string(p + 1));
        if (p == page) scr.fill(x - 2, 1, text_width(f, g) + 4, 10, kSel);
        scr.text(f, g, x, 2, p == page ? 0xFFFFFFFF : kDim);
        x += text_width(f, g) + 7;
    }
    scr.text(f, glyphs_from_utf8(std::string("F") + std::to_string(page + 1) + " " + names[page]), x + 6, 2, 0xFFFFFFFF);
    return 12;
}

void Jukebox::draw(Screen& scr, const Font& f, int page) const {
    scr.clear(kBg);
    draw_page_header(scr, f, page);
    const auto& l = list(page);
    for (int r = 0; r < kRows; ++r) {
        const int i = top_[page] + r;
        if (i >= static_cast<int>(l.size())) break;
        const int y = kTop + r * kRow;
        if (i == sel_[page]) scr.fill(0, y - 1, kScreenW, kRow, kSel);
        const bool on = l[i].label == playing_;
        scr.text(f, glyphs_from_utf8((on ? "> " : "  ") + l[i].label), 4, y, on ? kPlay : kInk);
    }
    scr.fill(0, 176, kScreenW, 24, kBar);
    if (!music_.ok()) {
        scr.text(f, glyphs_from_utf8("Son Atari indisponible : " + music_.error()), 4, 178, kPlay);
    } else if (!playing_.empty()) {
        const bool on = music_.busy(playing_kind_ == 0 ? 0 : 1) || playing_kind_ >= 2;
        scr.text(f, glyphs_from_utf8(playing_ + "  " + clock(music_.seconds()) + (on ? "" : "  (fin)")), 4, 178, kPlay);
    }
    scr.text(f, glyphs_from_utf8("Haut/Bas choisir - Entrée jouer - Espace stop"), 4, 189, kDim);
}

bool Jukebox::key(SDL_Keycode k, int page) {
    const int n = static_cast<int>(list(page).size());
    int& s = sel_[page];
    switch (k) {
    case SDLK_UP: s = (s + n - 1) % n; break;
    case SDLK_DOWN: s = (s + 1) % n; break;
    case SDLK_PAGEUP: s = std::max(0, s - kRows); break;
    case SDLK_PAGEDOWN: s = std::min(n - 1, s + kRows); break;
    case SDLK_HOME: s = 0; break;
    case SDLK_END: s = n - 1; break;
    case SDLK_RETURN: case SDLK_KP_ENTER: play(list(page)[s]); break;
    case SDLK_SPACE: music_.stop(); playing_.clear(); break;
    default: return false;
    }
    if (s < top_[page]) top_[page] = s;
    if (s >= top_[page] + kRows) top_[page] = s - kRows + 1;
    return true;
}

bool Jukebox::click(int x, int y, int page) {
    (void)x;
    if (y < 12 && page >= 1) return false;
    const int r = (y - kTop + 1) / kRow;
    if (y < kTop - 1 || r < 0 || r >= kRows) return false;
    const int i = top_[page] + r;
    if (i >= static_cast<int>(list(page).size())) return false;
    sel_[page] = i;
    play(list(page)[i]);
    return true;
}

bool Jukebox::tick() {
    const int s = static_cast<int>(music_.seconds());
    if (s == last_second_) return false;
    last_second_ = s;
    return !playing_.empty();
}

}  // namespace maupiti
