# Maupiti Island — réécriture native

Réécriture complète, native Windows, du jeu d'aventure *Maupiti Island*
(Lankhor, 1990-1991), avec les graphismes et la musique de la version Atari ST
et de vraies voix de synthèse à la place de la synthèse vocale d'origine.

Le dépôt ne contient **aucune donnée du jeu** : les outils extraient les
ressources depuis vos propres fichiers d'origine (disquettes Atari ST au format
`.STX` et/ou version PC DOS).

## Organisation

| Dossier | Contenu |
|---|---|
| `tools/` | outils d'extraction et de conversion (Python 3, sans dépendance) |
| `docs/` | formats de fichiers, notes de rétro-ingénierie, empreintes des originaux |
| `engine/` | le moteur (C++20 / SDL2) — voir `engine/README.md` |
| `originals/` | vos fichiers d'origine, en lecture seule (non versionné) |
| `out/` | résultats des extractions (non versionné) |

## Extraire les données

```sh
# Version PC (dossier contenant maup.exe, G1.CON … G6.CON, MUS.HUF, SND.HUF)
python tools/extract_pc.py originals/pc out/pc

# Version Atari ST (les deux disquettes .STX)
python tools/extract_atari.py originals/atari/set1/disk_a.stx originals/atari/set1/disk_b.stx out/atari/set1
```

Puis `tools/convert_atari.py` (images, sons, correspondance des lieux) et
`tools/build_pack.py` (données du moteur) : voir `engine/README.md`.

Documentation : `docs/formats.md` (formats de fichiers),
`docs/architecture.md` (architecture du jeu d'origine et API du moteur),
`PLAN.md` (avancement).

## Licence

Le décodeur d'images (`tools/maupiti/gfx.py`) est un port du code de ScummVM
(moteur Mortevielle), sous GPL v3 ou ultérieure ; l'ensemble du projet suit
donc la GPL v3+. *Maupiti Island* reste la propriété de ses ayants droit.
