"""Services `int 0xFE` du moteur (maup.exe), réimplémentés pour l'oracle.

Chaque service reçoit (oracle, words) : words[0] = dernier paramètre empilé (le plus proche
de SS:CX), words[1] = l'avant-dernier, etc. Il renvoie le résultat DX:AX (entier 32 bits).
Convention Pascal : les paramètres sont empilés dans l'ordre de la déclaration ; un long
(hi, lo) occupe donc deux mots consécutifs, `lo` en premier dans `words`.
Un pointeur est une adresse linéaire 20 bits : hi:lo = (hi << 16) | lo (docs/architecture.md §7).
"""
from __future__ import annotations

import os
import random
import struct

from unicorn.x86_const import UC_X86_REG_SS  # noqa: F401  (réservé aux services à pile)

ZONE_BASE = 0x40000          # zone de données renvoyée par le service 28 (au-dessus de la pile)


def ptr(words, i=0) -> int:
    return (words[i + 1] << 16 | words[i]) & 0xFFFFF


def mem_read(o, addr, n) -> bytes:
    return bytes(o.uc.mem_read(addr, n))


def svc_read_byte(o, w):   return mem_read(o, ptr(w), 1)[0]
TICK_ADDR = 0x565A0                  # compteur de temps (mis à jour par l interruption d horloge du moteur)


INPUT = 0x5658A                      # enregistrement d'entrée : +0 clic, +2 x, +4 y


def svc_read_word(o, w):
    a = ptr(w)
    if a == INPUT + 2 and not o.state.get("events") and o.state.get("auto_clicks"):
        o.state["idle_reads"] = o.state.get("idle_reads", 0) + 1
        if o.state["idle_reads"] >= 50:              # la logique attend une action : clic automatique
            o.state["idle_reads"] = 0
            o.state["auto_clicks"] -= 1
            o.state["events"] = [(*o.state.get("auto_xy", (130, 90)), 1)]
            if o.observer:
                o.observer(-1, list(o.state["events"][0]))
    if a == INPUT + 2 and not o.state.get("events") and o.state.get("stop_when_idle"):
        o.state["idle_polls"] = o.state.get("idle_polls", 0) + 1
        if o.state["idle_polls"] >= 3:               # plus aucun événement : la logique attend, on a tout vu
            o.uc.emu_stop()
    if a == INPUT + 2 and o.state.get("events") and int.from_bytes(mem_read(o, INPUT, 2), "big") == 0:
        x, y, code = o.state["events"].pop(0)      # nouveau clic : le moteur d'origine l'écrit par interruption
        for off, v in ((2, x), (4, y), (0, code)):
            o.uc.mem_write(INPUT + off, v.to_bytes(2, "big"))
    v = int.from_bytes(mem_read(o, a, 2), "big")
    if a == TICK_ADDR:                # horloge simulée : avance d'un tic toutes les tick_every lectures
        n = o.state["tick_reads"] = o.state.get("tick_reads", 0) + 1
        if n % o.state.get("tick_every", 1) == 0:
            o.uc.mem_write(a, ((v + 1) & 0xFFFF).to_bytes(2, "big"))
    return v
def svc_read_long(o, w):   return int.from_bytes(mem_read(o, ptr(w), 4), "big")


def svc_write_byte(o, w):  o.uc.mem_write(ptr(w, 2), bytes([w[0] & 0xFF]))
def svc_write_word(o, w):  o.uc.mem_write(ptr(w, 2), (w[0] & 0xFFFF).to_bytes(2, "big"))
def svc_write_long(o, w):  o.uc.mem_write(ptr(w, 2), ((w[1] << 16 | w[0]) & 0xFFFFFFFF).to_bytes(4, "big"))


def svc_zone_base(o, w):
    """Service 7 : adresse linéaire de DS:0x166 de MAUPITI1 (mot sauvegardé en 0000:03C6)."""
    ds = o.mailbox_ds
    return (ds << 4) + 0x166


def svc_init_memory(o, w):
    return ZONE_BASE


def svc_table_entry_size(o, w):
    """Service 38 : taille de l'entrée n d'une table de mots big-endian cumulés (T[n] - T[n-1])."""
    base, n = ptr(w, 2), w[0]
    t = lambda i: int.from_bytes(mem_read(o, base + 2 * i, 2), "big")
    return (t(n) - t(n - 1)) & 0xFFFF


def svc_table_address(o, w):
    """Service 15 : adresse de l'entrée n d'une table de tailles (additionnée au premier appel).

    Le moteur transforme la table en offsets cumulés une seule fois (drapeau en mémoire moteur)
    puis renvoie base + offset ; l'oracle la transforme sur place à la première utilisation.
    """
    base, n = ptr(w, 2), w[0]
    fixed = o.state.setdefault("fixed_tables", set())
    if base not in fixed:
        size = int.from_bytes(mem_read(o, base, 2), "big")
        count = (size - 2) // 2
        acc = size
        for i in range(1, count + 1):
            acc = (acc + int.from_bytes(mem_read(o, base + 2 * i, 2), "big")) & 0xFFFF
            o.uc.mem_write(base + 2 * i, acc.to_bytes(2, "big"))
        fixed.add(base)
    off = int.from_bytes(mem_read(o, base + 2 * n, 2), "big")
    return base + off


def svc_random(o, w):
    """Service 21 : nombre aléatoire entre deux bornes incluses (les deux mots du haut = bornes)."""
    lo, hi = sorted((w[2] - (1 << 16) if w[2] > 0x7FFF else w[2], w[0] - (1 << 16) if w[0] > 0x7FFF else w[0]))
    return random.Random(o.state.setdefault("rng_seed", 1991) + o.calls).randint(lo, hi) & 0xFFFF


_MAUP = None
GAME_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "originals", "pc")


def _maup_tables():
    """Tables de ressources de maup.exe (segment de données 0xA42) : id -> (groupe, rang) et
    offsets de nœuds par groupe (fichiers G1..G6.CON). Voir le chargeur maup.exe:0x3F9F."""
    global _MAUP
    if _MAUP is None:
        d = open(os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                              "out", "re", "maup.img"), "rb").read()
        base = 0xA420
        ids = struct.unpack("<68H", d[base + 0xA44:base + 0xA44 + 136])
        where, group, rank = {}, 1, 0
        for v in ids:
            if v == 0xFFFF:
                group, rank = group + 1, 0
            else:
                where[v] = (group, rank)
                rank += 1
        sizes = {1: (0x98C, 21), 2: (0x9E0, 6), 3: (0x9F8, 5), 4: (0xA0C, 6), 5: (0xA24, 3), 6: (0xA30, 3)}
        offs = {g: struct.unpack(f"<{n}I", d[base + a:base + a + 4 * n]) for g, (a, n) in sizes.items()}
        _MAUP = (where, offs)
    return _MAUP


def read_resource(res_id: int, sub: int) -> bytes:
    """Octets du sous-bloc `sub` du nœud `res_id` (copie du chargeur d'origine : deux longs
    consécutifs de la table du nœud donnent début et fin, 16 bits bas seulement)."""
    where, offs = _maup_tables()
    group, rank = where[res_id]
    with open(os.path.join(GAME_DIR, f"G{group}.CON"), "rb") as f:
        node = offs[group][rank]
        f.seek(node + 4 * sub)
        a, _, b, _ = struct.unpack("<4H", f.read(8))
        f.seek(node + a)
        return f.read((b - a) & 0xFFFF)


def svc_load_resource(o, w):
    """Service 25 : charge un bloc de ressource à une adresse linéaire (w0 + 0x1e = id)."""
    data = read_resource(w[0] + 0x1E, w[2])
    o.uc.mem_write(ptr(w, 4), data)
    if o.trace:
        print(f"         charge id {w[0] + 0x1E:#x} sous-bloc {w[2]} : {len(data)} octets en {ptr(w, 4):#07x}", flush=True)
    return 0


def svc_load_game(o, w):
    """Service 11 : charge une partie sauvegardée (SAUVEMAU.00x, 2467 octets) en DS:0x166.
    Comme sur une installation neuve, il n'y a pas de sauvegarde : renvoie 0 (échec), sauf si
    o.state["save_file"] désigne un fichier."""
    path = o.state.get("save_file")
    if not path:
        return 0
    o.uc.mem_write(ptr(w, 2), open(path, "rb").read()[:w[0]])
    return 1


def svc_new_game(o, w):
    """Service 40 : nouvelle partie. maup.exe lit SAUP.MAU (état de départ, 0x9A3 octets) à l'adresse
    passée en SS:CX+4, et DEMOFR.MAU (script de démonstration) dans sa propre mémoire."""
    from unicorn.x86_const import UC_X86_REG_CX, UC_X86_REG_SS
    ss, cx = o.reg(UC_X86_REG_SS), o.reg(UC_X86_REG_CX)
    dest = ((o.rw(ss, cx + 6) << 16) | o.rw(ss, cx + 4)) & 0xFFFFF
    o.uc.mem_write(dest, open(os.path.join(GAME_DIR, "SAUP.MAU"), "rb").read()[:0x9A3])
    if o.trace:
        print(f"         nouvelle partie : SAUP.MAU en {dest:#07x}", flush=True)
    return 0


def svc_voice(o, w):
    """Service 30 : voix. L'oracle enregistre (réplique, hauteur) au lieu de parler."""
    o.state.setdefault("voice_log", []).append(list(w))
    return 0


def install(o) -> None:
    o.state = {}
    o.services.update({
        3: svc_read_byte, 4: svc_read_word, 5: svc_read_long,
        31: svc_write_byte, 32: svc_write_word, 33: svc_write_long,
        7: svc_zone_base, 28: svc_init_memory, 38: svc_table_entry_size, 15: svc_table_address,
        21: svc_random, 30: svc_voice, 25: svc_load_resource, 11: svc_load_game, 40: svc_new_game,
    })
