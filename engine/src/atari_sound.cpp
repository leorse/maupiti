#include "atari_sound.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>

#include "m68k.h"

namespace maupiti {
namespace {

constexpr double kCpuHz = 8010613.0;               // 68000 d'un ST PAL
constexpr double kMfpHz = 2457600.0;               // horloge des timers du MFP 68901
constexpr double kVblCycles = kCpuHz / 50.0533;    // trame PAL
constexpr int kPrescale[8] = {0, 4, 10, 16, 50, 64, 100, 200};
constexpr unsigned kMemSize = 0x400000;
// petit programme hôte, hors de la mémoire du jeu
constexpr unsigned kIdle = 0x3F0000;               // stop #$2300 ; bra.s kIdle
constexpr unsigned kVbl = 0x3F0010;                // gestionnaire VBL : appelle le tic du pilote ($168)
constexpr unsigned kLoader = 0x3F0040;             // remplace le chargeur de disquette ($B6DC)
constexpr unsigned kStack = 0x3EF000;
constexpr unsigned kDriver = 0x138;                // archive 2 : pilote son
// niveaux de sortie de la puce (utilisés seulement pour un triplet de volumes absent de la table du jeu)
constexpr double kYmLevel[16] = {0.0,    0.0137, 0.0205, 0.0291, 0.0423, 0.0618, 0.0847, 0.1369,
                                 0.1691, 0.2647, 0.3527, 0.4499, 0.5704, 0.6873, 0.8482, 1.0};

AtariSound* g_sound = nullptr;

}  // namespace
}  // namespace maupiti

// --- rappels de Musashi (adresses déjà ramenées à 24 bits par le cœur 68000)
extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return maupiti::g_sound->read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) {
    return (maupiti::g_sound->read8(a) << 8) | maupiti::g_sound->read8(a + 1);
}
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { maupiti::g_sound->write8(a, v & 0xFF); }
void m68k_write_memory_16(unsigned int a, unsigned int v) {
    maupiti::g_sound->write8(a, (v >> 8) & 0xFF);
    maupiti::g_sound->write8(a + 1, v & 0xFF);
}
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v & 0xFFFF);
}
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
int atari_int_ack(int level) { return maupiti::g_sound->int_ack(level); }
void atari_instruction_hook(unsigned int pc) { maupiti::g_sound->instruction(pc); }
}

namespace maupiti {

namespace {
// état de la sortie, partagé entre render() et les écritures YM faites pendant m68k_execute()
struct Output {
    int16_t* out = nullptr;
    int n = 0, pos = 0;
};
Output g_out;
double g_slice_start = 0;
bool g_irq_dirty = false;
}  // namespace

AtariSound::AtariSound(const std::filesystem::path& dir) : dir_(dir), mem_(kMemSize) {
    timers_[0] = Timer{0x19, 0x1F, 0x07, 5, 13};
    timers_[1] = Timer{0x1B, 0x21, 0x07, 0, 8};
    timers_[2] = Timer{0x1D, 0x23, 0x09, 5, 5};
    timers_[3] = Timer{0x1D, 0x25, 0x09, 4, 4};
    std::ifstream f(dir / "map.bin", std::ios::binary);
    map_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (map_.empty()) { error_ = "table du chargeur absente (" + (dir / "map.bin").string() + ")"; return; }
    for (int arc : {0, 2, 12, 19, 60, 61})
        if (!load_size(arc, 0)) { error_ = "archive Atari " + std::to_string(arc) + " absente de " + dir.string(); return; }
    g_sound = this;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    ok_ = true;
}

AtariSound::~AtariSound() {
    if (g_sound == this) g_sound = nullptr;
}

size_t AtariSound::load_size(int arc, int entry) const {
    char name[64];
    std::snprintf(name, sizeof name, "arc%02d/entry%03d.bin", arc, entry);
    std::error_code ec;
    const auto n = std::filesystem::file_size(dir_ / name, ec);
    return ec ? 0 : static_cast<size_t>(n);
}

void AtariSound::load(int arc, int entry, unsigned dest) {
    char name[64];
    std::snprintf(name, sizeof name, "arc%02d/entry%03d.bin", arc, entry);
    std::ifstream f(dir_ / name, std::ios::binary);
    const std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (dest < kMemSize) std::copy_n(d.begin(), std::min<size_t>(d.size(), kMemSize - dest), mem_.begin() + dest);
}

// --- commandes (fil principal)
namespace {
void poke16(std::vector<uint8_t>& m, unsigned a, unsigned v) { m[a] = v >> 8; m[a + 1] = v & 0xFF; }
void poke32(std::vector<uint8_t>& m, unsigned a, unsigned v) { poke16(m, a, v >> 16); poke16(m, a + 2, v & 0xFFFF); }
unsigned peek32(const std::vector<uint8_t>& m, unsigned a) {
    return (m[a] << 24) | (m[a + 1] << 16) | (m[a + 2] << 8) | m[a + 3];
}
}  // namespace

void AtariSound::title() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    in_game_ = false;
    ambience_tracks_ = 0;
    queue_.push_back({{"reset", 0, 0}, {"init", 0x18A, 0}, {"actif", 0, 0}});
}

void AtariSound::music(int bank, int seq) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    in_game_ = true;
    ambience_tracks_ = 0;
    queue_.push_back({{"reset", 0, 0}, {"init", 0x18A, 0}, {"init", 0x186, 0}, {"init", 0x18E, 0}, {"jeu", 0, 0},
                      {"banque", 0x15C, static_cast<unsigned>(bank)}, {"sequence", 0x164, static_cast<unsigned>(seq)}});
}

void AtariSound::effect(int bank, unsigned call, int duration) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    in_game_ = true;
    ambience_tracks_ = 0;
    std::vector<Call> cmd = {{"reset", 0, 0}, {"init", 0x18A, 0}, {"init", 0x186, 0}, {"init", 0x18E, 0},
                             {"jeu", 0, 0}, {"bruitage", call, static_cast<unsigned>(bank)}};
    if (duration >= 0) cmd.push_back({"duree", 0x13C, static_cast<unsigned>(duration)});
    queue_.push_back(std::move(cmd));
}

void AtariSound::room(int bank, int room_archive) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    in_game_ = true;
    ambience_tracks_ = 0;
    constexpr unsigned kAmbience = 0x380000;
    std::vector<Call> cmd = {{"reset", 0, 0}, {"init", 0x18A, 0}, {"init", 0x186, 0}, {"init", 0x18E, 0},
                             {"jeu", 0, 0}, {"banque", 0x15C, static_cast<unsigned>(bank)}};
    int tracks = 0;
    if (room_archive >= 0) {
        char name[64];
        std::snprintf(name, sizeof name, "arc%02d/entry002.bin", room_archive);
        std::ifstream f(dir_ / name, std::ios::binary);
        char head[4] = {};
        f.read(head, 4);
        if (f) {
            tracks = static_cast<unsigned char>(head[3]);
            cmd.push_back({"charge", kAmbience, static_cast<unsigned>(room_archive << 8 | 2)});
        }
    }
    cmd.push_back({"duree", 0x13C, 0x41});
    if (tracks) cmd.push_back({"ambiance", 0x158, kAmbience});
    cmd.push_back({"fin", 0x154, 0});
    for (int k = 0; k < tracks && k < 6; ++k) cmd.push_back({"piste", 0x138, static_cast<unsigned>(k)});
    queue_.push_back(std::move(cmd));
    // pas de relance automatique : en jeu, chaque animation du lieu redéclenche sa piste à chaque cycle ;
    // tant que les animations ne tournent pas, la relancer dès sa fin fait des boucles courtes qui s'empilent
    ambience_tracks_ = 0;
}

void AtariSound::ambience(int room_archive) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    in_game_ = true;
    ambience_tracks_ = 0;
    constexpr unsigned kAmbience = 0x380000;             // zone libre pour la séquence d'ambiance
    std::vector<Call> cmd = {{"reset", 0, 0}, {"init", 0x18A, 0}, {"init", 0x186, 0}, {"init", 0x18E, 0}, {"jeu", 0, 0},
                             {"charge", kAmbience, static_cast<unsigned>(room_archive << 8 | 2)},
                             {"duree", 0x13C, 0x41}, {"ambiance", 0x158, kAmbience}, {"fin", 0x154, 0}};
    // pistes : 4e octet de l'en-tête ; en jeu, chaque animation du lieu déclenche la sienne ($138, piste k)
    char name[64];
    std::snprintf(name, sizeof name, "arc%02d/entry002.bin", room_archive);
    std::ifstream f(dir_ / name, std::ios::binary);
    char head[4] = {};
    f.read(head, 4);
    const int tracks = f ? static_cast<unsigned char>(head[3]) : 0;
    for (int k = 0; k < tracks && k < 6; ++k) cmd.push_back({"piste", 0x138, static_cast<unsigned>(k)});
    queue_.push_back(std::move(cmd));
    ambience_tracks_ = std::min(tracks, 6);
}

void AtariSound::sample(int n, int rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    in_game_ = true;
    ambience_tracks_ = 0;
    raw_rate_ = rate;
    raw_index_ = n;
    queue_.push_back({{"reset", 0, 0}, {"init", 0x18A, 0}, {"init", 0x186, 0}, {"init", 0x18E, 0}, {"jeu", 0, 0},
                      {"numero", 0x3366, static_cast<unsigned>(n)}, {"direct", 0x102C, 0}});
}

void AtariSound::call(unsigned vector, unsigned d0, unsigned poke_addr, unsigned poke_value) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Call> cmd;
    if (!in_game_) {
        queue_.clear();
        ambience_tracks_ = 0;
        cmd = {{"reset", 0, 0}, {"init", 0x18A, 0}, {"init", 0x186, 0}, {"init", 0x18E, 0}, {"jeu", 0, 0}};
        in_game_ = true;
    }
    if (poke_addr) cmd.push_back({"numero", poke_addr, poke_value});
    cmd.push_back({"appel", vector, d0});
    queue_.push_back(std::move(cmd));
}

void AtariSound::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    in_game_ = false;
    ambience_tracks_ = 0;
    queue_.push_back({{"reset", 0, 0}});
}

bool AtariSound::channel_busy(int ch) const { return ok_ && mem_[0x37A0 + 16 * ch] == 0; }
double AtariSound::seconds() const { return (cycle_ - command_cycle_) / kCpuHz; }

// --- machine
void AtariSound::reset_machine() {
    std::fill(mem_.begin(), mem_.end(), 0);
    std::fill(std::begin(mfp_), std::end(mfp_), 0);
    mfp_[0x17] = 0x40;                                   // vecteurs du MFP à partir de $100
    for (auto& t : timers_) { t.control = t.data = 0; t.next = -1; }
    pending_ = 0;
    ym_sel_ = 0;
    std::fill(std::begin(ym_), std::end(ym_), 0);
    std::fill(std::begin(vol_), std::end(vol_), 0);
    // programme hôte
    const uint8_t idle[] = {0x4E, 0x72, 0x23, 0x00, 0x60, 0xFA};                      // stop #$2300 ; bra.s
    const uint8_t vbl[] = {0x48, 0xE7, 0xFF, 0xFE, 0x20, 0x78, 0x01, 0x68, 0x4E, 0x90,  // movem ; movea.l $168.w,a0 ; jsr (a0)
                           0x4C, 0xDF, 0x7F, 0xFF, 0x4E, 0x73};                         // movem ; rte
    std::copy(std::begin(idle), std::end(idle), mem_.begin() + kIdle);
    std::copy(std::begin(vbl), std::end(vbl), mem_.begin() + kVbl);
    poke16(mem_, kLoader, 0x4E75);                                                      // rts (voir instruction())
    // variables et chargements du démarrage (2 lecteurs, 512 Ko : toute la musique accessible à la demande)
    poke16(mem_, 0xC6, 1);
    poke16(mem_, 0xC8, 0);
    load(0, 0, 0xB66C);                                  // bibliothèque du moteur (tables de volume en $B774)
    poke32(mem_, 0xB6DC, kLoader);
    load(2, 0, kDriver);
    load(3, 0, 0x74C48);
    load(18, 0, 0x501A4);
    load(18, 1, 0x5C4F4);
    load(18, 2, 0x68844);
    poke32(mem_, 0, kStack);
    poke32(mem_, 4, kIdle);
    m68k_pulse_reset();
    poke32(mem_, 0x70, kVbl);
    m68k_set_irq(0);
    in_call_ = false;
    // niveau voulu par le jeu pour chaque triplet de volumes : indice de l'entrée de sa table de mixage
    // un triplet absent de la table (écritures de deux voix entremêlées, quand une interruption en
    // interrompt une autre entre ses 3 écritures) ne dure que quelques microsecondes : on le saute
    level_of_.assign(4096, 0xFFFF);
    const unsigned base = peek32(mem_, 0xB774);
    for (int i = 255; i >= 0; --i)
        level_of_[((mem_[base + i] & 15) << 8) | ((mem_[base + 256 + i] & 15) << 4) | (mem_[base + 512 + i] & 15)] =
            static_cast<uint16_t>(i);
    level_ = 128;
}

double AtariSound::period(const Timer& t) const {
    return kPrescale[t.control & 7] * (t.data ? t.data : 256) * (kCpuHz / kMfpHz);
}

void AtariSound::set_control(Timer& t, uint8_t c) {
    const bool was = t.control >= 1 && t.control <= 7;
    t.control = c;
    if (c >= 1 && c <= 7) {
        if (!was) {
            t.next = g_slice_start + m68k_cycles_run() + period(t);
            m68k_end_timeslice();                        // l'ordonnanceur doit tenir compte de cette échéance
        }
    } else {
        t.next = -1;                                     // arrêt (les autres modes ne servent pas au pilote)
    }
}

unsigned AtariSound::read8(unsigned a) {
    if (a < kMemSize) return mem_[a];
    if (a >= 0xFF8800 && a < 0xFF8900) return ym_[ym_sel_];
    if (a >= 0xFFFA00 && a < 0xFFFA40) return mfp_[a - 0xFFFA00];
    return 0xFF;
}

void AtariSound::write8(unsigned a, unsigned v) {
    if (a < kMemSize) { mem_[a] = static_cast<uint8_t>(v); return; }
    if (a >= 0xFF8800 && a < 0xFF8900) {
        if ((a & 3) == 0) ym_sel_ = v & 15;
        else if ((a & 3) == 2) {
            ym_[ym_sel_] = static_cast<uint8_t>(v);
            if (ym_sel_ >= 8 && ym_sel_ <= 10) {
                vol_[ym_sel_ - 8] = v & 15;
                if (ym_sel_ == 10) emit_level(g_slice_start + m68k_cycles_run());   // le pilote écrit 8, 9, 10
            }
        }
        return;
    }
    if (a >= 0xFFFA00 && a < 0xFFFA40) {
        const unsigned r = a - 0xFFFA00;
        mfp_[r] = static_cast<uint8_t>(v);
        if (r == 0x19) set_control(timers_[0], v & 15);
        else if (r == 0x1B) set_control(timers_[1], v & 15);
        else if (r == 0x1D) { set_control(timers_[2], (v >> 4) & 7); set_control(timers_[3], v & 7); }
        else if (r == 0x1F) timers_[0].data = static_cast<uint8_t>(v);
        else if (r == 0x21) timers_[1].data = static_cast<uint8_t>(v);
        else if (r == 0x23) timers_[2].data = static_cast<uint8_t>(v);
        else if (r == 0x25) timers_[3].data = static_cast<uint8_t>(v);
    }
}

void AtariSound::raise_irq() { m68k_set_irq((pending_ & 15) ? 6 : (pending_ & 16) ? 4 : 0); }

int AtariSound::int_ack(int level) {
    int vec = M68K_INT_ACK_SPURIOUS;
    if (level == 6) {
        for (int k = 0; k < 4; ++k)                      // priorité du MFP : A, B, C, D
            if (pending_ & (1u << k)) {
                pending_ &= ~(1u << k);
                vec = (mfp_[0x17] & 0xF0) + timers_[k].channel;
                break;
            }
    } else if (level == 4) {
        pending_ &= ~16u;
        vec = M68K_INT_ACK_AUTOVECTOR;
    }
    g_irq_dirty = true;                                  // la ligne d'interruption sera mise à jour après
    m68k_end_timeslice();
    return vec;
}

void AtariSound::start_call(const Call& c) {
    if (c.what == "reset") return;
    if (c.what == "charge") { load(static_cast<int>(c.d0 >> 8), static_cast<int>(c.d0 & 0xFF), c.addr); return; }
    if (c.what == "numero") { poke16(mem_, c.addr, c.d0); return; }     // $3366 : n° de l'échantillon
    if (c.what == "actif") { poke16(mem_, 0x17C, 0); return; }          // le démarrage ($73EE4) active le pilote
    if (c.what == "jeu") {
        poke16(mem_, 0x17C, 0);
        mem_[0x37A0] = 1;                                // libère le canal 0 (titre, prioritaire)
        // en jeu, la zone des échantillons du titre (archive 18) devient le tas des instruments
        poke32(mem_, 0x170, 0x501A4);
        poke32(mem_, 0x174, 0x501A4);
        return;
    }
    // appel d'une fonction du pilote (vecteur de son API) depuis la boucle d'attente
    unsigned sp = m68k_get_reg(nullptr, M68K_REG_SP) - 4;
    poke32(mem_, sp, kIdle);
    m68k_set_reg(M68K_REG_SP, sp);
    m68k_set_reg(M68K_REG_D0, c.d0);
    m68k_set_reg(M68K_REG_SR, 0x2300);                   // programme principal : interruptions ouvertes
    m68k_set_reg(M68K_REG_PC, c.what == "direct" ? c.addr : peek32(mem_, c.addr));
    in_call_ = true;
}

void AtariSound::instruction(unsigned pc) {
    if (pc == kLoader) {                                 // chargeur : (requête, entrée, destination, travail)
        const unsigned sp = m68k_get_reg(nullptr, M68K_REG_SP);
        const unsigned ret = peek32(mem_, sp), req = peek32(mem_, sp + 4), entry = peek32(mem_, sp + 8),
                       dest = peek32(mem_, sp + 12);
        const unsigned idx = (req & 0xFF) + 0x1E;
        const int arc = idx < map_.size() ? map_[idx] : -1;
        if (arc >= 0) load(arc, static_cast<int>(entry), dest);
        poke32(mem_, sp + 16, ret);                      // la fonction dépile ses 4 arguments ; rts suit
        m68k_set_reg(M68K_REG_SP, sp + 16);
        m68k_set_reg(M68K_REG_D0, arc >= 0 ? static_cast<unsigned>(load_size(arc, static_cast<int>(entry))) : 0);
        return;
    }
    if (pc != kIdle && pc != kIdle + 4) return;
    // boucle d'attente du programme principal : appel suivant de la commande en cours
    if (in_call_) { in_call_ = false; ++step_; }
    while (!in_call_) {
        if (step_ >= running_.size()) {
            std::lock_guard<std::mutex> lock(mutex_);
            // une commande commençant par une remise à zéro est prise par render(), hors de m68k_execute
            if (queue_.empty() || (!queue_.front().empty() && queue_.front()[0].what == "reset")) return;
            running_ = std::move(queue_.front());
            queue_.pop_front();
            step_ = 0;
        }
        start_call(running_[step_]);
        if (!in_call_) ++step_;
    }
}

// --- sortie
void AtariSound::emit_level(double cycle) {
    // ligne de temps continue : l'échantillon k couvre [base + k*samp, base + (k+1)*samp[
    Output& o = g_out;
    for (;;) {
        const double boundary = out_base_ + static_cast<double>(out_index_ + 1) * out_samp_;
        if (o.pos >= o.n || cycle < boundary) break;
        acc_ += level_ * (boundary - out_last_);
        const double x = acc_ / out_samp_ / 255.0 - 0.5;  // niveau moyen de l'échantillon, centré
        dc_ += (x - dc_) * 0.0002;                       // composante continue (passe-haut ~ 4 Hz)
        o.out[o.pos++] = static_cast<int16_t>(std::clamp((x - dc_) * 52000.0, -32768.0, 32767.0));
        acc_ = 0;
        out_last_ = boundary;
        ++out_index_;
    }
    if (cycle > out_last_) { acc_ += level_ * (cycle - out_last_); out_last_ = cycle; }
    const unsigned lv = level_of_.empty() ? 0xFFFF : level_of_[(vol_[0] << 8) | (vol_[1] << 4) | vol_[2]];
    if (lv != 0xFFFF) level_ = lv;
}

void AtariSound::advance(double target) {
    while (cycle_ < target) {
        double next = std::min(target, next_vbl_);
        for (const auto& t : timers_)
            if (t.next >= 0) next = std::min(next, t.next);
        if (next > cycle_) {
            g_slice_start = cycle_;
            const int ran = m68k_execute(std::max(1, static_cast<int>(std::ceil(next - cycle_))));
            cycle_ += ran;
        }
        bool changed = g_irq_dirty;
        g_irq_dirty = false;
        for (auto& t : timers_) {
            if (t.next < 0 || t.next > cycle_) continue;
            while (t.next <= cycle_) t.next += period(t);
            if (((mfp_[t.ier] >> t.bit) & 1) && ((mfp_[t.ier + 0x0C] >> t.bit) & 1)) {
                pending_ |= 1u << (&t - timers_);
                changed = true;
            }
        }
        if (next_vbl_ <= cycle_) {
            next_vbl_ += kVblCycles;
            pending_ |= 16;
            changed = true;
        }
        if (changed) raise_irq();
    }
}

void AtariSound::render(int16_t* out, int n, int rate) {
    if (!ok_) { std::fill(out, out + n, 0); return; }
    out_samp_ = kCpuHz / rate;
    bool fresh = false;
    {   // nouvelle commande : la machine repart du démarrage
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queue_.empty() && !queue_.front().empty() && queue_.front()[0].what == "reset") {
            running_ = std::move(queue_.front());
            queue_.pop_front();
            step_ = 1;
            fresh = true;
        }
    }
    if (fresh) {
        raw_on_ = false;
        reset_machine();
        next_vbl_ = cycle_ + kVblCycles;
        // initialisation du pilote (décodage des instruments : 1 à 3 s de travail du 68000) exécutée d'avance,
        // sans produire de son : la musique démarre aussitôt
        g_out = Output{nullptr, 0, 0};
        const double limit = cycle_ + 20 * kCpuHz;
        while ((in_call_ || step_ < running_.size()) && cycle_ < limit) advance(cycle_ + kCpuHz / 100);
        command_cycle_ = cycle_;
        out_base_ = cycle_;
        out_index_ = 0;
        out_last_ = cycle_;
        acc_ = 0;
        dc_ = level_ / 255.0 - 0.5;                      // pas de saut au démarrage
        if (!running_.empty() && running_.back().what == "direct" && raw_index_ >= 0) {
            // son numérisé décodé par $102C : table $334A, 16 octets par son (début ; fins des 3 segments)
            const unsigned e = peek32(mem_, 0x334A) + 16u * static_cast<unsigned>(raw_index_);
            const unsigned a = peek32(mem_, e), b = peek32(mem_, e + 12);
            raw_.clear();
            if (a < b && b < kMemSize && b - a < 0x100000)
                for (unsigned k = a; k < b; ++k)
                    if (mem_[k]) raw_.push_back(mem_[k]);   // 0 = fin de segment
            raw_pos_ = 0;
            raw_step_ = static_cast<double>(raw_rate_) / rate;
            raw_on_ = !raw_.empty();
        }
    }
    if (raw_on_) {                                       // son numérisé : lecture directe des octets
        for (int k = 0; k < n; ++k) {
            const size_t i = static_cast<size_t>(raw_pos_);
            out[k] = i < raw_.size() ? static_cast<int16_t>((raw_[i] - 128) * 200) : 0;
            raw_pos_ += raw_step_;
        }
        if (raw_pos_ >= raw_.size()) raw_on_ = false;
        command_cycle_ -= n * out_samp_;                 // le compteur de temps avance
        return;
    }
    g_out = Output{out, n, 0};
    const double target = out_base_ + static_cast<double>(out_index_ + n) * out_samp_;
    advance(target);
    if (ambience_tracks_ > 0 && !in_call_ && step_ >= running_.size()) {
        // en jeu, l'animation redéclenche son bruitage à chaque cycle : ici, dès que la piste est finie
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            std::vector<Call> again;
            for (int k = 0; k < ambience_tracks_; ++k)
                if (mem_[0x37D0 + 16 * k] != 0) again.push_back({"piste", 0x138, static_cast<unsigned>(k)});
            if (!again.empty()) queue_.push_back(std::move(again));
        }
    }
    emit_level(target);                                  // termine le dernier échantillon du tampon
    while (g_out.pos < n) out[g_out.pos++] = 0;          // (ne se produit pas)
}

}  // namespace maupiti
