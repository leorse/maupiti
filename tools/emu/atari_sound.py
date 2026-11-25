#!/usr/bin/env python3
"""Oracle sonore Atari : exécute le pilote son d'origine (archive 2, 68000) et enregistre la puce YM2149.

    python tools/emu/atari_sound.py musique 5 [--secondes 30]      -> out/atari/son/musique_05.wav
    python tools/emu/atari_sound.py titre --secondes 180            -> out/atari/son/titre.wav
    python tools/emu/atari_sound.py tout                           titre + banques 0-41, en parallèle
    python tools/emu/atari_sound.py appel 0x148 42                  (appel brut de l'API du pilote)

Vitesse : environ le temps réel par rendu (25 000 interruptions par seconde de musique, chacune relancée
depuis Python) ; le mode « tout » en fait plusieurs à la fois. Le moteur, lui, exécutera le pilote en
natif (Musashi) pendant le jeu : aucune extraction préalable de la musique ne sera nécessaire.

Le pilote (docs/architecture.md §12) est chargé en $138 comme par le jeu ; ses appels au chargeur de
disquette ($B6DC) sont servis à partir des archives extraites (out/atari/set1). Le 68000 est émulé par
unicorn ; le MFP (4 timers) et la VBL (50 Hz) sont simulés par un ordonnanceur d'événements, la puce YM
est réduite à ses 3 registres de volume (le pilote coupe les voies sonores et joue des échantillons
en modulant les volumes). Le résultat sert de référence pour le lecteur natif du moteur.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
import wave

from unicorn import UC_ARCH_M68K, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_MODE_BIG_ENDIAN, Uc
from unicorn.m68k_const import (UC_CPU_M68K_M68000, UC_M68K_REG_A7, UC_M68K_REG_D0, UC_M68K_REG_PC,
                                UC_M68K_REG_SR)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
ATARI = os.path.join(ROOT, "out", "atari", "set1")

MFP_HZ = 2_457_600                       # horloge des timers du MFP 68901
VBL_TICKS = MFP_HZ / 50.053              # trame PAL
PRESCALE = (0, 4, 10, 16, 50, 64, 100, 200)
STOP = 0x3F0000                          # adresse de retour factice des appels
IRQ_STOP = 0x3F0004
LOADER = 0x3F0010                        # remplace le chargeur de disquette (pointeur en $B6DC)
DRIVER_BASE = 0x138
# YM2149 : niveaux de sortie des 16 volumes (mesures usuelles, normalisées)
YM_LEVEL = (0.0, 0.0137, 0.0205, 0.0291, 0.0423, 0.0618, 0.0847, 0.1369, 0.1691, 0.2647, 0.3527, 0.4499,
            0.5704, 0.6873, 0.8482, 1.0)


def archive_map():
    from maupiti.atari.loader import find_tables
    return find_tables(open(os.path.join(ATARI, "loader.bin"), "rb").read()).archive_map


class Timer:
    def __init__(self, name, ier, bit, vector):
        self.name, self.ier, self.bit, self.vector = name, ier, bit, vector
        self.control = 0
        self.data = 0
        self.next = None

    def period(self):
        return PRESCALE[self.control & 7] * (self.data or 256)


class AtariSound:
    def __init__(self, two_drives=True, big_memory=False):
        self.uc = uc = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
        uc.ctl_set_cpu_model(UC_CPU_M68K_M68000)
        uc.mem_map(0, 0x400000)
        uc.mem_map(0xFFFF0000, 0x10000)              # E/S (adresses courtes étendues : $8800.w = $FFFF8800)
        uc.mem_map(0x00FF0000, 0x10000)
        self.map = archive_map()
        self.t = 0.0                                 # temps simulé, en tops MFP
        self.mfp = bytearray(0x40)
        self.timers = {"A": Timer("A", 0x07, 5, 0x134), "B": Timer("B", 0x07, 0, 0x120),
                       "C": Timer("C", 0x09, 5, 0x114), "D": Timer("D", 0x09, 4, 0x110)}
        self.ym_sel = 0
        self.ym = [0] * 16
        self.events = []                             # (temps, registre, valeur) des écritures YM
        self.loads = []
        uc.mem_write(STOP, b"\x4e\x71" * 8)          # nop
        uc.mem_write(LOADER, b"\x4e\x75")            # rts
        uc.hook_add(UC_HOOK_CODE, self._loader, begin=LOADER, end=LOADER)
        # unicorn n'exécute pas RTE en mode 68000 : on le fait à sa place, aux adresses où il figure
        drv = open(os.path.join(ATARI, "arc02", "entry000.bin"), "rb").read()
        for off in range(0, len(drv) - 1, 2):
            if drv[off:off + 2] == b"Ns":
                uc.hook_add(UC_HOOK_CODE, self._rte, begin=DRIVER_BASE + off, end=DRIVER_BASE + off)
        # « ori.b #n,$FA1D » dont n vient d'être écrit par l'instruction précédente (gestionnaires $31F0 et
        # $3214, changement de fréquence des timers C et D) : unicorn exécuterait l'ancienne valeur
        for at in (0x320C, 0x3230):
            uc.hook_add(UC_HOOK_CODE, self._patched_ori, begin=at, end=at)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._io, begin=0xFFFF8000, end=0xFFFFFFFF)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._io, begin=0x00FF8000, end=0x00FFFFFF)
        w = lambda a, v, n=2: uc.mem_write(a, v.to_bytes(n, "big"))
        w(0xC6, 1 if two_drives else 0)              # 2e lecteur : toute la musique accessible à la demande
        w(0xC8, 1 if big_memory else 0)              # 1 Mo : tout est préchargé en mémoire
        self.load_arc(0, 0, 0xB66C)                  # bibliothèque du moteur : tables de volume YM ($B774)
        w(0xB6DC, LOADER, 4)
        # chargements faits par le programme de démarrage (archive 1, table en $74110)
        self.load_arc(2, 0, DRIVER_BASE)
        self.load_arc(3, 0, 0x74C48)
        if two_drives:                               # la table de $74120 n'est remplacée qu'avec 1 lecteur
            for k, a in enumerate((0x501A4, 0x5C4F4, 0x68844)):
                self.load_arc(18, k, a)
        else:
            self.load_arc(4, 0, 0x501A4)
            self.load_arc(4, 1, 0x5B16C)

    # --- mémoire et périphériques
    def r32(self, a):
        return struct.unpack(">I", self.uc.mem_read(a, 4))[0]

    def load_arc(self, arc, entry, dest):
        p = os.path.join(ATARI, f"arc{arc:02d}", f"entry{entry:03d}.bin")
        data = open(p, "rb").read() if os.path.exists(p) else b""
        self.uc.mem_write(dest, data)
        return len(data)

    def _loader(self, uc, address, size, _):
        sp = uc.reg_read(UC_M68K_REG_A7)
        ret, req, entry, dest, _work = struct.unpack(">5I", uc.mem_read(sp, 20))
        arc = self.map[(req & 0xFF) + 0x1E]
        n = self.load_arc(arc, entry, dest)
        self.loads.append((req, arc, entry, dest, n))
        uc.mem_write(sp + 16, struct.pack(">I", ret))   # la fonction dépile ses 4 arguments
        uc.reg_write(UC_M68K_REG_A7, sp + 16)
        uc.reg_write(UC_M68K_REG_D0, n)

    def _rte(self, uc, address, size, _):
        if uc.mem_read(address, 2) != b"Ns":
            return
        sp = uc.reg_read(UC_M68K_REG_A7)
        sr, pc = struct.unpack(">HI", uc.mem_read(sp, 6))
        uc.reg_write(UC_M68K_REG_A7, sp + 6)
        uc.reg_write(UC_M68K_REG_SR, sr | 0x2000)    # on reste en superviseur (même pile)
        uc.reg_write(UC_M68K_REG_PC, pc)

    def _patched_ori(self, uc, address, size, _):
        op = bytes(uc.mem_read(address, 6))
        if op[:2] != bytes((0x00, 0x38)) or op[4:6] != bytes((0xFA, 0x1D)):
            return
        value = (uc.mem_read(0xFFFFFA1D, 1)[0] | op[3]) & 0xFF
        uc.mem_write(0xFFFFFA1D, bytes([value]))
        self._io_byte(0xFA1D, value)
        uc.reg_write(UC_M68K_REG_PC, address + 6)

    def _io(self, uc, access, address, size, value, _):
        a = address & 0xFFFF
        data = value.to_bytes(size, "big")
        for i, b in enumerate(data):
            self._io_byte((a + i) & 0xFFFF, b)

    def _io_byte(self, a, b):
        if a in (0x8800, 0x8801):
            if a == 0x8800:
                self.ym_sel = b & 0x0F
        elif a in (0x8802, 0x8803):
            if a == 0x8802:
                self.ym[self.ym_sel] = b
                self.events.append((self.t, self.ym_sel, b))
        elif 0xFA00 <= a < 0xFA40:
            r = a - 0xFA00
            self.mfp[r] = b
            tm = self.timers
            if r == 0x19:
                self._set_control(tm["A"], b & 0x0F)
            elif r == 0x1B:
                self._set_control(tm["B"], b & 0x0F)
            elif r == 0x1D:
                self._set_control(tm["C"], (b >> 4) & 7)
                self._set_control(tm["D"], b & 7)
            elif r in (0x1F, 0x21, 0x23, 0x25):
                t = tm["ABCD"[(r - 0x1F) // 2]]
                t.data = b
                if t.next is None or not (t.control & 7):
                    pass

    def _set_control(self, t, c):
        was = t.control & 7 if t.control < 8 else 0
        t.control = c
        if c == 0 or c >= 8:                         # arrêt (les modes « événement » ne servent pas ici)
            t.next = None
        elif not was or t.next is None:
            t.next = self.t + t.period()

    # --- exécution
    def call(self, addr, d0=0, max_events=2_000_000):
        """Appelle une fonction du pilote (retour sur STOP) ; les interruptions continuent pendant l'appel."""
        uc = self.uc
        sp = 0x3EFF00
        uc.mem_write(sp, struct.pack(">I", STOP))
        uc.reg_write(UC_M68K_REG_SR, 0x2300)         # SR d'abord : il choisit la pile (superviseur)
        uc.reg_write(UC_M68K_REG_A7, sp)
        uc.reg_write(UC_M68K_REG_D0, d0)
        pc = addr
        for _ in range(max_events):
            uc.emu_start(pc, STOP, count=20000)
            pc = uc.reg_read(UC_M68K_REG_PC)
            if pc == STOP:
                return uc.reg_read(UC_M68K_REG_D0)
            self.step(self.ipl())                    # attente active : le temps avance
        raise RuntimeError(f"appel {addr:#x} bloqué en {pc:#x}")

    def ipl(self):
        return (self.uc.reg_read(UC_M68K_REG_SR) >> 8) & 7

    def _run_until(self, start, chunk=4000, limit=20000):
        """Exécute jusqu'au retour sur IRQ_STOP. Si le code attend (par ex. le tic qui guette l'échantillon
        suivant d'une voix pour démarrer les autres en phase), les timers du MFP l'interrompent comme sur la
        machine réelle : le temps avance jusqu'à leur prochaine échéance."""
        uc = self.uc
        pc = start
        for _ in range(limit):
            uc.emu_start(pc, IRQ_STOP, count=chunk)
            pc = uc.reg_read(UC_M68K_REG_PC)
            if pc == IRQ_STOP:
                return
            self.step(self.ipl(), timers_only=True)
        raise RuntimeError(f"code bloqué en {pc:#x}")

    def interrupt(self, vector):
        uc = self.uc
        pc, sr, sp = uc.reg_read(UC_M68K_REG_PC), uc.reg_read(UC_M68K_REG_SR), uc.reg_read(UC_M68K_REG_A7)
        handler = self.r32(vector)
        if not handler:
            return
        frame = sp - 6
        uc.mem_write(frame, struct.pack(">HI", sr, IRQ_STOP))
        uc.reg_write(UC_M68K_REG_SR, (sr & ~0x0700) | 0x2600)
        uc.reg_write(UC_M68K_REG_A7, frame)
        self._run_until(handler)
        uc.reg_write(UC_M68K_REG_SR, sr)
        uc.reg_write(UC_M68K_REG_A7, sp)
        uc.reg_write(UC_M68K_REG_PC, pc)

    def vbl(self):
        """Tic du pilote à chaque trame (le gestionnaire VBL du jeu appelle le vecteur $168), niveau 4 :
        les timers (niveau 6) peuvent l'interrompre."""
        uc = self.uc
        pc, sr, sp = uc.reg_read(UC_M68K_REG_PC), uc.reg_read(UC_M68K_REG_SR), uc.reg_read(UC_M68K_REG_A7)
        frame = sp - 4
        uc.mem_write(frame, struct.pack(">I", IRQ_STOP))
        uc.reg_write(UC_M68K_REG_SR, (sr & ~0x0700) | 0x2400)
        uc.reg_write(UC_M68K_REG_A7, frame)
        self._run_until(self.r32(0x168))
        uc.reg_write(UC_M68K_REG_SR, sr)
        uc.reg_write(UC_M68K_REG_A7, sp)
        uc.reg_write(UC_M68K_REG_PC, pc)

    def step(self, ipl=0, timers_only=False):
        """Avance jusqu'au prochain événement (VBL, niveau 4 ; timer, niveau 6) et le traite s'il n'est pas
        masqué par le niveau d'interruption `ipl` du code en cours."""
        if not hasattr(self, "next_vbl"):
            self.next_vbl = 0.0
        if ipl >= 6:                                 # tout est masqué : l'événement reste en attente
            return
        vbl_ok = not timers_only and ipl < 4
        best, which = (self.next_vbl if vbl_ok else float("inf")), None
        for t in self.timers.values():
            if t.next is not None and t.next < best:
                best, which = t.next, t
        if best == float("inf"):
            return                                   # rien ne peut arriver : on laisse le code continuer
        self.t = max(self.t, best)
        if which is None:
            self.next_vbl += VBL_TICKS
            self.frames = getattr(self, "frames", 0) + 1
            if self.frames % 50 == 0:                # contourne un plantage d'unicorn sur les longues durées
                self.uc.ctl_flush_tb()
            self.vbl()
            return
        which.next += which.period()
        if ipl < 6 and (self.mfp[which.ier] >> which.bit) & 1 and (self.mfp[which.ier + 0x0C] >> which.bit) & 1:
            self.interrupt(which.vector)

    def run_song(self, channel, max_seconds=600.0, tail=1.0):
        """Joue jusqu'à la fin du morceau du canal (0 = titre, 1 = musique) : canal libéré (octet d'état à 1)
        ou retour en arrière dans sa liste de motifs (morceau en boucle : on s'arrête au premier tour).
        Renvoie (durée en secondes, "fin" | "boucle" | "plafond")."""
        base = 0x37A0 + 16 * channel
        start, last = self.t, None
        while self.t - start < max_seconds * MFP_HZ:
            self.run(0.25)
            st = self.uc.mem_read(base, 1)[0]
            pos = struct.unpack(">I", self.uc.mem_read(base + 10, 4))[0]
            if st:
                self.run(tail)
                return (self.t - start) / MFP_HZ, "fin"
            if last is not None and pos < last:
                return (self.t - start) / MFP_HZ, "boucle"
            last = pos
        return (self.t - start) / MFP_HZ, "plafond"

    def run(self, seconds):
        end = self.t + seconds * MFP_HZ
        while self.t < end:
            self.step()

    def init_title(self):
        """Pendant l'intro, seul $18A a été appelé (archive 1, $73E56) : banque du titre sur le canal 0."""
        self.call(self.r32(0x18A))
        self.uc.mem_write(0x17C, b"\0\0")

    def init_game(self):
        """Ordre du jeu : $18A (archive 1 : banque du titre), puis $186 (tables) et $18E (préchargement),
        appelés par l'initialisation de l'archive 0 ($14628)."""
        self.call(self.r32(0x18A))
        self.call(self.r32(0x186))
        self.call(self.r32(0x18E))
        self.uc.mem_write(0x17C, b"\0\0")           # le démarrage (archive 1, $73EE4) active le pilote

    def level_map(self):
        """Niveau voulu par le jeu pour chaque triplet de volumes (A, B, C) : le pilote n'écrit que des
        entrées de sa table de mixage, construite à partir des 3 × 256 volumes de l'archive 0 ($B774) ;
        l'indice de l'entrée est le niveau linéaire (0-255) que les auteurs ont visé pour la puce réelle."""
        base = self.r32(0xB774)
        t = [bytes(self.uc.mem_read(base + 256 * k, 256)) for k in range(3)]
        m = {}
        for i in range(255, -1, -1):
            m[(t[0][i], t[1][i], t[2][i])] = i
        return m

    def levels(self):
        """(temps, niveau 0-255) de la sortie : chaque triplet de volumes YM ramené à son entrée de table."""
        lm = self.level_map()
        fallback = lambda v: 255 * sum(YM_LEVEL[x & 15] for x in v) / 3.0
        vol, lt, lv = [0, 0, 0], [], []
        for tt, r, v in self.events:
            if r in (8, 9, 10):
                vol[r - 8] = v & 15
                if r == 10:                          # le pilote écrit 8, 9 puis 10 : triplet complet
                    tv = tuple(vol)
                    lt.append(tt)
                    lv.append(lm[tv] if tv in lm else fallback(tv))
        return lt, lv

    def render(self, path, rate=44100, t0=0.0):
        """Signal en escalier (niveau constant entre deux changements), moyenné sur chaque échantillon."""
        import numpy as np
        lt, lv = self.levels()
        t = np.asarray(lt, dtype=np.float64)
        v = np.asarray(lv, dtype=np.float64)
        keep = t >= t0
        prev = v[~keep][-1] if (~keep).any() else 0.0
        t = np.concatenate(([t0], t[keep]))
        v = np.concatenate(([prev], v[keep]))
        n = int((self.t - t0) / MFP_HZ * rate)
        edges = t0 + np.arange(n + 1) * (MFP_HZ / rate)
        # intégrale cumulée du signal aux instants de changement, puis aux bords des échantillons
        cum = np.concatenate(([0.0], np.cumsum(v[:-1] * np.diff(t))))
        idx = np.clip(np.searchsorted(t, edges, side="right") - 1, 0, len(t) - 1)
        integ = cum[idx] + v[idx] * (edges - t[idx])
        level = np.diff(integ) / (MFP_HZ / rate) / 255.0
        pcm = np.clip((level - 0.5) * 60000, -32768, 32767).astype("<i2")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(pcm.tobytes())


def render_job(job):
    """Un rendu complet dans un processus : ("titre", 0, s) ou ("musique", n, s), s = None : jusqu'à la fin
    du morceau. Renvoie (fichier, durée, motif d'arrêt)."""
    mode, num, seconds = job
    s = AtariSound()
    if mode == "titre":
        s.init_title()
    else:
        s.init_game()
    s.events.clear()
    t0 = s.t
    if mode == "musique":                        # banque n (archive 60, entrée n), puis séquence 0
        s.uc.mem_write(0x37A0, bytes((1,)))      # libère le canal 0 (musique du titre, prioritaire)
        # en jeu, la zone des échantillons du titre (archive 18) devient le tas des instruments chargés à
        # la demande (archive 61) : $170/$174 = début du tas, $334E = limite (fixée par $186)
        s.uc.mem_write(0x170, struct.pack(">II", 0x501A4, 0x501A4))
        s.call(s.r32(0x15C), num)
        s.call(s.r32(0x164), 0)
    if seconds:
        s.run(seconds)
        how = "durée fixée"
    else:
        _, how = s.run_song(0 if mode == "titre" else 1)
    out = os.path.join(ROOT, "out", "atari", "son", "titre.wav" if mode == "titre" else f"musique_{num:02d}.wav")
    s.render(out, t0=t0)
    return out, (s.t - t0) / MFP_HZ, how


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["titre", "musique", "tout", "appel"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--secondes", type=float, default=None,
                    help="durée fixe (par défaut : jusqu'à la fin du morceau, ou son premier rebouclage)")
    ap.add_argument("--processus", type=int, default=max(1, (os.cpu_count() or 2) - 2),
                    help="mode tout : nombre de rendus simultanés")
    a = ap.parse_args()
    if a.mode == "tout":                         # titre + banques 0-41, en parallèle (un processus par rendu)
        import multiprocessing
        import time
        jobs = [("titre", 0, a.secondes)] + [("musique", n, a.secondes) for n in range(42) if n != 31]
        t = time.time()
        with multiprocessing.Pool(a.processus) as pool:
            for k, (out, dur, how) in enumerate(pool.imap_unordered(render_job, jobs), 1):
                print(f"[{k}/{len(jobs)}] {os.path.relpath(out, ROOT)} : {dur:.0f} s ({how})   "
                      f"[{time.time() - t:.0f} s écoulées]", flush=True)
        return
    if a.mode in ("titre", "musique"):
        out, dur, how = render_job((a.mode, int(a.args[0], 0) if a.args else 0, a.secondes))
        print(f"{out} : {dur:.0f} s ({how})")
        return
    s = AtariSound()                             # appel brut : vecteur de l'API, argument
    s.init_game()
    s.events.clear()
    t0 = s.t
    api, num = int(a.args[0], 0), int(a.args[1], 0)
    s.call(s.r32(api), num)
    s.run(a.secondes or 20)
    out = os.path.join(ROOT, "out", "atari", "son", f"appel_{api:03x}_{num:02d}.wav")
    s.render(out, t0=t0)
    print(out)


if __name__ == "__main__":
    main()
