#!/usr/bin/env python3
"""Explore l'arbre des menus de l'interrogatoire pour chaque personnage et relève les répliques.

    python tools/emu/explore.py [out/re/tree.json] [profondeur] [colonne x]

Un état = suite de clics. On essaie un clic à chaque hauteur (x fixe) ; un écran déjà vu (même empreinte
d'affichage) n'est pas exploré deux fois. Résultat : {personnage: {"sequence": [...], "repliques": [...]}}
où chaque ligne donne le chemin de clics qui produit la réplique.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import Oracle, ROOT  # noqa: E402
from probe import SNAP  # noqa: E402

DS = 0x114C0
HEAD = [(120, 5, 1), (120, 5, 1)]                  # interrompre l'accueil, puis « Questionner »
YS = list(range(12, 190, 4))
X = 200
STATE = {}          # {décalage dans DS: mot} écrit avant chaque essai (ex. {0x168: 58} = heure de jeu)


def run(o, char, path, max_insn=8_000_000, mode=0, head=None):
    o.restore(SNAP)
    o.calls = 0
    o.sig = 0
    o.uc.mem_write(DS + 0x842, bytes([1] * 0x4E))      # tous les sujets / objets / lieux proposés (0x842-0x88F)
    for off, val in STATE.items():
        o.uc.mem_write(DS + off, (val & 0xFFFF).to_bytes(2, "little"))
    o.state.update(events=list(HEAD if head is None else head) + [(X, y, 1) for y in path], voice_log=[], stop_when_idle=True, idle_polls=0)
    o.call_near(0xADD4, [char, mode & 0xFFFF], max_insn=max_insn)
    return o.sig, [(w[0], w[2] if w[2] < 0x8000 else w[2] - 0x10000) for w in o.state["voice_log"]][1:]


def explore(o, char, max_depth=4, mode=0, head=None):
    seen, found, todo = {}, [], [()]
    root_sig, _ = run(o, char, (), mode=mode, head=head)
    seen[root_sig] = ()
    while todo:
        path = todo.pop(0)
        if len(path) >= max_depth:
            continue
        for y in YS:
            sig, voices = run(o, char, path + (y,), mode=mode, head=head)
            for u, pitch in voices:
                found.append({"clics": list(path) + [y], "replique": u, "hauteur": pitch})
            if voices or sig in seen:
                continue
            seen[sig] = path + (y,)
            todo.append(path + (y,))
    return found, len(seen)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "out", "re", "tree.json")
    depth = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    global X
    X = int(sys.argv[3]) if len(sys.argv) > 3 else X          # colonne de clic : 200 (liste de gauche), 260 (liste de droite)
    o = Oracle()
    res = {}
    for c in range(9):
        found, nstates = explore(o, c, depth)
        uniq = sorted({f["replique"] for f in found})
        res[c] = found
        print(f"personnage {c} : {nstates} écrans, {len(found)} réponses, {len(uniq)} répliques distinctes", flush=True)
        json.dump(res, open(out, "w"), indent=0)


if __name__ == "__main__":
    main()
