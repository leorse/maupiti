# Oracle : la logique d'origine dans un émulateur x86-16

`oracle.py` charge `MAUPITI1.EXE` décompressé (adresse 0, donc sans relocations),
lance le programme principal (`0000:EF37`, en-tête LZEXE), laisse la logique se
rendre résidente (`int 21h/31h`) puis reprend à l'adresse que `maup.exe` aurait
utilisée (`0000:03C8`). Les 51 services `int 0xFE` du moteur sont réimplémentés dans
`services.py` ; ceux qui ne le sont pas renvoient 0 (`--trace` les signale).

```sh
python tools/re/unlzexe.py originals/pc/MAUPITI1.EXE out/re/maupiti1.exe
pip install unicorn capstone
python tools/emu/oracle.py --trace --steps 20000000
```

## Modèle mémoire (établi)

Les paramètres « pointeur » des services sont des **adresses linéaires 20 bits du PC**
(`hi:lo` = 0x1658C pour `0001:658C`) : la mémoire de l'oracle suffit, aucune traduction.
Services mémoire à écrire : 3/4/5 (lecture octet / mot / long **big-endian**), 31/32/33
(écriture), 7 (adresse de `DS:0x166`), 28 (zone de données, p. ex. 0x40000), 15/38 (tables),
21 (aléa). Les services graphiques (13, 14, 36, 39…) peuvent renvoyer 0. Voir
`docs/architecture.md` §7.

## État (2026-11-21)

La logique d'origine **démarre et fonctionne** dans l'oracle :
* service 25 (chargement de blocs) fidèle au chargeur de `maup.exe` : l'id `0x2a` donne
  les 41 885 octets du bloc « logique » (identique à `tools/maupiti/pc/logic.py`) ;
* service 11 branché sur `SAUP.MAU` (état de départ, 2467 octets, placé en `DS:0x166` de la
  logique : `[0x166]` = lieu courant) ;
* services mémoire/tables/aléa, horloge simulée (compteur en `0x565A0`, qui avance à chaque
  lecture), graphismes ignorés, voix journalisée (`state["voice_log"]`) ;
* ~3 millions d'appels de services : initialisation, musique (services 46-47), affichages,
  puis la logique **attend une action du joueur** (boucle de lecture des mots d'entrée).

Zone d'entrée : enregistrement en `0x5658A` (pointeur `[0xDC]`) ; x en `+2` (`0x5658C`),
y en `+4` (`0x5658E`), état souris/touche en `+0` ; mots d'horloge en `0x565A0`.

## Exploration des interrogatoires (fait en grande partie)

* `probe.py snapshot` : démarre le jeu (~2 min) et sauve l'état au repos (`out/re/idle.snap`).
* L'interrogatoire (`0xADD4`, arguments `(personnage 0..8, mode)`) est appelé directement ; les
  clics sont injectés dans l'enregistrement d'entrée (`0x5658A` : +0 clic, +2 x, +4 y). Deux
  clics sur « Questionner » (le premier interrompt l'accueil), puis un clic par niveau de menu.
* `explore.py` parcourt l'arbre des menus (empreinte d'affichage pour ne pas revoir un écran) :
  ~0,03 s par essai, 9 personnages en ~7 min. `attribution.py` en tire
  `docs/repliques_par_personnage.md` (numéros seulement) et `out/tts/speakers.json`.
* Résultat : 389 des 450 répliques attribuées (955 couples ; l'essentiel vient de la lecture statique des matrices de la logique, `tools/maupiti/pc/dialogues.py`) ; colonnes de clic 200 et 260 (listes de gauche et de droite).

## Suite
1. Atteindre les 199 répliques restantes : autres modes (montrer/donner/soudoyer/tabasser un
   objet, événements de l'histoire, fins) — mêmes outils, nouveaux écrans à explorer.
2. Banc de test : même scénario sur l'oracle et sur la logique réécrite en C++ (étape 6).

Débogage : `python tools/emu/oracle.py --trace --quiet 3,4,5,15,31,32,33 --steps 150000000 --tail 20`.

Note (plateau) : activer tous les indicateurs de listes (`DS:0x842-0x88F`) ou changer les clics
d'ouverture n'ajoute plus rien ; les 159 répliques restantes dépendent sans doute de l'état de
l'histoire (objets en main, événements, fins), donc de séquences de jeu à rejouer dans l'oracle.
