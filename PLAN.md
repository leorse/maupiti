# Plan de réécriture

Chaque étape produit un résultat vérifiable avant de passer à la suivante.

| Étape | Objet | État |
|---|---|---|
| 0 | Mise en place du dépôt, originaux en lecture seule, documentation | **fait** |
| 1 | Extraction des disquettes Atari ST (format Lankhor) | **fait** |
| 2 | Décodage des données PC | **fait** |
| 3 | Décodage des données Atari et comparaison PC / Atari | **fait** : images, textes, polices, sons numérisés, musique (pilote son et formats, docs/architecture.md §12) |
| 4 | Compréhension de la logique du jeu | **en cours** : architecture, API moteur, démarrage, interrogatoires complets (matrices, « Argumenter », objets : 445/450 répliques attribuées) ; reste la décompilation du reste de la logique Pascal (déplacements, objets, événements, fins) |
| 5 | Squelette du moteur C++20 / SDL | **fait** (visionneuse : lieux, éclairages, cadre, textes) |
| 6 | Reprogrammation du jeu, morceau par morceau | **en cours** : moteur hybride (logique PC d'origine exécutée par un 8086 intégré, validé sur 219 852 appels ; services graphiques en cours, docs/architecture.md §14) ; démarrage (titre + musique, nouvelle partie, cabine, panneau, horloge, musique du lieu) ; visionneuse devenue navigateur de médias (F1 lieux, F2 musiques, F3 bruitages, F4 sons numérisés, F5 sprites, F6 personnages, F7 objets, F8 documents) |
| 7 | Son : **musique et bruitages Atari** (demande explicite), nouvelles voix | **musique Atari jouée par le moteur** (pilote d'origine émulé : Musashi + MFP + YM), juke-box F2/F3 ; voix des personnages lues par le moteur (OGG, Qwen « stable ») ; sons numérisés décodés (F4) ; reste : bruitages en situation (déclenchés par la logique) |
| 8 | Finitions (options, sous-titres, filtres) | à faire |
| 9 | Distribution (exe + assistant d'extraction) | à faire |

## Étape 1 — résultat
* `tools/extract_atari.py` : 603 entrées extraites (set1), 317/317 pistes saines.
* Deux éditions comparées : 592/603 entrées identiques.

## Étape 2 — résultat
* `tools/extract_pc.py` : 511 images, 1589 textes, 2 polices, script
  phonétique, 29 morceaux AdLib + 28 banques d'instruments.

## Étape 3 — résultat
* `tools/convert_atari.py` : 611 images Atari en PNG, 134 sons numérisés en
  WAV, correspondance des 33 lieux Atari avec les lieux PC, page
  `compare.html`.
* Police du jeu décodée ; tous les glyphes des textes identifiés.
* Musique Atari : ressources `CO` (partitions YM2149) et décodeur localisés.
* Textes et script phonétique identiques sur les deux versions.

## Étape 4 — résultat du premier passage (`docs/architecture.md`)
* Le jeu PC = **logique en Turbo Pascal** (`MAUPITI1.EXE`, résident) +
  **moteur en Turbo C** (`maup.exe`), reliés par `int 0xFE` : **51 services**
  (rôle établi pour une vingtaine : accès mémoire big-endian, textes, voix,
  hasard, musique, attente d'action…).
* `MAUPITI1.EXE` décompressé (LZEXE) : ~180 fonctions de logique.
* Interrogatoires et voix localisés (`docs/architecture.md` §6) : écran
  d'interrogatoire `0x8A43`, fonction « dire » `0x1773`, moteur de règles
  `0x63A3`, table des 9 hauteurs de voix (= 9 personnages dans l'ordre du menu).
* **Oracle** (`tools/emu/`) : la logique d'origine s'exécute dans l'émulateur (chargement
  des blocs de données, état de départ `SAUP.MAU`, horloge simulée) jusqu'à la boucle
  d'attente d'une action du joueur. Format du bloc « logique » décodé
  (`tools/maupiti/pc/logic.py`). Clics injectés, menus d'interrogatoire parcourus :
  **389 répliques sur 450 attribuées** (955 couples personnage/réplique) : l'oracle, puis la lecture directe des
  matrices de la logique (`tools/maupiti/pc/dialogues.py`, bloc 135 = réactions aux objets). Reste 61 répliques
  produites par les blocs de règles (types 1 à 5 du moteur 0x63A3) à décoder.
* Reste : **émulateur oracle** (unicorn) complet qui
  exécute la logique d'origine pour dresser (locuteur, sujet, réplique) puis
  servir de test à la réécriture ; données « logique », état initial, sauvegardes.

## Étape 5 — résultat
* `engine/` (C++20, SDL2, Makefile MSYS2) : décodeur d'images natif,
  paquet de données (`tools/build_pack.py`), écran 320×200 mis à l'échelle,
  cadre de l'interface, police et textes d'origine, jour/soir/nuit.
* Test : `tools/tests/test_engine_decoder.py` — les 350 images décodées par le
  moteur sont identiques, pixel pour pixel, à la référence Python.
* SDL2 au lieu de SDL3 (déjà installé) ; passage à SDL3 possible plus tard.

## Étape 7 — POC voix
* `tools/tts/generate.py` : génération des voix à l'avance (Piper, voix
  neuronales hors ligne ; ou voix Windows), régénération incrémentale.
* Moteur : lecture de la réplique du texte affiché (`src/audio.*`).
* Comparatif de 5 moteurs (24 voix) sur les textes 1379–1383, choix dans la
  barre d'outils du moteur, contrôle Whisper (`tools/tts/check.py`) ;
  retenu : Qwen3-TTS « stable » (voix décrite une fois puis clonée).
* Répliques parlées identifiées : textes 1138–1587 = les 450 répliques du
  script phonétique d'origine, dans l'ordre (`tools/tts/lines.py`). Le reste
  (interface, narrateur, documents, quiz) n'est pas lu.
* Personnages et brouillon de prononciation : `docs/personnages.md` ;
  catalogue des sons générés : `out/tts/catalogue.md` (`tools/tts/catalog.py`).
* Reste (après l'étape 4) : locuteur de chaque réplique, une voix par
  personnage, lexique de prononciation, génération complète et contrôle.
