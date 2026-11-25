#!/usr/bin/env python3
"""Matrice des dialogues, relevée sur la logique d'origine (oracle) avec les vrais libellés.

Pour chaque personnage (indice interne 0..8, celui que la logique passe à l'interrogatoire), on ouvre
l'interrogatoire, on lit les entrées de menu affichées (service 9 : texte et position), on clique sur
chacune et on descend dans les sous-menus. Chaque réponse est rattachée au chemin de libellés qui y mène
(ex. « Votre emploi du temps. > 2 à 3 h PM »), à l'heure de jeu et à l'état utilisés.

    python tools/emu/dialogues.py [--heures 20,36,52] [--sortie out/re/dialogues.json]

Résultat : liste de {personnage, accueil, heure, chemin: [libellés], replique, texte}.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import Oracle, ROOT  # noqa: E402
from probe import SNAP  # noqa: E402

DS = 0x114C0
HEAD = [(120, 5, 1), (120, 5, 1)]       # interrompre l'accueil, puis « Questionner »
TEXTS = [t["text"].replace("\n", " ").strip() for t in
         json.load(open(os.path.join(ROOT, "out", "pc", "texts.json"), encoding="utf-8"))]
QUIET = {297, 375}                     # esquives « temps écoulé » / clic hors liste


def run(o, char, clicks, hour, max_insn=8_000_000):
    """Exécute l'interrogatoire avec une suite de clics ; renvoie (menus dessinés, répliques dites)."""
    o.restore(SNAP)
    o.calls = 0
    o.uc.mem_write(DS + 0x168, hour.to_bytes(2, "little"))
    drawn = []
    o.observer = lambda n, w: drawn.append((w[0] - 1, w[14], w[12])) if n == 9 else None
    o.state.update(events=list(HEAD) + list(clicks), voice_log=[], stop_when_idle=True, idle_polls=0)
    o.call_near(0xADD4, [char, 0], max_insn=max_insn)
    o.observer = None
    return drawn, [w[0] for w in o.state["voice_log"]]


def menu_entries(drawn, after):
    """Entrées de menu dessinées après l'indice `after` du journal : {(x, y): libellé} (texte blanc)."""
    out = {}
    for t, x, y in drawn[after:]:
        if 0 <= t < 1138 and 20 <= y < 176 and 60 <= x < 300:
            out[(x - x % 2, y - y % 8)] = TEXTS[t]
    return out


def explore(o, char, hour, max_depth=3):
    rows, seen_menus = [], set()
    base_drawn, base_voice = run(o, char, [], hour)
    greeting = base_voice[0] if base_voice else None
    todo = [((), ())]                  # (clics, libellés)
    while todo:
        clicks, labels = todo.pop(0)
        drawn, voice = run(o, char, clicks, hour)
        n_before = len(run(o, char, clicks[:-1], hour)[0]) if clicks else 0
        entries = menu_entries(drawn, n_before)
        key = tuple(sorted(entries.values()))
        if len(clicks) >= max_depth or (key in seen_menus and clicks):
            continue
        seen_menus.add(key)
        for (x, y), label in sorted(entries.items(), key=lambda kv: (kv[0][1], kv[0][0])):
            c = clicks + ((x + 6, y + 3, 1),)
            d2, v2 = run(o, char, c, hour)
            said = [u for u in v2[len(voice):] if u not in QUIET] if len(v2) > len(voice) else []
            if said:
                for u in said:
                    rows.append({"personnage": char, "accueil": TEXTS[1137 + greeting] if greeting is not None else "",
                                 "heure": hour, "chemin": list(labels) + [label], "replique": u,
                                 "texte": TEXTS[1137 + u]})   # u : numérotation à partir de 1
            else:
                todo.append((c, labels + (label,)))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--heures", default="20")
    ap.add_argument("--personnages", default="0,1,2,3,4,5,6,7,8")
    ap.add_argument("--sortie", default=os.path.join(ROOT, "out", "re", "dialogues.json"))
    a = ap.parse_args()
    o = Oracle()
    rows = []
    for h in map(int, a.heures.split(",")):
        for c in map(int, a.personnages.split(",")):
            r = explore(o, c, h)
            rows += r
            print(f"heure {h} personnage {c} ({r[0]['accueil'] if r else '?'}) : {len(r)} réponses", flush=True)
            json.dump(rows, open(a.sortie, "w", encoding="utf-8"), ensure_ascii=False, indent=0)


if __name__ == "__main__":
    main()
