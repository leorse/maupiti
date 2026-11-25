#!/usr/bin/env python3
"""Interroge chaque personnage sur chaque sujet dans l'oracle et relève la réplique dite.

    python tools/emu/answers.py [out/re/answers.json]

Séquence de clics : deux clics sur « Questionner » (le premier interrompt l'accueil), puis un clic
sur la ligne du sujet. Tous les sujets sont rendus disponibles (indicateurs DS:0x842).
Résultat : {personnage: {ligne: [(réplique, hauteur), ...]}}.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import Oracle, ROOT  # noqa: E402
from probe import SNAP  # noqa: E402

DS_LIN = 0x114C0
ROWS_Y = [44, 52, 60, 68] + [84 + 8 * k for k in range(11)]      # 15 lignes de la liste des sujets


def ask(o, char, row_y, max_insn=1_500_000):
    o.restore(SNAP)
    o.calls = 0
    o.uc.mem_write(DS_LIN + 0x842, bytes([1] * 16))
    o.uc.mem_write(DS_LIN + 0x851, bytes([1] * 20))
    o.state["events"] = [(120, 5, 1), (120, 5, 1)] + ([(200, row_y, 1)] if row_y is not None else [])
    o.state["voice_log"] = []
    o.call_near(0xADD4, [char, 0], max_insn=max_insn)
    return [(w[0], w[2] if w[2] < 0x8000 else w[2] - 0x10000) for w in o.state["voice_log"]]


def sweep(o, char, ys=range(40, 172, 2), x=200):
    """Balayage fin en hauteur de la liste des sujets : {y: [répliques dites après l'accueil]}."""
    return {y: [u for u in ask(o, char, y)[1:]] for y in ys}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "out", "re", "answers.json")
    o = Oracle()
    res = {}
    for c in range(9):
        greet = ask(o, c, None)
        res[c] = {"accueil": greet, "sujets": {}}
        res[c]["sujets"] = {y: v for y, v in sweep(o, c).items()}
        got = sorted({u[0] for v in res[c]["sujets"].values() for u in v} - {297})
        print(c, "accueil", greet[0][0], "réponses", got, flush=True)
        json.dump(res, open(out, "w"), indent=1)


if __name__ == "__main__":
    main()
