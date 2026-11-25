#!/usr/bin/env python3
"""Matrice des dialogues de l'interrogatoire, lue dans la logique d'origine, avec ses libellés.

    python tools/emu/matrice.py        -> out/tts/matrice_dialogues.md (contient les textes : non versionné)

Structure (docs/architecture.md §8, §10) :
  accueil                    368 + personnage
  « Votre emploi du temps »  bloc 1 [créneau][personnage]   créneaux = liste du bloc 0x107 (39)
  « Qu'avez-vous remarqué »  bloc 2 [créneau][personnage]
  « Votre avis sur X »       bloc 4 [personnage][X]          X = Maguy … Juste (ordre du menu)
                             bloc 5 lignes 0-1 [Marie, Lucie][personnage]
  sujets (« Argumenter »)    bloc 5 lignes 2-21 [sujet][personnage]   sujets = liste du bloc 0x106 (20)
  bloc 3 (74 lignes), bloc 135 (202 lignes) : [?][personnage], libellés à établir
  réactions communes         voir GENERIC dans attribution.py
Les valeurs de la logique commencent à 1 : valeur u = texte 1137 + u.
Ordre des personnages = ordre du menu (texte 78 + indice) ; vérifié par l'accueil (Bruce « Oui, matelot ? »).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)
from maupiti.pc.logic import parse_blocks, words  # noqa: E402
from attribution import GENERIC, GREETING, NAMES  # noqa: E402

T = [t["text"].replace("\n", " ").strip() for t in
     json.load(open(os.path.join(ROOT, "out", "pc", "texts.json"), encoding="utf-8"))]


def said(u):
    return T[1137 + u] if 0 < u <= 451 else ""


def table(md, title, rows, labels, cell):
    md += [f"## {title}", "", "| | " + " | ".join(NAMES) + " |", "|---|" + "---|" * 9]
    for r, lab in enumerate(labels[:rows]):
        md.append(f"| **{lab}** | " + " | ".join(cell(r, c).replace("|", "/") for c in range(9)) + " |")
    md.append("")


def main():
    B = parse_blocks(open(os.path.join(ROOT, "out", "pc", "raw", "G3", "logic.bin"), "rb").read())
    slots = [T[v - 1] if v else "—" for v in B[0x107]]
    subjects = [T[v - 1] for v in B[0x106]]
    persons = NAMES
    md = ["# Matrice des dialogues (logique d'origine)", "",
          "Généré par `tools/emu/matrice.py`. Lignes = question ou choix du menu, colonnes = personnage qui "
          "répond. Case vide = pas de réponse (la logique tire alors une esquive : « Je n'ai rien remarqué… »).",
          "", "## Accueil", "", "| " + " | ".join(NAMES) + " |", "|" + "---|" * 9,
          "| " + " | ".join(said(GREETING + c) for c in range(9)) + " |", ""]
    w1, w2, w4, w5 = (words(B[n]) for n in (1, 2, 4, 5))
    table(md, "Votre emploi du temps (bloc 1)", len(w1) // 9, slots, lambda r, c: said(w1[r * 9 + c]))
    table(md, "Qu'avez-vous remarqué (bloc 2)", len(w2) // 9, slots, lambda r, c: said(w2[r * 9 + c]))
    table(md, "Votre avis sur… (bloc 4 ; Marie et Lucie : bloc 5)", 11, persons + ["Marie", "Lucie"],
          lambda r, c: said(w4[c * 9 + r]) if r < 9 else said(w5[(r - 9) * 9 + c]))
    table(md, "Sujets de conversation (bloc 5)", 20, subjects, lambda r, c: said(w5[(r + 2) * 9 + c]))
    def when(r):                                   # heure de jeu 20 + r = heures depuis le 30.01 à 12 h
        a = r + 20 + 12
        return f"{['30.01', '31.01', '01.02', '02.02', '03.02'][a // 24]} {a % 24} h"
    w3 = words(B[3])
    table(md, "Que faites-vous là ? (bloc 3 : selon l'heure du jeu)", len(w3) // 9,
          [when(r) for r in range(len(w3) // 9)], lambda r, c: said(w3[r * 9 + c]))
    w135 = words(B[135])
    table(md, "Montrer un objet (bloc 135 ; numéro d'objet de l'inventaire, libellés à établir)", len(w135) // 9,
          [f"objet {r}" for r in range(len(w135) // 9)], lambda r, c: said(w135[r * 9 + c]))
    # règles « Argumenter »
    from maupiti.pc.dialogues import QUESTION_TYPES, rules
    slot_lab = slots
    subj_lab = ["Marie", "Lucie"] + subjects

    def lab(t, e):
        if e is None:
            return "?"
        if t in (1, 2):
            return slot_lab[e] if e < len(slot_lab) else str(e)
        if t == 3:
            return when(e)
        if t == 4:
            return NAMES[e] if e < 9 else str(e)
        if t == 5:
            return subj_lab[e] if e < len(subj_lab) else str(e)
        return str(e)
    md += ["## Argumenter : réponses conditionnelles (moteur de règles 0x63A3)", "",
           "On oppose au personnage, sur une question, la déclaration d'un autre (« contre ») ; ou on le "
           "contraint (tabasser, soudoyer…) : la règle passe à l'étape suivante à chaque fois.", "",
           "| Personnage | Question | Étape | Contre (qui, question) | Déclaration opposée | Nouvelle réponse |",
           "|---|---|---|---|---|---|"]
    for r in rules(open(os.path.join(ROOT, "out", "pc", "raw", "G3", "logic.bin"), "rb").read()):
        who = NAMES[r["personnage"]] if r["personnage"] is not None and r["personnage"] < 9 else "?"
        cw = NAMES[r["contre_personnage"]] if r["contre_personnage"] is not None and r["contre_personnage"] < 9 else "?"
        md.append(f"| {who} | {QUESTION_TYPES[r['type']]} : {lab(r['type'], r['element'])} | {r['etape'] + 1} | "
                  f"{cw}, {QUESTION_TYPES.get(r['contre_type'], r['contre_type'])} : "
                  f"{lab(r['contre_type'], r['contre_element'])} | "
                  f"{said(r['contre_replique'] + 1) if r['contre_replique'] is not None else ''} | "
                  f"{said(r['reponse'] + 1) if r['reponse'] is not None else ''} |")
    md.append("")
    md += ["## Réactions communes à tous les personnages", "",
           "Conditions relevées dans l'interrogatoire (0xADD4) : déjà interrogé (`[0x7E]`), mode d'approche "
           "1/2, heure (`[0x68]` hors 8-22 h : dérangement), lieu du personnage (`[bp-0x33]`), argent, menace.", ""]
    md += [f"* {u} : {said(u)}" for u in GENERIC]
    out = os.path.join(ROOT, "out", "tts", "matrice_dialogues.md")
    open(out, "w", encoding="utf-8").write("\n".join(md) + "\n")
    print(out)


if __name__ == "__main__":
    main()
