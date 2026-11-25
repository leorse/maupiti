#include "host.hpp"

#include <algorithm>
#include <fstream>

namespace maupiti::logic {

namespace {
constexpr uint16_t kEntryCs = 0x0000, kEntryIp = 0xEF37;      // programme principal (en-tête LZEXE)
constexpr uint16_t kStackSs = 0x12BD, kStackSp = 0x2000;
constexpr uint16_t kDataSeg = 0x10F1;
constexpr uint32_t kZoneBase = 0x40000;                        // zone de données rendue par le service 28

std::vector<uint8_t> read_all(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
uint16_t le16(const std::vector<uint8_t>& d, size_t o) { return d[o] | (d[o + 1] << 8); }
uint32_t le32(const std::vector<uint8_t>& d, size_t o) { return le16(d, o) | (static_cast<uint32_t>(le16(d, o + 2)) << 16); }
}  // namespace

Host::Host(const std::filesystem::path& dir) : dir_(dir) {
    image_ = read_all(dir / "maupiti1.img");
    const auto maup = read_all(dir / "maup.img");
    if (image_.empty() || maup.size() < 0xA420 + 0xB00) {
        error_ = "logique PC absente (" + (dir / "maupiti1.img").string() + ", maup.img)";
        return;
    }
    // octets de paramètres de chaque service, lus dans les enveloppes Pascal :
    //   mov ax,n ; mov cx,bp ; add cx,6 ; int 0FEh ; pop bp ; retf [n]
    for (size_t i = 0; i + 14 < image_.size(); ++i) {
        const uint8_t* p = &image_[i];
        if (p[0] == 0xB8 && p[2] == 0x00 && p[3] == 0x8B && p[4] == 0xCD && p[5] == 0x83 && p[6] == 0xC1 &&
            p[7] == 0x06 && p[8] == 0xCD && p[9] == 0xFE && p[10] == 0x5D) {
            if (p[11] == 0xCA) stack_bytes_[p[1]] = p[12] | (p[13] << 8);
            else if (p[11] == 0xCB) stack_bytes_[p[1]] = 0;
        }
    }
    // ressources de maup.exe (segment de données 0xA42 : chargeur 0x3F9F)
    constexpr size_t base = 0xA420;
    int group = 1, rank = 0;
    for (int k = 0; k < 68; ++k) {
        const uint16_t v = le16(maup, base + 0xA44 + 2 * k);
        if (v == 0xFFFF) { ++group; rank = 0; }
        else res_where_[v] = {group, rank++};
    }
    const std::pair<int, int> sizes[7] = {{0, 0}, {0x98C, 21}, {0x9E0, 6}, {0x9F8, 5}, {0xA0C, 6}, {0xA24, 3}, {0xA30, 3}};
    for (int g = 1; g <= 6; ++g)
        for (int k = 0; k < sizes[g].second; ++k) res_nodes_[g].push_back(le32(maup, base + sizes[g].first + 4 * k));
    std::copy(image_.begin(), image_.end(), cpu_.mem.begin());
    cpu_.on_int = [this](x86::Cpu& c, uint8_t n) { on_int(c, n); };
    ok_ = true;
}

std::vector<uint8_t> Host::read_resource(int id, int sub) const {
    const auto it = res_where_.find(id);
    if (it == res_where_.end()) return {};
    const auto [group, rank] = it->second;
    const auto nodes = res_nodes_.find(group);
    if (nodes == res_nodes_.end() || rank >= static_cast<int>(nodes->second.size())) return {};
    std::ifstream f(dir_ / ("G" + std::to_string(group) + ".CON"), std::ios::binary);
    const uint32_t node = nodes->second[rank];
    f.seekg(node + 4 * sub);
    uint8_t h[8] = {};
    f.read(reinterpret_cast<char*>(h), 8);
    const uint16_t a = h[0] | (h[1] << 8), b = h[4] | (h[5] << 8);
    std::vector<uint8_t> out(static_cast<uint16_t>(b - a));
    f.seekg(node + a);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return out;
}

uint32_t Host::load_resource(int id, int sub, uint32_t dest) {
    const auto d = read_resource(id, sub);
    for (size_t i = 0; i < d.size(); ++i) cpu_.wb(dest + static_cast<uint32_t>(i), d[i]);
    if (on_load) on_load(id, sub, dest, static_cast<uint32_t>(d.size()));
    return static_cast<uint32_t>(d.size());
}

bool Host::boot(uint64_t max_steps) {
    if (!ok_) return false;
    cpu_.s[x86::SS] = kStackSs;
    cpu_.r[x86::SP] = kStackSp;
    cpu_.s[x86::DS] = cpu_.s[x86::ES] = kDataSeg;
    cpu_.s[x86::CS] = kEntryCs;
    cpu_.ip = kEntryIp;
    cpu_.run(max_steps);
    if (!resident_) { error_ = "la logique ne s'est pas mise en résident"; return false; }
    // maup.exe reprend la main à l'adresse laissée par la logique (boîte aux lettres 0000:03C6)
    cpu_.s[x86::DS] = cpu_.s[x86::ES] = mailbox_ds_;
    cpu_.s[x86::CS] = resume_cs_;
    cpu_.ip = resume_ip_;
    return true;
}

uint64_t Host::run(uint64_t max_steps) {
    idle_polls_ = 0;
    uint64_t n = 0;
    while (n < max_steps && !waiting()) {
        n += cpu_.run(std::min<uint64_t>(100000, max_steps - n));
        if (cpu_.stop && !waiting()) break;   // arrêt demandé (fin du programme)
    }
    return n;
}

void Host::click(int x, int y, int button) { pending_clicks_.push_back({x, y, button}); idle_polls_ = 0; }

void Host::on_int(x86::Cpu& c, uint8_t n) {
    if (n == 0x21) { dos(); return; }
    if (n != 0xFE) return;                    // autres interruptions : ignorées (comme l'oracle)
    const int svc = c.r[x86::AX];
    const uint16_t cx = c.r[x86::CX];
    const int nbytes = stack_bytes_.count(svc) ? stack_bytes_[svc] : 0;
    std::vector<uint16_t> w(static_cast<size_t>(nbytes / 2));
    for (size_t i = 0; i < w.size(); ++i) w[i] = c.rw(x86::Cpu::lin(c.s[x86::SS], static_cast<uint16_t>(cx + 2 * i)));
    ++service_calls;
    if (on_trace) on_trace(svc, w);
    const uint32_t res = service(svc, w);
    c.r[x86::AX] = res & 0xFFFF;
    c.r[x86::DX] = res >> 16;
}

void Host::dos() {
    const uint8_t ah = cpu_.r[x86::AX] >> 8;
    if (ah == 0x31) {                         // reste résident : maup.exe reprend la main
        resume_cs_ = cpu_.rw(0x3CA);
        resume_ip_ = cpu_.rw(0x3C8);
        mailbox_ds_ = cpu_.rw(0x3C6);
        // l'image est chargée à l'adresse 0 : la boîte aux lettres a écrasé 6 octets de code
        std::copy(image_.begin() + 0x3C6, image_.begin() + 0x3CC, cpu_.mem.begin() + 0x3C6);
        resident_ = true;
        cpu_.stop = true;
    } else if (ah == 0x4C) {
        cpu_.stop = true;
    } else if (ah == 0x30) {
        cpu_.r[x86::AX] = 0x0005;             // DOS 5.0
    }
}

uint32_t Host::service(int n, const std::vector<uint16_t>& w) {
    auto mem_be = [&](uint32_t a, int bytes) {
        uint32_t v = 0;
        for (int i = 0; i < bytes; ++i) v = (v << 8) | cpu_.rb(a + i);
        return v;
    };
    switch (n) {
    case 3: return w.size() >= 2 ? cpu_.rb(ptr(w, 0)) : 0;
    case 4: {                                 // lire un mot (entrée et horloge : variables du moteur)
        if (w.size() < 2) return 0;
        const uint32_t a = ptr(w, 0);
        if (a == input + 2) {
            if (be16(input) == 0 && !pending_clicks_.empty()) {
                const auto c = pending_clicks_.front();
                pending_clicks_.erase(pending_clicks_.begin());
                set_be16(input + 2, static_cast<uint16_t>(c[0]));
                set_be16(input + 4, static_cast<uint16_t>(c[1]));
                set_be16(input, static_cast<uint16_t>(c[2]));
                idle_polls_ = 0;
            } else if (pending_clicks_.empty() && tick_per_read) {
                if (++idle_polls_ >= 3) cpu_.stop = true;     // la logique attend une action
            }
        }
        const uint16_t v = be16(a);
        if (a == tick_addr() && tick_per_read) set_be16(a, static_cast<uint16_t>(v + 1));   // oracle
        return v;
    }
    case 5: return w.size() >= 2 ? mem_be(ptr(w, 0), 4) : 0;
    case 31: if (w.size() >= 4) cpu_.wb(ptr(w, 2), w[0] & 0xFF); return 0;
    case 32: if (w.size() >= 4) set_be16(ptr(w, 2), w[0]); return 0;
    case 33:
        if (w.size() >= 4) {
            const uint32_t a = ptr(w, 2), v = (static_cast<uint32_t>(w[1]) << 16) | w[0];
            set_be16(a, v >> 16);
            set_be16(a + 2, v & 0xFFFF);
        }
        return 0;
    case 7: return zone();
    case 28: return kZoneBase;
    case 38: {                                // taille de l'entrée n d'une table de mots cumulés
        const uint32_t base = ptr(w, 2);
        const int i = w[0];
        return static_cast<uint16_t>(be16(base + 2 * i) - be16(base + 2 * (i - 1)));
    }
    case 15: {                                // adresse de l'entrée n d'une table de tailles (cumulée au 1er appel)
        const uint32_t base = ptr(w, 2);
        if (std::find(fixed_tables_.begin(), fixed_tables_.end(), base) == fixed_tables_.end()) {
            const uint16_t size = be16(base);
            uint16_t acc = size;
            for (int i = 1; i <= (size - 2) / 2; ++i) {
                acc = static_cast<uint16_t>(acc + be16(base + 2 * i));
                set_be16(base + 2 * i, acc);
            }
            fixed_tables_.push_back(base);
        }
        return base + be16(base + 2 * w[0]);
    }
    case 21: {                                // hasard entre deux bornes incluses
        int lo = static_cast<int16_t>(w[2]), hi = static_cast<int16_t>(w[0]);
        if (lo > hi) std::swap(lo, hi);
        if (random_mode == 1) return static_cast<uint16_t>(lo);
        rng_ = rng_ * 1103515245u + 12345u;
        return static_cast<uint16_t>(lo + static_cast<int>((rng_ >> 16) % static_cast<uint32_t>(hi - lo + 1)));
    }
    case 25:                                  // charger un bloc de ressource (w0 + 0x1E = id)
        load_resource(w[0] + 0x1E, w[2], ptr(w, 4));
        return 0;
    case 11: return on_service ? on_service(11, w) : 0;   // partie sauvegardée : fournie par le jeu
    case 40: {                                // nouvelle partie : SAUP.MAU à l'adresse passée en SS:CX+4
        const uint16_t cx = cpu_.r[x86::CX], ss = cpu_.s[x86::SS];
        const uint32_t dest = ((static_cast<uint32_t>(cpu_.rw(x86::Cpu::lin(ss, cx + 6))) << 16) |
                               cpu_.rw(x86::Cpu::lin(ss, cx + 4))) & 0xFFFFF;
        const auto d = read_all(dir_ / "SAUP.MAU");
        for (size_t i = 0; i < d.size() && i < 0x9A3; ++i) cpu_.wb(dest + static_cast<uint32_t>(i), d[i]);
        return 0;
    }
    default:
        return on_service ? on_service(n, w) : 0;
    }
}

}  // namespace maupiti::logic
