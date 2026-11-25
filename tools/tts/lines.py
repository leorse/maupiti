"""Nature des 1589 textes du jeu : lesquels sont des répliques parlées.

Établi le 2026-11-21 :
  - la réplique n° k du script phonétique d'origine (G4/8/28/10/1/0, 450 répliques
    non vides) est le texte SPOKEN_FIRST + k : corrélation des longueurs 0,87 sur
    l'ensemble, alignement vérifié au début et à la fin. Certaines répliques
    n'étaient prononcées qu'en partie (script plus court que le texte) ;
  - tout le reste (interface, commentaires du narrateur, documents, quiz) était muet.
Le locuteur de chaque réplique est choisi par la logique (MAUPITI1.EXE) : étape 4.
"""
from __future__ import annotations

SPOKEN_FIRST, SPOKEN_LAST = 1138, 1587

# (premier, dernier, catégorie, description)
RANGES = [
    (0, 315, "interface", "verbes, lieux, objets, heures, menus, sujets d'interrogatoire"),
    (316, 797, "commentaire", "remarques du narrateur sur les actions (« Rien de remarquable. »)"),
    (798, 898, "document", "documents lus à l'écran et récits : lettres, journal de bord, échecs, fins de partie"),
    (899, 1002, "scène", "scènes observées (« Bruce fait les cent pas... »)"),
    (1003, 1137, "quiz", "questionnaire final, questions et réponses"),
    (SPOKEN_FIRST, SPOKEN_LAST, "réplique", "réponses des personnages, parlées dans le jeu d'origine"),
]


def category(i: int) -> str:
    for a, b, cat, _ in RANGES:
        if a <= i <= b:
            return cat
    return "vide"


def is_spoken(i: int) -> bool:
    return SPOKEN_FIRST <= i <= SPOKEN_LAST


def speech_index(i: int) -> int | None:
    """Numéro de la réplique dans le script phonétique d'origine."""
    return i - SPOKEN_FIRST if is_spoken(i) else None


def spoken_ids() -> list[int]:
    return list(range(SPOKEN_FIRST, SPOKEN_LAST + 1))
