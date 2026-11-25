// Musique et bruitages de la version Atari ST : le pilote son d'origine (archive 2, code 68000) exécuté par
// Musashi, avec les 4 timers du MFP 68901, la VBL à 50 Hz et la sortie de la puce YM2149 (docs/architecture.md
// §12). Rien n'est converti à l'avance : le son est produit pendant le jeu, à partir des archives extraites
// des disquettes (dossier <données>/arcNN/entryKKK.bin et map.bin, la table du chargeur).
#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace maupiti {

class AtariSound {
public:
    // dir : dossier des archives Atari extraites. ok() est faux s'il manque le pilote ou une table.
    explicit AtariSound(const std::filesystem::path& dir);
    ~AtariSound();
    AtariSound(const AtariSound&) = delete;
    AtariSound& operator=(const AtariSound&) = delete;

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }

    // Commandes (exécutées par le 68000 dans l'ordre, comme le programme principal du jeu)
    void title();                       // musique du titre (canal 0, banque de l'archive 19)
    void music(int bank, int seq = 0);  // musique : banque n de l'archive 60, séquence k (canal 1)
    // bruitage : banque n jouée par l'appel $148 (canal 0) ou $144 (canal 1 : bruitages longs, fouille) ;
    // duration >= 0 : appel $13C ensuite (durée, comme le moteur après le jingle de la cabine)
    void effect(int bank, unsigned call = 0x148, int duration = -1);
    // décor, comme le moteur Atari ($BA48, cas général) : banque de sons n ($15C), $13C(0x41), séquence
    // d'ambiance du lieu (entrée 2 de son archive, $158) s'il en a une, $154 ; room_archive < 0 : sans ambiance
    void room(int bank, int room_archive);
    // ambiance sonore d'un lieu : entrée 2 de l'archive du lieu, passée au pilote par l'appel $158
    // (comme au changement de lieu : $13C durée, $158 séquence, $154)
    void ambience(int room_archive);
    // son numérisé n (archive 61) : décodé par le pilote ($102C), puis joué tel quel à `rate` Hz
    void sample(int n, int rate);
    bool sample_playing() const { return raw_on_; }
    void stop();                        // silence (réinitialise le pilote)
    // appel d'un vecteur du pilote sans remise à zéro, par-dessus ce qui joue (comme le jeu : $148, $144,
    // $164, $17E…) ; poke : mot écrit avant l'appel (addr = 0 : aucun). Hors jeu : pilote démarré d'abord.
    void call(unsigned vector, unsigned d0, unsigned poke_addr = 0, unsigned poke_value = 0);

    // Produit n échantillons 16 bits mono à `rate` Hz en faisant avancer l'émulation (fil audio).
    void render(int16_t* out, int n, int rate);

    // État pour l'interface (lu sans verrou : indicatif)
    bool channel_busy(int ch) const;    // canal de partition en cours (0 = titre/jingles, 1 = musique)
    double seconds() const;             // temps écoulé depuis la dernière commande

    // Rappels de Musashi (usage interne)
    unsigned read8(unsigned a);
    void write8(unsigned a, unsigned v);
    int int_ack(int level);
    void instruction(unsigned pc);

private:
    struct Timer {
        int ctrl_reg, data_reg, ier, bit, channel;
        uint8_t control = 0, data = 0;
        double next = -1;                // échéance, en cycles du 68000 (-1 : arrêté)
    };
    struct Call {
        std::string what;
        unsigned addr;                   // vecteur de l'API du pilote (ou adresse directe si what == "direct")
        unsigned d0;
    };

    void reset_machine();
    void load(int arc, int entry, unsigned dest);
    size_t load_size(int arc, int entry) const;
    void set_control(Timer& t, uint8_t c);
    double period(const Timer& t) const;
    void start_call(const Call& c);
    void raise_irq();
    void emit_level(double cycle);
    void advance(double target);        // fait tourner la machine jusqu'au cycle donné

    std::filesystem::path dir_;
    std::vector<uint8_t> map_;          // n° de requête + $1E -> n° d'archive (table MAP du chargeur)
    std::vector<uint8_t> mem_;          // 4 Mo
    bool ok_ = false;
    std::string error_;

    double cycle_ = 0;                  // horloge du 68000
    double next_vbl_ = 0;
    Timer timers_[4];                   // A, B, C, D
    uint8_t mfp_[0x40] = {};
    unsigned pending_ = 0;              // bit 0-3 : timers A-D en attente ; bit 4 : VBL
    uint8_t ym_sel_ = 0;
    uint8_t ym_[16] = {};
    uint8_t vol_[3] = {};
    std::vector<uint16_t> level_of_;    // triplet de volumes (a<<8|b<<4|c) -> niveau 0-255 (table du jeu), 0xFFFF hors table
    bool levels_ready_ = false;

    std::mutex mutex_;                  // file de commandes (fil principal -> fil audio)
    std::deque<std::vector<Call>> queue_;
    std::vector<Call> running_;         // commande en cours : suite d'appels du pilote
    size_t step_ = 0;
    bool in_call_ = false;
    double command_cycle_ = 0;

    // sortie
    double level_ = 128;                // niveau courant 0-255
    double acc_ = 0;                    // intégrale du niveau sur l'échantillon en cours
    double out_base_ = 0, out_last_ = 0, out_samp_ = 1;
    uint64_t out_index_ = 0;            // n° de l'échantillon en cours depuis la dernière commande
    double dc_ = 0;                     // filtre passe-haut (supprime la composante continue)
    // lecture d'un son numérisé décodé
    std::vector<uint8_t> raw_;
    double raw_pos_ = 0, raw_step_ = 0;
    int raw_rate_ = 0, raw_index_ = -1;
    bool raw_on_ = false;
    bool in_game_ = false;              // la dernière commande a mis le pilote en mode jeu
    int ambience_tracks_ = 0;           // ambiance en cours : pistes redéclenchées dès qu'elles se terminent
};

}  // namespace maupiti
