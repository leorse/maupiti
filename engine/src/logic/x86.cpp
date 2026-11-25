#include "x86.hpp"

namespace maupiti::x86 {

namespace {
bool parity(uint8_t v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return !(v & 1);
}
}  // namespace

uint16_t Cpu::fetch16() {
    const uint16_t lo = fetch8();
    return lo | (fetch8() << 8);
}

Cpu::ModRM Cpu::modrm() {
    ModRM m{};
    const uint8_t b = fetch8();
    m.mod = b >> 6;
    m.reg = (b >> 3) & 7;
    m.rm = b & 7;
    if (m.mod == 3) {
        m.mem = false;
        return m;
    }
    m.mem = true;
    uint16_t off = 0;
    int def = DS;
    switch (m.rm) {
    case 0: off = r[BX] + r[SI]; break;
    case 1: off = r[BX] + r[DI]; break;
    case 2: off = r[BP] + r[SI]; def = SS; break;
    case 3: off = r[BP] + r[DI]; def = SS; break;
    case 4: off = r[SI]; break;
    case 5: off = r[DI]; break;
    case 6:
        if (m.mod == 0) off = fetch16();
        else { off = r[BP]; def = SS; }
        break;
    case 7: off = r[BX]; break;
    }
    if (m.mod == 1) off += static_cast<int8_t>(fetch8());
    else if (m.mod == 2) off += fetch16();
    m.off = off;
    m.ea = lin(s[seg_for(def)], off);
    return m;
}

void Cpu::set_szp(uint32_t v, bool word) {
    const uint32_t mask = word ? 0xFFFF : 0xFF;
    v &= mask;
    setf(ZF, v == 0);
    setf(SF, v & (word ? 0x8000 : 0x80));
    setf(PF, parity(static_cast<uint8_t>(v)));
}

uint32_t Cpu::alu(int op, uint32_t a, uint32_t b, bool word) {
    const uint32_t mask = word ? 0xFFFF : 0xFF, sign = word ? 0x8000 : 0x80;
    uint32_t res = 0;
    switch (op) {
    case 0:   // ADD
    case 2: { // ADC
        const uint32_t c = (op == 2 && (flags & CF)) ? 1 : 0;
        res = a + b + c;
        setf(CF, res > mask);
        setf(OF, ((a ^ res) & (b ^ res) & sign) != 0);
        setf(AF, ((a ^ b ^ res) & 0x10) != 0);
        break;
    }
    case 3:   // SBB
    case 5:   // SUB
    case 7: { // CMP
        const uint32_t c = (op == 3 && (flags & CF)) ? 1 : 0;
        res = a - b - c;
        setf(CF, a < b + c);
        setf(OF, ((a ^ b) & (a ^ res) & sign) != 0);
        setf(AF, ((a ^ b ^ res) & 0x10) != 0);
        break;
    }
    case 1: res = a | b; setf(CF, false); setf(OF, false); setf(AF, false); break;
    case 4: res = a & b; setf(CF, false); setf(OF, false); setf(AF, false); break;
    case 6: res = a ^ b; setf(CF, false); setf(OF, false); setf(AF, false); break;
    }
    res &= mask;
    set_szp(res, word);
    return res;
}

uint32_t Cpu::shift(int op, uint32_t v, unsigned count, bool word) {
    const unsigned bits = word ? 16 : 8;
    const uint32_t mask = word ? 0xFFFF : 0xFF, sign = word ? 0x8000 : 0x80;
    count &= 0x1F;
    if (!count) return v;
    v &= mask;
    const bool msb0 = v & sign;
    for (unsigned i = 0; i < count; ++i) {
        bool cf = flags & CF;
        switch (op) {
        case 0: cf = v & sign; v = ((v << 1) | (cf ? 1 : 0)) & mask; break;                  // ROL
        case 1: cf = v & 1; v = (v >> 1) | (cf ? sign : 0); break;                          // ROR
        case 2: { const bool o = v & sign; v = ((v << 1) | (cf ? 1 : 0)) & mask; cf = o; break; }   // RCL
        case 3: { const bool o = v & 1; v = (v >> 1) | (cf ? sign : 0); cf = o; break; }   // RCR
        case 4: case 6: cf = v & sign; v = (v << 1) & mask; break;                          // SHL/SAL
        case 5: cf = v & 1; v >>= 1; break;                                                 // SHR
        case 7: cf = v & 1; v = (v >> 1) | (v & sign); break;                               // SAR
        }
        setf(CF, cf);
    }
    // OF : défini pour un décalage de 1 ; on applique la règle du 8086 à chaque fois
    switch (op) {
    case 0: case 2: case 4: case 6: setf(OF, ((v & sign) != 0) != ((flags & CF) != 0)); break;
    case 1: case 3: setf(OF, ((v ^ (v << 1)) & sign) != 0); break;
    case 5: setf(OF, msb0); break;                              // bit de poids fort d'origine
    case 7: setf(OF, false); break;
    }
    if (op >= 4) set_szp(v, word);
    (void)bits;
    return v;
}

bool Cpu::cond(int cc) const {
    const bool cf = flags & CF, zf = flags & ZF, sf = flags & SF, of = flags & OF, pf = flags & PF;
    switch (cc) {
    case 0x0: return of;
    case 0x1: return !of;
    case 0x2: return cf;
    case 0x3: return !cf;
    case 0x4: return zf;
    case 0x5: return !zf;
    case 0x6: return cf || zf;
    case 0x7: return !cf && !zf;
    case 0x8: return sf;
    case 0x9: return !sf;
    case 0xA: return pf;
    case 0xB: return !pf;
    case 0xC: return sf != of;
    case 0xD: return sf == of;
    case 0xE: return zf || (sf != of);
    default: return !zf && (sf == of);
    }
}

void Cpu::interrupt(uint8_t n) {
    push(flags);
    push(s[CS]);
    push(ip);
    flags &= ~(IF | TF);
    ip = rw(n * 4);
    s[CS] = rw(n * 4 + 2);
}

void Cpu::group3(const ModRM& m, bool word) {
    const uint32_t v = word ? get16(m) : get8(m);
    switch (m.reg) {
    case 0: case 1: {                        // TEST
        const uint32_t imm = word ? fetch16() : fetch8();
        alu(4, v, imm, word);
        break;
    }
    case 2: if (word) set16(m, ~v); else set8(m, ~v); break;   // NOT
    case 3: {                                // NEG
        const uint32_t res = alu(5, 0, v, word);
        setf(CF, v != 0);
        if (word) set16(m, res); else set8(m, res);
        break;
    }
    case 4:                                  // MUL
        if (word) {
            const uint32_t p = static_cast<uint32_t>(r[AX]) * v;
            r[AX] = p & 0xFFFF;
            r[DX] = p >> 16;
            setf(CF, r[DX] != 0); setf(OF, r[DX] != 0);
        } else {
            r[AX] = static_cast<uint16_t>(r8(0) * v);
            setf(CF, r[AX] > 0xFF); setf(OF, r[AX] > 0xFF);
        }
        break;
    case 5:                                  // IMUL
        if (word) {
            const int32_t p = static_cast<int16_t>(r[AX]) * static_cast<int16_t>(v);
            r[AX] = p & 0xFFFF;
            r[DX] = static_cast<uint32_t>(p) >> 16;
            const bool ov = p != static_cast<int16_t>(p);
            setf(CF, ov); setf(OF, ov);
        } else {
            const int16_t p = static_cast<int8_t>(r8(0)) * static_cast<int8_t>(v);
            r[AX] = static_cast<uint16_t>(p);
            const bool ov = p != static_cast<int8_t>(p);
            setf(CF, ov); setf(OF, ov);
        }
        break;
    case 6:                                  // DIV
        if (word) {
            const uint32_t num = (static_cast<uint32_t>(r[DX]) << 16) | r[AX];
            if (!v || num / v > 0xFFFF) { if (on_int) on_int(*this, 0); else interrupt(0); return; }
            r[AX] = num / v;
            r[DX] = num % v;
        } else {
            const uint16_t num = r[AX];
            if (!v || num / v > 0xFF) { if (on_int) on_int(*this, 0); else interrupt(0); return; }
            r8(0) = num / v;
            r8(4) = num % v;
        }
        break;
    case 7:                                  // IDIV
        if (word) {
            const int32_t num = static_cast<int32_t>((static_cast<uint32_t>(r[DX]) << 16) | r[AX]);
            const int16_t d = static_cast<int16_t>(v);
            if (!d || num / d > 32767 || num / d < -32768) { if (on_int) on_int(*this, 0); else interrupt(0); return; }
            r[AX] = static_cast<uint16_t>(num / d);
            r[DX] = static_cast<uint16_t>(num % d);
        } else {
            const int16_t num = static_cast<int16_t>(r[AX]);
            const int8_t d = static_cast<int8_t>(v);
            if (!d || num / d > 127 || num / d < -128) { if (on_int) on_int(*this, 0); else interrupt(0); return; }
            r8(0) = static_cast<uint8_t>(num / d);
            r8(4) = static_cast<uint8_t>(num % d);
        }
        break;
    }
}

void Cpu::string_op(uint8_t op) {
    const bool word = op & 1;
    const int step = (flags & DF) ? -(word ? 2 : 1) : (word ? 2 : 1);
    const int src_seg = seg_for(DS);
    auto once = [&]() -> bool {              // renvoie faux si une REPE/REPNE doit s'arrêter
        switch (op) {
        case 0xA4: case 0xA5:                // MOVS
            if (word) ww(lin(s[ES], r[DI]), rw(lin(s[src_seg], r[SI])));
            else wb(lin(s[ES], r[DI]), rb(lin(s[src_seg], r[SI])));
            r[SI] += step; r[DI] += step;
            return true;
        case 0xA6: case 0xA7:                // CMPS
            if (word) alu(7, rw(lin(s[src_seg], r[SI])), rw(lin(s[ES], r[DI])), true);
            else alu(7, rb(lin(s[src_seg], r[SI])), rb(lin(s[ES], r[DI])), false);
            r[SI] += step; r[DI] += step;
            return rep_ == 0xF3 ? (flags & ZF) != 0 : (flags & ZF) == 0;
        case 0xAA: case 0xAB:                // STOS
            if (word) ww(lin(s[ES], r[DI]), r[AX]); else wb(lin(s[ES], r[DI]), r8(0));
            r[DI] += step;
            return true;
        case 0xAC: case 0xAD:                // LODS
            if (word) r[AX] = rw(lin(s[src_seg], r[SI])); else r8(0) = rb(lin(s[src_seg], r[SI]));
            r[SI] += step;
            return true;
        default:                             // SCAS
            if (word) alu(7, r[AX], rw(lin(s[ES], r[DI])), true);
            else alu(7, r8(0), rb(lin(s[ES], r[DI])), false);
            r[DI] += step;
            return rep_ == 0xF3 ? (flags & ZF) != 0 : (flags & ZF) == 0;
        }
    };
    if (!rep_) { once(); return; }
    const bool conditional = op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF;
    while (r[CX]) {
        const bool go_on = once();
        --r[CX];
        if (conditional && !go_on) break;
    }
}

uint64_t Cpu::run(uint64_t max) {
    stop = false;
    uint64_t n = 0;
    while (!stop && n < max) {
        step();
        ++n;
    }
    return n;
}

void Cpu::step() {
    ++steps;
    seg_override_ = -1;
    rep_ = 0;
    uint8_t op;
    for (;;) {                               // préfixes
        op = fetch8();
        if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E) seg_override_ = (op >> 3) & 3;
        else if (op == 0xF2 || op == 0xF3) rep_ = op;
        else if (op == 0xF0) {}
        else break;
    }
    if (op < 0x40 && (op & 7) < 6) {         // ADD OR ADC SBB AND SUB XOR CMP
        const int aop = op >> 3;
        switch (op & 7) {
        case 0: { auto m = modrm(); const auto v = alu(aop, get8(m), r8(m.reg), false); if (aop != 7) set8(m, v); break; }
        case 1: { auto m = modrm(); const auto v = alu(aop, get16(m), r[m.reg], true); if (aop != 7) set16(m, v); break; }
        case 2: { auto m = modrm(); const auto v = alu(aop, r8(m.reg), get8(m), false); if (aop != 7) r8(m.reg) = v; break; }
        case 3: { auto m = modrm(); const auto v = alu(aop, r[m.reg], get16(m), true); if (aop != 7) r[m.reg] = v; break; }
        case 4: { const auto v = alu(aop, r8(0), fetch8(), false); if (aop != 7) r8(0) = v; break; }
        case 5: { const auto v = alu(aop, r[AX], fetch16(), true); if (aop != 7) r[AX] = v; break; }
        }
        return;
    }
    switch (op) {
    case 0x06: push(s[ES]); break;
    case 0x07: s[ES] = pop(); break;
    case 0x0E: push(s[CS]); break;
    case 0x0F: s[CS] = pop(); break;          // POP CS (8086)
    case 0x16: push(s[SS]); break;
    case 0x17: s[SS] = pop(); break;
    case 0x1E: push(s[DS]); break;
    case 0x1F: s[DS] = pop(); break;
    case 0x27: {                              // DAA
        uint8_t al = r8(0);
        const bool cf = flags & CF;
        if ((al & 0xF) > 9 || (flags & AF)) { al += 6; setf(AF, true); } else setf(AF, false);
        if (r8(0) > 0x99 || cf) { al += 0x60; setf(CF, true); } else setf(CF, false);
        r8(0) = al;
        set_szp(al, false);
        break;
    }
    case 0x2F: {                              // DAS
        uint8_t al = r8(0);
        const bool cf = flags & CF;
        if ((al & 0xF) > 9 || (flags & AF)) { al -= 6; setf(AF, true); } else setf(AF, false);
        if (r8(0) > 0x99 || cf) { al -= 0x60; setf(CF, true); } else setf(CF, false);
        r8(0) = al;
        set_szp(al, false);
        break;
    }
    case 0x37: case 0x3F: {                   // AAA, AAS
        if ((r8(0) & 0xF) > 9 || (flags & AF)) {
            if (op == 0x37) { r[AX] += 0x106; } else { r8(0) -= 6; r8(4) -= 1; }
            setf(AF, true); setf(CF, true);
        } else {
            setf(AF, false); setf(CF, false);
        }
        r8(0) &= 0xF;
        break;
    }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: {   // INC r16
        const bool cf = flags & CF;
        r[op & 7] = alu(0, r[op & 7], 1, true);
        setf(CF, cf);
        break;
    }
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {   // DEC r16
        const bool cf = flags & CF;
        r[op & 7] = alu(5, r[op & 7], 1, true);
        setf(CF, cf);
        break;
    }
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x55: case 0x56: case 0x57: push(r[op & 7]); break;
    case 0x54: { const uint16_t sp = r[SP]; push(sp); break; }   // PUSH SP (80186 : valeur avant)
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: r[op & 7] = pop(); break;
    case 0x60: {                              // PUSHA
        const uint16_t sp = r[SP];
        for (int i = 0; i < 8; ++i) push(i == SP ? sp : r[i]);
        break;
    }
    case 0x61:                                // POPA
        for (int i = 7; i >= 0; --i) { const uint16_t v = pop(); if (i != SP) r[i] = v; }
        break;
    case 0x68: push(fetch16()); break;
    case 0x6A: push(static_cast<uint16_t>(static_cast<int8_t>(fetch8()))); break;
    case 0x69: case 0x6B: {                   // IMUL Gv, Ev, imm
        auto m = modrm();
        const int16_t a = static_cast<int16_t>(get16(m));
        const int16_t b = op == 0x69 ? static_cast<int16_t>(fetch16()) : static_cast<int8_t>(fetch8());
        const int32_t p = a * b;
        r[m.reg] = static_cast<uint16_t>(p);
        const bool ov = p != static_cast<int16_t>(p);
        setf(CF, ov); setf(OF, ov);
        break;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        const int8_t d = static_cast<int8_t>(fetch8());
        if (cond(op & 0xF)) ip += d;
        break;
    }
    case 0x80: case 0x82: { auto m = modrm(); const auto v = alu(m.reg, get8(m), fetch8(), false); if (m.reg != 7) set8(m, v); break; }
    case 0x81: { auto m = modrm(); const auto v = alu(m.reg, get16(m), fetch16(), true); if (m.reg != 7) set16(m, v); break; }
    case 0x83: {
        auto m = modrm();
        const uint16_t imm = static_cast<uint16_t>(static_cast<int8_t>(fetch8()));
        const auto v = alu(m.reg, get16(m), imm, true);
        if (m.reg != 7) set16(m, v);
        break;
    }
    case 0x84: { auto m = modrm(); alu(4, get8(m), r8(m.reg), false); break; }
    case 0x85: { auto m = modrm(); alu(4, get16(m), r[m.reg], true); break; }
    case 0x86: { auto m = modrm(); const uint8_t t = get8(m); set8(m, r8(m.reg)); r8(m.reg) = t; break; }
    case 0x87: { auto m = modrm(); const uint16_t t = get16(m); set16(m, r[m.reg]); r[m.reg] = t; break; }
    case 0x88: { auto m = modrm(); set8(m, r8(m.reg)); break; }
    case 0x89: { auto m = modrm(); set16(m, r[m.reg]); break; }
    case 0x8A: { auto m = modrm(); r8(m.reg) = get8(m); break; }
    case 0x8B: { auto m = modrm(); r[m.reg] = get16(m); break; }
    case 0x8C: { auto m = modrm(); set16(m, s[m.reg & 3]); break; }
    case 0x8D: { auto m = modrm(); r[m.reg] = m.off; break; }
    case 0x8E: { auto m = modrm(); s[m.reg & 3] = get16(m); break; }
    case 0x8F: { auto m = modrm(); set16(m, pop()); break; }
    case 0x90: break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        const uint16_t t = r[AX]; r[AX] = r[op & 7]; r[op & 7] = t; break;
    }
    case 0x98: r[AX] = static_cast<uint16_t>(static_cast<int8_t>(r8(0))); break;                 // CBW
    case 0x99: r[DX] = (r[AX] & 0x8000) ? 0xFFFF : 0; break;                                     // CWD
    case 0x9A: {                              // CALL far
        const uint16_t nip = fetch16(), ncs = fetch16();
        push(s[CS]); push(ip);
        ip = nip; s[CS] = ncs;
        break;
    }
    case 0x9B: break;                         // WAIT
    case 0x9C: push(flags); break;
    case 0x9D: flags = (pop() & 0x0FD5) | 0xF002; break;
    case 0x9E: flags = (flags & 0xFF00) | (r8(4) & 0xD5) | 0x02; break;                          // SAHF
    case 0x9F: r8(4) = flags & 0xFF; break;                                                       // LAHF
    case 0xA0: r8(0) = rb(lin(s[seg_for(DS)], fetch16())); break;
    case 0xA1: r[AX] = rw(lin(s[seg_for(DS)], fetch16())); break;
    case 0xA2: wb(lin(s[seg_for(DS)], fetch16()), r8(0)); break;
    case 0xA3: ww(lin(s[seg_for(DS)], fetch16()), r[AX]); break;
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(op);
        break;
    case 0xA8: alu(4, r8(0), fetch8(), false); break;
    case 0xA9: alu(4, r[AX], fetch16(), true); break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: r8(op & 7) = fetch8(); break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: r[op & 7] = fetch16(); break;
    case 0xC0: { auto m = modrm(); const uint8_t c = fetch8(); set8(m, shift(m.reg, get8(m), c, false)); break; }
    case 0xC1: { auto m = modrm(); const uint8_t c = fetch8(); set16(m, shift(m.reg, get16(m), c, true)); break; }
    case 0xC2: { const uint16_t n = fetch16(); ip = pop(); r[SP] += n; break; }
    case 0xC3: ip = pop(); break;
    case 0xC4: case 0xC5: {                   // LES, LDS
        auto m = modrm();
        r[m.reg] = rw(m.ea);
        s[op == 0xC4 ? ES : DS] = rw(m.ea + 2);
        break;
    }
    case 0xC6: { auto m = modrm(); set8(m, fetch8()); break; }
    case 0xC7: { auto m = modrm(); set16(m, fetch16()); break; }
    case 0xC8: {                              // ENTER
        const uint16_t size = fetch16();
        const uint8_t level = fetch8() & 31;
        push(r[BP]);
        const uint16_t frame = r[SP];
        for (int i = 1; i < level; ++i) { r[BP] -= 2; push(rw(lin(s[SS], r[BP]))); }
        if (level) push(frame);
        r[BP] = frame;
        r[SP] -= size;
        break;
    }
    case 0xC9: r[SP] = r[BP]; r[BP] = pop(); break;   // LEAVE
    case 0xCA: { const uint16_t n = fetch16(); ip = pop(); s[CS] = pop(); r[SP] += n; break; }
    case 0xCB: ip = pop(); s[CS] = pop(); break;
    case 0xCC: if (on_int) on_int(*this, 3); else interrupt(3); break;
    case 0xCD: { const uint8_t n = fetch8(); if (on_int) on_int(*this, n); else interrupt(n); break; }
    case 0xCE: if (flags & OF) { if (on_int) on_int(*this, 4); else interrupt(4); } break;
    case 0xCF: ip = pop(); s[CS] = pop(); flags = (pop() & 0x0FD5) | 0xF002; break;   // IRET
    case 0xD0: { auto m = modrm(); set8(m, shift(m.reg, get8(m), 1, false)); break; }
    case 0xD1: { auto m = modrm(); set16(m, shift(m.reg, get16(m), 1, true)); break; }
    case 0xD2: { auto m = modrm(); set8(m, shift(m.reg, get8(m), r8(1), false)); break; }
    case 0xD3: { auto m = modrm(); set16(m, shift(m.reg, get16(m), r8(1), true)); break; }
    case 0xD4: {                              // AAM
        const uint8_t base = fetch8();
        if (!base) { if (on_int) on_int(*this, 0); else interrupt(0); break; }
        r8(4) = r8(0) / base; r8(0) %= base;
        set_szp(r8(0), false);
        break;
    }
    case 0xD5: {                              // AAD
        const uint8_t base = fetch8();
        r8(0) = static_cast<uint8_t>(r8(4) * base + r8(0));
        r8(4) = 0;
        set_szp(r8(0), false);
        break;
    }
    case 0xD7: r8(0) = rb(lin(s[seg_for(DS)], static_cast<uint16_t>(r[BX] + r8(0)))); break;   // XLAT
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        modrm();                              // coprocesseur : ignoré (non utilisé par la logique)
        break;
    case 0xE0: { const int8_t d = fetch8(); if (--r[CX] && !(flags & ZF)) ip += d; break; }   // LOOPNZ
    case 0xE1: { const int8_t d = fetch8(); if (--r[CX] && (flags & ZF)) ip += d; break; }    // LOOPZ
    case 0xE2: { const int8_t d = fetch8(); if (--r[CX]) ip += d; break; }                     // LOOP
    case 0xE3: { const int8_t d = fetch8(); if (!r[CX]) ip += d; break; }                      // JCXZ
    case 0xE4: fetch8(); r8(0) = 0xFF; break;  // IN / OUT : pas de périphériques
    case 0xE5: fetch8(); r[AX] = 0xFFFF; break;
    case 0xE6: case 0xE7: fetch8(); break;
    case 0xEC: r8(0) = 0xFF; break;
    case 0xED: r[AX] = 0xFFFF; break;
    case 0xEE: case 0xEF: break;
    case 0xE8: { const int16_t d = static_cast<int16_t>(fetch16()); push(ip); ip += d; break; }
    case 0xE9: { const int16_t d = static_cast<int16_t>(fetch16()); ip += d; break; }
    case 0xEA: { const uint16_t nip = fetch16(), ncs = fetch16(); ip = nip; s[CS] = ncs; break; }
    case 0xEB: { const int8_t d = static_cast<int8_t>(fetch8()); ip += d; break; }
    case 0xF4: stop = true; break;            // HLT : rend la main à l'hôte
    case 0xF5: flags ^= CF; break;
    case 0xF6: { auto m = modrm(); group3(m, false); break; }
    case 0xF7: { auto m = modrm(); group3(m, true); break; }
    case 0xF8: setf(CF, false); break;
    case 0xF9: setf(CF, true); break;
    case 0xFA: setf(IF, false); break;
    case 0xFB: setf(IF, true); break;
    case 0xFC: setf(DF, false); break;
    case 0xFD: setf(DF, true); break;
    case 0xFE: {                              // INC/DEC Eb
        auto m = modrm();
        const bool cf = flags & CF;
        set8(m, alu(m.reg == 0 ? 0 : 5, get8(m), 1, false));
        setf(CF, cf);
        break;
    }
    case 0xFF: {
        auto m = modrm();
        switch (m.reg) {
        case 0: case 1: {
            const bool cf = flags & CF;
            set16(m, alu(m.reg == 0 ? 0 : 5, get16(m), 1, true));
            setf(CF, cf);
            break;
        }
        case 2: { const uint16_t t = get16(m); push(ip); ip = t; break; }                     // CALL near
        case 3: { const uint16_t nip = rw(m.ea), ncs = rw(m.ea + 2);                         // CALL far
                  push(s[CS]); push(ip); ip = nip; s[CS] = ncs; break; }
        case 4: ip = get16(m); break;                                                         // JMP near
        case 5: ip = rw(m.ea); s[CS] = rw(m.ea + 2); break;                                   // JMP far
        default: push(get16(m)); break;                                                       // PUSH
        }
        break;
    }
    default:                                  // opcode inconnu : interruption 6 (80186)
        if (on_int) on_int(*this, 6); else interrupt(6);
        break;
    }
}

}  // namespace maupiti::x86
