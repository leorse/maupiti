// Processeur 8086 (mode réel, avec les ajouts du 80186) : exécute la logique d'origine de la version PC
// (MAUPITI1.EXE, Turbo Pascal) en attendant sa réécriture en C++ (docs/architecture.md §14).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace maupiti::x86 {

enum Reg16 { AX, CX, DX, BX, SP, BP, SI, DI };
enum SegReg { ES, CS, SS, DS };
enum Flag : uint16_t {
    CF = 1 << 0, PF = 1 << 2, AF = 1 << 4, ZF = 1 << 6, SF = 1 << 7, TF = 1 << 8, IF = 1 << 9, DF = 1 << 10,
    OF = 1 << 11
};

class Cpu {
public:
    static constexpr uint32_t kMemSize = 0x110000;   // 1 Mo + zone haute (adresses 16 bits qui débordent)

    Cpu() : mem(kMemSize, 0) {}

    uint16_t r[8] = {};          // AX CX DX BX SP BP SI DI
    uint16_t s[4] = {};          // ES CS SS DS
    uint16_t ip = 0;
    uint16_t flags = 0x0002;
    std::vector<uint8_t> mem;
    // interruption logicielle (INT n, division par zéro…) : traitée par l'hôte au lieu de la table d'interruptions
    std::function<void(Cpu&, uint8_t)> on_int;
    bool stop = false;           // l'hôte demande l'arrêt (fin de run())
    uint64_t steps = 0;

    // Exécute jusqu'à `max` instructions ou jusqu'à stop. Renvoie le nombre d'instructions exécutées.
    uint64_t run(uint64_t max);
    void step();

    static uint32_t lin(uint16_t seg, uint16_t off) { return (static_cast<uint32_t>(seg) << 4) + off; }
    uint8_t rb(uint32_t a) const { return a < kMemSize ? mem[a] : 0xFF; }
    uint16_t rw(uint32_t a) const { return rb(a) | (rb(a + 1) << 8); }
    void wb(uint32_t a, uint8_t v) { if (a < kMemSize) mem[a] = v; }
    void ww(uint32_t a, uint16_t v) { wb(a, v & 0xFF); wb(a + 1, v >> 8); }
    uint8_t& r8(int i) {                 // AL CL DL BL AH CH DH BH
        return reinterpret_cast<uint8_t*>(&r[i & 3])[i >> 2];
    }
    void push(uint16_t v) { r[SP] -= 2; ww(lin(s[SS], r[SP]), v); }
    uint16_t pop() { const uint16_t v = rw(lin(s[SS], r[SP])); r[SP] += 2; return v; }
    // appel d'interruption tel que le ferait le matériel (pour l'hôte qui veut un vrai INT)
    void interrupt(uint8_t n);

private:
    // décodage du mode d'adressage
    struct ModRM {
        int mod, reg, rm;
        uint32_t ea;                     // adresse linéaire si mémoire
        uint16_t off;                    // décalage (LEA)
        bool mem;
    };
    int seg_override_ = -1;
    int rep_ = 0;                        // 0, 0xF2 (REPNE), 0xF3 (REP/REPE)
    uint16_t fetch16();
    uint8_t fetch8() { return rb(lin(s[CS], ip++)); }
    ModRM modrm();
    uint8_t get8(const ModRM& m) { return m.mem ? rb(m.ea) : r8(m.rm); }
    uint16_t get16(const ModRM& m) { return m.mem ? rw(m.ea) : r[m.rm]; }
    void set8(const ModRM& m, uint8_t v) { if (m.mem) wb(m.ea, v); else r8(m.rm) = v; }
    void set16(const ModRM& m, uint16_t v) { if (m.mem) ww(m.ea, v); else r[m.rm] = v; }
    int seg_for(int def) const { return seg_override_ >= 0 ? seg_override_ : def; }

    // arithmétique : met à jour les indicateurs, renvoie le résultat
    uint32_t alu(int op, uint32_t a, uint32_t b, bool word);
    uint32_t shift(int op, uint32_t v, unsigned count, bool word);
    void set_szp(uint32_t v, bool word);
    void setf(uint16_t f, bool on) { flags = on ? (flags | f) : (flags & ~f); }
    bool cond(int cc) const;
    void group3(const ModRM& m, bool word);
    void string_op(uint8_t op);
};

}  // namespace maupiti::x86
