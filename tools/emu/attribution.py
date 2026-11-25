#!/usr/bin/env python3
"""Attribue les répliques aux personnages à partir de l'exploration de l'oracle (out/re/tree.json).

    python tools/emu/attribution.py

Produit :
    voix/repliques.json       {numéro de réplique: [personnages]} (+ hauteurs), pour la génération des voix
    voix/speakers.txt         la même chose pour le moteur
    docs/repliques_par_personnage.md   numéros seulement (aucun texte du jeu) : versionnable
Numéro de réplique k = texte 1138 + k (voir tools/tts/lines.py) ; la logique, elle, numérote à partir
de 1 (valeur u = k + 1) : les relevés de l'oracle et des matrices sont convertis.
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NAMES = ["Maguy", "Sue", "Anita", "Bruce", "Roy", "Chris", "Bob", "Anton", "Juste"]
PITCH = [3, 3, 2, -1, 0, -2, -2, -1, -3]
GREETING = 368          # valeur logique (à partir de 1) de l'accueil = 368 + indice du personnage
# Réactions conditionnelles communes à tous les personnages (code de l'interrogatoire 0xADD4 et de ses
# appelants) : esquives, dérangement, insistance, argent, menace, refus. Valeurs logiques (à partir de 1).
GENERIC = (289, 290, 291, 296, 297, 298, 299, 359, 360, 361, 362, 363, 364, 365, 367, 377, 378, 379, 380,
           381, 382, 383)


def ranges(nums):
    out, nums = [], sorted(nums)
    i = 0
    while i < len(nums):
        j = i
        while j + 1 < len(nums) and nums[j + 1] == nums[j] + 1:
            j += 1
        out.append(str(nums[i]) if i == j else f"{nums[i]}-{nums[j]}")
        i = j + 1
    return ", ".join(out)


def main():
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    from maupiti.pc.dialogues import static_attribution          # lecture directe des matrices de la logique
    static = static_attribution(open(os.path.join(ROOT, "out", "pc", "raw", "G3", "logic.bin"), "rb").read())
    by = {c: set(static[c]) for c in range(9)}
    from maupiti.pc.dialogues import rules                        # réponses « Argumenter » (conditionnelles)
    for r in rules(open(os.path.join(ROOT, "out", "pc", "raw", "G3", "logic.bin"), "rb").read()):
        if r["reponse"] is not None and r["personnage"] is not None and 0 <= r["personnage"] < 9:
            by[r["personnage"]].add(r["reponse"])
    for name in sorted(os.listdir(os.path.join(ROOT, "out", "re"))):
        if name.startswith("tree") and name.endswith(".json"):          # une exploration par colonne de clic
            for c, v in json.load(open(os.path.join(ROOT, "out", "re", name))).items():
                by[int(c)] |= {f["replique"] - 1 for f in v}     # service 30 : u à partir de 1 -> k = u - 1
    for c in range(9):
        by[c].add(GREETING + c - 1)
        by[c] |= {u - 1 for u in GENERIC}
    owners = {}
    for c, s in by.items():
        for u in s:
            owners.setdefault(u, []).append(c)
    voix = os.path.join(ROOT, "voix")                  # versionné : numéros seulement, aucun texte du jeu
    os.makedirs(voix, exist_ok=True)
    json.dump({str(u): {"personnages": sorted(cs), "hauteurs": [PITCH[c] for c in sorted(cs)]}
               for u, cs in sorted(owners.items())},
              open(os.path.join(voix, "repliques.json"), "w"), indent=1)
    with open(os.path.join(voix, "speakers.txt"), "w") as f:      # pour le moteur
        for u, cs in sorted(owners.items()):
            f.write(f"{u};{','.join(map(str, sorted(cs)))}\n")

    pack = os.path.join(ROOT, "out", "pack")
    if os.path.isdir(pack):                       # pour le moteur : réplique ; personnages qui peuvent la dire
        with open(os.path.join(pack, "speakers.txt"), "w") as f:
            for u, cs in sorted(owners.items()):
                f.write(f"{u};{','.join(map(str, sorted(cs)))}\n")
    reached = set(owners)
    missing = sorted(set(range(450)) - reached)
    shared = [u for u, cs in owners.items() if len(cs) > 1]
    md = ["# Répliques par personnage (relevé de l'oracle)", "",
          "Généré par `tools/emu/attribution.py` à partir de l'exploration des menus d'interrogatoire "
          "(`tools/emu/explore.py`) et des matrices de la logique (`tools/maupiti/pc/dialogues.py`). Numéro de réplique *k* = texte **1138 + k**. "
          "Seuls des numéros figurent ici (aucun texte du jeu).", "",
          f"* {len(reached)} répliques sur 450 atteintes, dont {len(shared)} dites par plusieurs personnages ;",
          f"* {len(missing)} non atteintes (autres modes : montrer, donner, soudoyer, tabasser, "
          "événements, fins… à explorer).", "",
          "| # | Personnage | Hauteur | Nb | Répliques |", "|---|---|---|---|---|"]
    for c in range(9):
        md.append(f"| {c} | {NAMES[c]} | {PITCH[c]:+d} | {len(by[c])} | {ranges(by[c])} |")
    md += ["", "## Répliques non atteintes", "", ranges(missing), "",
           "## Répliques partagées", "",
           "Une réplique partagée est dite par plusieurs personnages avec des voix différentes ; "
           "la génération des voix doit donc produire un fichier par couple (personnage, réplique). "
           "Les formes féminines/masculines écrites (« allé/allée ») se prononcent pareil : "
           "le texte peut servir aux deux.", "",
           ranges(shared)]
    open(os.path.join(ROOT, "docs", "repliques_par_personnage.md"), "w", encoding="utf-8").write("\n".join(md) + "\n")
    print(len(reached), "atteintes ;", len(missing), "non atteintes ;", len(shared), "partagées ;",
          sum(len(s) for s in by.values()), "couples (personnage, réplique)")


if __name__ == "__main__":
    main()
