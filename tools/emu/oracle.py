#!/usr/bin/env python3
"""Oracle : exécute la logique d'origine (MAUPITI1.EXE, Turbo Pascal) dans un émulateur x86-16.

Les services du moteur (`int 0xFE`, voir docs/architecture.md §3) sont réimplémentés en
Python (tools/emu/services.py). Sert à observer le comportement exact de la logique
(étape 4) puis à tester la logique réécrite en C++ (étape 6).

    python tools/emu/oracle.py [--steps N] [--trace]

Prérequis : out/re/maupiti1.exe (python tools/re/unlzexe.py originals/pc/MAUPITI1.EXE out/re/maupiti1.exe),
pip install unicorn.
"""
from __future__ import annotations

import argparse
import collections
import os
import re
import pickle
import struct
import sys

from unicorn import UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_CODE, Uc, UcError
from unicorn.x86_const import (UC_X86_REG_AX, UC_X86_REG_BP, UC_X86_REG_BX, UC_X86_REG_CS, UC_X86_REG_CX,
                               UC_X86_REG_DI, UC_X86_REG_DS, UC_X86_REG_DX, UC_X86_REG_ES, UC_X86_REG_IP,
                               UC_X86_REG_SI, UC_X86_REG_SP, UC_X86_REG_SS, UC_X86_REG_FLAGS)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IMAGE = os.path.join(ROOT, "out", "re", "maupiti1.exe")

ENTRY_IP, ENTRY_CS = 0xEF37, 0x0000          # programme principal (en-tête LZEXE)
STACK_SS, STACK_SP = 0x12BD, 0x2000
MEM_SIZE = 0x100000 + 0x10000                # 1 Mo réel + HMA (adresses 16 bits qui débordent)


def lin(seg: int, off: int) -> int:
    return ((seg << 4) + off) & 0xFFFFF


class Oracle:
    def __init__(self, trace: bool = False):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map(0, MEM_SIZE)
        self.image = open(IMAGE, "rb").read()
        self.uc.mem_write(0, self.image)
        self.trace = trace
        self.quiet = set()
        self.sig = 0
        self.observer = None         # fonction (n, words) appelée à chaque service (traçage)
        self.ring = collections.deque(maxlen=60)   # derniers appels de services
        self.calls = 0
        self.services: dict = {}
        self.stack_bytes = self._find_wrappers()
        from services import install            # noqa: E402  (tools/emu/services.py)
        install(self)
        self.uc.hook_add(UC_HOOK_INTR, self._on_int)
        self.done = False
        self.resume = None

    # --- accès mémoire
    def rb(self, seg, off): return self.uc.mem_read(lin(seg, off), 1)[0]
    def rw(self, seg, off): return struct.unpack("<H", self.uc.mem_read(lin(seg, off), 2))[0]
    def rl(self, seg, off): return struct.unpack("<I", self.uc.mem_read(lin(seg, off), 4))[0]
    def wb(self, seg, off, v): self.uc.mem_write(lin(seg, off), bytes([v & 0xFF]))
    def ww(self, seg, off, v): self.uc.mem_write(lin(seg, off), struct.pack("<H", v & 0xFFFF))
    def wl(self, seg, off, v): self.uc.mem_write(lin(seg, off), struct.pack("<I", v & 0xFFFFFFFF))
    def reg(self, r): return self.uc.reg_read(r)
    def setreg(self, r, v): self.uc.reg_write(r, v)

    def _find_wrappers(self) -> dict[int, int]:
        """Octets de paramètres de chaque service, lus dans les enveloppes Pascal (segment 0x10B8)."""
        pat = re.compile(rb"\xb8(.)\x00\x8b\xcd\x83\xc1\x06\xcd\xfe\x5d(?:\xca(..)|\xcb)", re.S)
        out = {}
        for m in pat.finditer(self.image):
            out[m.group(1)[0]] = struct.unpack("<H", m.group(2))[0] if m.group(2) else 0
        return out

    def args(self, cx: int, n: int | None = None) -> list[int]:
        """Mots de paramètres à SS:CX, dans l'ordre d'empilement inverse (le dernier empilé en premier)."""
        ss = self.reg(UC_X86_REG_SS)
        return [self.rw(ss, cx + 2 * i) for i in range(n if n is not None else 0)]

    # --- interruptions
    def _on_int(self, uc, intno, _):
        if intno == 0xFE:
            ax = self.reg(UC_X86_REG_AX) & 0xFFFF
            cx = self.reg(UC_X86_REG_CX) & 0xFFFF
            n = ax
            nbytes = self.stack_bytes.get(n, 0)
            words = self.args(cx, nbytes // 2)       # words[0] = dernier paramètre empilé
            self.calls += 1
            if n not in (3, 4, 5, 15, 21, 22, 31, 32, 33, 37, 38):
                self.sig = hash((self.sig, n, tuple(words)))     # empreinte de ce qui est affiché
            self.ring.append((self.calls, n, words, self.reg(UC_X86_REG_CS), self.rw(self.reg(UC_X86_REG_SS), (self.reg(UC_X86_REG_SP) + 2) & 0xFFFF)))
            if self.observer:
                self.observer(n, words)
            fn = self.services.get(n)
            if self.trace and n not in self.quiet:
                print(f"[{self.calls:6d}] svc {n:2d} args={['%04X' % w for w in words]}", flush=True)
            res = fn(self, words) if fn else 0
            if fn is None and self.trace and n not in self.quiet:
                print(f"         (non implémenté: 0)", flush=True)
            res = (res or 0) & 0xFFFFFFFF
            self.setreg(UC_X86_REG_AX, res & 0xFFFF)
            self.setreg(UC_X86_REG_DX, res >> 16)
        elif intno == 0x21:
            self._dos()
        else:
            if self.trace:
                print(f"int {intno:#x} ignorée", flush=True)

    def _dos(self):
        ah = (self.reg(UC_X86_REG_AX) >> 8) & 0xFF
        if self.trace:
            print(f"  DOS AH={ah:#04x}", flush=True)
        if ah == 0x31:
            # la logique reste résidente ; maup.exe saute ensuite à l'adresse laissée en 0000:03C8
            self.resume = (self.rw(0, 0x3CA), self.rw(0, 0x3C8), self.rw(0, 0x3C6))
            self.mailbox_ds = self.resume[2]
            # l'image est chargée à l'adresse 0 : la boîte aux lettres écrase 6 octets de code
            self.uc.mem_write(0x3C6, self.image[0x3C6:0x3CC])
            self.uc.emu_stop()
        elif ah == 0x4C:
            self.done = True
            self.uc.emu_stop()
        elif ah == 0x30:
            self.setreg(UC_X86_REG_AX, 0x0005)       # DOS 5.0

    # --- instantanés et appels directs de fonctions
    REGS = (UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_SI, UC_X86_REG_DI,
            UC_X86_REG_BP, UC_X86_REG_SP, UC_X86_REG_SS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_FLAGS)

    def snapshot(self, path):
        regs = {r: self.reg(r) for r in self.REGS}
        regs[UC_X86_REG_CS], regs[UC_X86_REG_IP] = self.reg(UC_X86_REG_CS), self.reg(UC_X86_REG_IP)
        with open(path, "wb") as f:
            pickle.dump({"mem": bytes(self.uc.mem_read(0, MEM_SIZE)), "regs": regs, "state": self.state,
                         "mailbox_ds": self.mailbox_ds, "calls": self.calls}, f)

    def restore(self, path):
        d = pickle.load(open(path, "rb"))
        self.uc.mem_write(0, d["mem"])
        for r, v in d["regs"].items():
            self.setreg(r, v)
        self.state, self.mailbox_ds, self.calls = d["state"], d["mailbox_ds"], d["calls"]
        self.resume_at = (d["regs"].get(UC_X86_REG_CS), d["regs"].get(UC_X86_REG_IP))

    def run_from_snapshot(self, max_insn=50_000_000):
        """Reprend l'exécution là où l'instantané a été pris (boucle principale du jeu)."""
        cs, ip = self.resume_at
        try:
            self.uc.emu_start(lin(cs, ip), 0xFFFFF, count=max_insn)
        except UcError as e:
            print(f"arrêt : {e}")

    STUB = 0xFFF0                      # adresse de retour factice (segment de code 0)

    def call_near(self, ip, args, max_insn=50_000_000):
        """Appelle une fonction Pascal du segment de code 0 (arguments empilés dans l'ordre de la
        déclaration, la fonction retire elle-même ses paramètres). Rend la main au retour."""
        sp = (self.reg(UC_X86_REG_SP) - 0x400) & 0xFFFF       # sous la pile en cours : rien d'écrasé
        ss = self.reg(UC_X86_REG_SS)
        for a in list(args) + [self.STUB]:
            sp = (sp - 2) & 0xFFFF
            self.ww(ss, sp, a)
        self.setreg(UC_X86_REG_SP, sp)
        self.setreg(UC_X86_REG_CS, 0)
        try:
            self.uc.emu_start(lin(0, ip), lin(0, self.STUB), count=max_insn)
        except UcError as e:
            print(f"arrêt : {e}")
        return self.reg(UC_X86_REG_IP) == self.STUB

    # --- exécution
    def run(self, max_insn: int = 50_000_000):
        self.setreg(UC_X86_REG_SS, STACK_SS)
        self.setreg(UC_X86_REG_SP, STACK_SP)
        self.setreg(UC_X86_REG_DS, 0x10F1)
        self.setreg(UC_X86_REG_ES, 0x10F1)
        self.setreg(UC_X86_REG_CS, ENTRY_CS)
        try:
            self.uc.emu_start(lin(ENTRY_CS, ENTRY_IP), 0xFFFFF, count=max_insn)
            if self.resume:
                cs, ip, ds = self.resume
                print(f"résident ; reprise à {cs:04X}:{ip:04X} (DS={ds:04X})")
                self.setreg(UC_X86_REG_DS, ds)
                self.setreg(UC_X86_REG_ES, ds)
                self.setreg(UC_X86_REG_CS, cs)
                self.uc.emu_start(lin(cs, ip), 0xFFFFF, count=max_insn)
            cs, ip = self.reg(UC_X86_REG_CS), self.reg(UC_X86_REG_IP)
            print(f"fin d'exécution à {cs:04X}:{ip:04X} (linéaire {lin(cs, ip):#07x})")
        except UcError as e:
            cs, ip = self.reg(UC_X86_REG_CS), self.reg(UC_X86_REG_IP)
            print(f"arrêt : {e} à {cs:04X}:{ip:04X} (linéaire {lin(cs, ip):#07x})")
        return self.calls


def main() -> int:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--steps", type=int, default=5_000_000)
    ap.add_argument("--trace", action="store_true")
    ap.add_argument("--tail", type=int, default=0, help="affiche les derniers appels de services")
    ap.add_argument("--quiet", default="3,4,5,15,31,32,33", help="services non affichés par --trace")
    a = ap.parse_args()
    o = Oracle(trace=a.trace)
    o.quiet = {int(x) for x in a.quiet.split(',') if x}
    print("services trouvés :", len(o.stack_bytes))
    n = o.run(a.steps)
    print(f"{n} appels de services ; derniers :")
    for c, sv, w, cs, ret in list(o.ring)[-a.tail:]:
        print(f"  [{c}] svc {sv} {['%X' % x for x in w]}  (retour logique {ret:04X})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
