// Hôte de la logique d'origine (version PC) : charge MAUPITI1.EXE décompressé, émule le peu de DOS qu'il
// utilise et sert ses appels au moteur (`int 0xFE`, 51 services ; docs/architecture.md §3, §7, §14).
// Les services sont écrits en C++ ; ceux qui touchent l'écran, le son ou l'entrée sont délégués au jeu.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "x86.hpp"

namespace maupiti::logic {

class Host {
public:
    // dir : dossier des fichiers de la version PC utilisés par la logique (pack/pc : maupiti1.img, maup.img,
    // G1.CON … G6.CON, SAUP.MAU)
    explicit Host(const std::filesystem::path& dir);
    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }

    // Démarrage : jusqu'à la mise en résident, puis reprise comme maup.exe. Renvoie faux en cas d'échec.
    bool boot(uint64_t max_steps = 200'000'000);
    // Fait tourner la logique jusqu'à ce qu'elle attende une action (ou max_steps).
    uint64_t run(uint64_t max_steps);

    // Service non traité par l'hôte : (n, mots de paramètres ; mots[0] = dernier empilé) -> résultat DX:AX
    std::function<uint32_t(int, const std::vector<uint16_t>&)> on_service;
    // Bloc de ressource chargé par la logique (service 25) : (id, sous-bloc, adresse, taille)
    std::function<void(int, int, uint32_t, uint32_t)> on_load;
    // Charge un bloc de ressource comme le chargeur de maup.exe (rend sa taille)
    uint32_t load_resource(int id, int sub, uint32_t dest);
    // Trace de chaque appel (n, mots)
    std::function<void(int, const std::vector<uint16_t>&)> on_trace;

    // Mémoire (adresses linéaires 20 bits, valeurs big-endian comme les données d'origine)
    x86::Cpu& cpu() { return cpu_; }
    uint16_t be16(uint32_t a) const { return (cpu_.rb(a) << 8) | cpu_.rb(a + 1); }
    void set_be16(uint32_t a, uint16_t v) { cpu_.wb(a, v >> 8); cpu_.wb(a + 1, v & 0xFF); }
    uint32_t zone() const { return (static_cast<uint32_t>(mailbox_ds_) << 4) + 0x166; }   // DS:0x166 (état du jeu)

    // Entrée : enregistrement désigné par la logique au service 23 (+0 clic, +2 x, +4 y, +0x16 horloge à
    // 18,2 Hz), par défaut 0x5658A comme dans l'oracle
    uint32_t input = 0x5658A;
    uint32_t tick_addr() const { return input + 0x16; }
    bool tick_per_read = true;                           // vrai : un tic par lecture (oracle) ; faux : tick()
    // interruption d'horloge (1Ch, 18,2 Hz) comme maup.exe (02DB:06F0) : toutes les 36 interruptions, +0x200
    void tick() {
        if (--tick_div_ > 0) return;
        tick_div_ = 36;
        // maup.exe ajoute 0x200 au mot en little-endian ; la logique le lit en big-endian (+2 toutes les 2 s,
        // divisé par 60 : 10 minutes de jeu par minute réelle)
        cpu_.ww(tick_addr(), static_cast<uint16_t>(cpu_.rw(tick_addr()) + 0x200));
    }
    void click(int x, int y, int button = 1);
    // tranche d'exécution sans arrêt sur l'attente (jeu interactif)
    void run_slice(uint64_t steps) { cpu_.run(steps); }
    bool waiting() const { return idle_polls_ >= 3; }   // la logique attend une action
    int random_mode = 0;                                 // 0 : hasard ; 1 : borne basse (tests)
    uint64_t service_calls = 0;

private:
    void on_int(x86::Cpu& c, uint8_t n);
    void dos();
    uint32_t service(int n, const std::vector<uint16_t>& w);
    std::vector<uint8_t> read_resource(int id, int sub) const;
    uint32_t ptr(const std::vector<uint16_t>& w, size_t i) const {
        return ((static_cast<uint32_t>(w[i + 1]) << 16) | w[i]) & 0xFFFFF;
    }

    std::filesystem::path dir_;
    x86::Cpu cpu_;
    std::vector<uint8_t> image_;
    std::map<int, int> stack_bytes_;    // service -> octets de paramètres (enveloppes Pascal)
    // ressources de maup.exe : id -> (groupe, rang) ; décalages des nœuds par groupe
    std::map<int, std::pair<int, int>> res_where_;
    std::map<int, std::vector<uint32_t>> res_nodes_;
    bool ok_ = false, resident_ = false;
    std::string error_;
    uint16_t mailbox_ds_ = 0;
    uint16_t resume_cs_ = 0, resume_ip_ = 0;
    std::vector<uint32_t> fixed_tables_;
    int idle_polls_ = 0;
    int tick_div_ = 36;
    uint32_t tick_reads_ = 0;
    uint32_t rng_ = 1991;
    std::vector<std::array<int, 3>> pending_clicks_;
};

}  // namespace maupiti::logic
