# Formats de données de Maupiti Island

État des connaissances après les étapes 1 et 2. Tout ce qui est décrit ici est
implémenté dans `tools/maupiti/` et vérifié sur les fichiers d'origine
(empreintes dans `docs/originals.sha1`).

La version PC est un portage de la version Atari ST : elle charge les données
ST telles quelles (big-endian, palettes ST 3 bits par composante) et les
convertit à l'exécution. Un même décodeur sert donc pour les deux versions.

---

## 1. Disquettes Atari ST (format Lankhor) — `tools/maupiti/atari/`

Images Pasti (`.STX`, lues par `stx.py`). Deux éditions disponibles :

| Jeu | Fichiers | État |
|---|---|---|
| set1 | `originals/atari/set1/disk_a.stx`, `disk_b.stx` (dump 2005) | 317/317 pistes, sommes de contrôle bonnes |
| set2 | `originals/atari/set2/disk1.stx`, `disk2.stx` (dump 2025) | 3 pistes à somme fausse (175, 179, 224) |

Les deux éditions sont deux révisions du programme (archives 0, 2, 6-9, 16
différentes) ; les données graphiques, textes et sons sont identiques.
**set1 est la référence.**

### Pistes (`lankhor_disk.py`)
* Piste 0 face 0 de la disquette 1 : 9 secteurs standard. Secteur 1 = boot
  (68000, recopie les secteurs 2-9 en `$148E8` et y saute). Les secteurs 2-9
  sont le chargeur, qui pilote directement le contrôleur WD1772.
* Toutes les autres pistes : un faux secteur (ID 7, statut FDC `$18`, sert de
  protection) et une zone lue par la commande « Read Track » :

      … A1 A1 … FE 07 TT 07 CH 07 CL xx <5842 octets bruts>

  `TT` = cylindre (+`$80` sur la disquette 2), `CH:CL` = somme 16 bits des 5842
  octets, `xx` ignoré.
* Octets bruts **échappés** : `07 xx` → `xx`. Positions et longueurs sont en
  octets bruts.
* Adressage linéaire : piste logique `L = P // 5842` ; `L // 80` : bit 0 = face,
  bit 1 = disquette ; `L % 80` = cylindre.
* Disquette 2, piste 79 face 1 : secteurs standard (sauvegardes).

### Archives (`archive.py`, `loader.py`)
Le chargeur contient deux tables (trouvées par signature de code) :
`MAP` (n° d'archive demandé → index) et `POS` (66 positions brutes).
L'archive 65 pointerait sur la piste 79 face 0, vide sur l'image (non utilisée ?).

    +0  u8   drapeau compressé      +1 u8 marqueur RLE
    +2  n × (u16 taille décompressée, u16 taille compressée brute), FFFFFFFF
        u8   ignoré
        u8   bits 0-2 = longueur de code minimale, bits 3-7 = bits par longueur
        256 longueurs de code (bitstream, mots de 32 bits)
        u16  taille brute de l'en-tête
    données : entrée i à P + taille_en_tête + Σ tailles compressées précédentes

Huffman canonique (codes les plus longs numérotés depuis 0 ; à longueur égale,
symbole le plus grand d'abord), lu MSB d'abord par mots de 16 bits, puis RLE :
`marqueur n` (n<2) → n+1 marqueurs ; `marqueur n v` → n+1 fois `v`.

---

## 2. Conteneurs PC `.CON` — `tools/maupiti/pc/con.py`

Bloc conteneur = table de n offsets u32 **little-endian** (le premier vaut
4n). Les lieux forment des listes chaînées : `[décor, sprites, suite]`.
`maup.exe` contient aussi la liste des offsets des lieux (DS:`0x989`).

| Fichier | Contenu |
|---|---|
| G1, G2, G6 | chaînes de lieux (décor + banque de sprites) |
| G3 | interface/icônes (0-8), musique AdLib (9), instruments (10), **textes + logique (11)** |
| G4 | portraits/photos (0-7), lieux et sprites (8/…), **banque de phonèmes** (8/28/10/0), **script phonétique** (8/28/10/1/0) |
| G5 | sprites, 77 objets d'inventaire (14/…) |

### Bloc « décor »
`palette A (16 × u16)`, `palette B (16 × u16)`, puis une image avec sa propre
palette (C).
* **A** : vérifiée sur une capture d'écran de la version Atari (cabine du
  bateau, 01:02 du matin, lumière allumée) — c'est la palette par défaut.
* **B** : plus sombre et bleutée (nuit ?).
* **C** : la plus chaude et lumineuse (plein jour ?).
Le choix selon l'heure dépend de la logique du jeu (étape 4/6).

### Banque de sprites
Suite d'images consécutives (position d'origine à l'écran incluse), suivie de
51 octets non encore interprétés (zones / animations ?). Les portraits de G4
ont 34 octets en fin.

---

## 3. Images — `tools/maupiti/gfx.py`

Format Lankhor commun à Mortevielle : port du décodeur de ScummVM
(`engines/mortevielle/graphics.cpp`, **GPL v3+**).

    +0 u8  0 = opaque (palette de 16 mots intégrée), sinon octet +2 = couleur transparente
    +1 u8  nombre de morceaux     [+4 : 30 octets de palette si opaque]
    morceau : u16 lookupBytes, u16 srcSize, u16 x, u16 y, u16 méthode,
              u16 largeur-1, u16 hauteur-1, données
    19 méthodes de parcours (lignes, colonnes, diagonales, zigzag…),
    données en quartets avec table de motifs et répétitions.

**Particularité Maupiti** : les quartets décodés sont dans l'ordre des plans de
bits de l'Atari. L'indice de palette s'obtient en **inversant les 4 bits**
(1→8, 2→4, 3→12…). La même table de permutation existe dans `maup.exe`
(DS:`0xEA6`). Vérifié sur une capture Atari : sans cette inversion les
couleurs sont fausses.

Palettes : mots ST `0x0RGB`, 3 bits par composante.

Banques de sprites des lieux : plusieurs états par emplacement (ex. cabine :
4 éléments × 2 états, porte et placard ouverts/fermés). Les éléments animés
(ventilateur de la cabine) ne sont pas dans la banque du lieu.

---

## 4. Compression `.HUF` (PC) — `tools/maupiti/pc/lzhuf.py`

**LZHUF** (Yoshizaki/Okumura 1988), routine `maup.exe $9C6A` : LZSS (fenêtre
4096, F=60, seuil 2) + Huffman adaptatif. En-tête u32 LE = taille décompressée.

* `MUS.HUF` : 29 flux, offsets dans `maup.exe` DS:`0x2840` → morceaux
  **AdLib `.MUS`** (format Ad Lib Inc., événements MIDI).
* `SND.HUF` : 28 flux, offsets DS:`0x28B8` → **banques d'instruments AdLib**
  (`piano1`, `bdrum1`…).
* G3/9 et G3/10 : un morceau et une banque supplémentaires.

---

### Taille d'une image en plusieurs morceaux (2026-11-24)
Une image Lankhor peut être faite de plusieurs morceaux positionnés. La taille est la **boîte englobante** de
tous les morceaux (et non la somme des largeurs de la première ligne et des hauteurs de la première colonne,
comme dans ScummVM) : les grands portraits Atari (archive 15, 91×132 en 4 à 7 morceaux), leurs bandes
d'animation, le décor de la plage (archive 37 : 256×163, et non 256×119) et une image de l'archive 10 étaient
tronqués. Corrigé dans `tools/maupiti/gfx.py` et `engine/src/lankhor_image.cpp` (350 images identiques).

## 5. Textes — `tools/maupiti/text.py`

* Table : 1589 × u16 BE (longueur de chaque message en octets).
* Données : codes 5 bits ; 0 = fin ; 1-29 alphabet principal ; 30/31 =
  échappement vers les alphabets 2/3. Les 3 alphabets (87 octets) sont dans
  `maup.exe` DS:`0x429` et donnent des **indices de glyphes**.
* Un octet d'alignement précède le message 235 (groupe des objets).
* Correspondance glyphes → caractères établie par le contexte puis vérifiée
  sur le dessin de la police : tous les glyphes utilisés sont identifiés.
* Identique sur Atari (archives 7 = table, 8 = données).

Contenu : verbes, lieux, personnages, sujets d'interrogatoire, objets,
descriptions, réponses des personnages, journal…

---

## 6. Synthèse vocale d'origine — `tools/maupiti/speech.py`

* Script phonétique : 460 offsets u16 BE (table de `0x398` octets) puis mots
  u16 : bits 0-5 phonème (`0x3C` pause courte, `0x3D` pause longue, `0x3E`
  fin), bits 6-15 prosodie. **Indépendant du texte affiché.** Identique sur
  Atari (archive 17).
* Banque de phonèmes : échantillons DPCM 4 bits (table de deltas dans
  `maup.exe` DS:`0x1212`), indexés via les tables DS:`0xF62`/`0xF72`.
* Sorties d'origine : haut-parleur PC (PWM via le timer), AdLib, Sound Blaster
  (DSP en écriture directe).

---

## 7. Polices — `tools/maupiti/font.py`

Deux polices dans `maup.exe` (DS:`0x1522`, 8 px ; DS:`0x1F11`, 7 px), en-tête
de type GEM : +0x4E hauteur, +0x50 nombre de glyphes (98), +0x5C offset du
bitmap, +0x60 octets par ligne, +0x62 table (début, largeur), +0x66 avances.
Bitmap 1 bit/pixel ; sur PC les octets de chaque mot 16 bits sont inversés.
Les glyphes sont numérotés comme dans les textes (a=1, A=0x1B, 0=0x35…).

## 8. Musique et sons Atari

* **Ressources `CO`** (archives 11, 54, 60, 62) : partitions compressées pour
  la puce YM2149. En-tête `"CO"`, u16 largeur, u16 largeur/2, u16 nombre de
  voies, table d'offsets ; décodeur dans l'archive 2 (vers `$0820`), qui
  produit un flux à codes de contrôle `$7E`/`$7F`/`$80`/`$81` pour le lecteur.
  Format du flux et lecteur à étudier à l'étape 7.
* **Sons numérisés** (archive 61, 146 entrées ; l'archive 55 en est un
  sous-ensemble) : PCM 4 bits non signé, deux échantillons par octet (centre
  8). Fréquence d'échantillonnage à confirmer (export provisoire à 8 kHz).
* Archives 4 et 18 : longs échantillons de même nature (présentation ?).

## 9. Correspondance des lieux Atari ↔ PC

`tools/convert_atari.py` l'établit automatiquement (`out/atari/set1/rooms.json`) :
* 29 lieux par géométrie des sprites (positions et tailles identiques sur les
  deux versions ; seuls les décors ont été redessinés) ;
* 3 lieux sans sprites par palettes communes et taille (arc 34, 39, 64) ;
* arc 53 (320×200, palette noire = écran en fondu) sans équivalent ;
* doublons Atari : 13/63, 41/42, 46/47 (variantes d'un même lieu).

## 10. Écran de jeu (Atari)

| Zone | Contenu |
|---|---|
| (0,0)–(320,11) | barre de menu (arc 10/1) |
| (4,12), 256×163 | décor (certains lieux sont moins hauts) |
| x 261–317 | panneau droit : sablier, horloge, logo (arc 10/0) |
| y 176–178 | séparateur |
| (6,179)–(261,200) | zone de texte |

Tout l'écran utilise la palette du lieu (y compris le cadre). L'archive 10/5
est une planche d'icônes et de curseurs ; 10/2 est l'écran titre.

## 11. Inventaire des archives Atari (set1)

Déduit par comparaison d'octets avec la version PC et par la forme des données.

| Archives | Contenu |
|---|---|
| 0, 2, 6 | code et données de programme (différent du PC) |
| 9 | logique du jeu (correspond en partie au bloc « logique » du PC) |
| 1 | code de démarrage 68000 (destination `$73DE8`) |
| 3, 5, 12, 19, 23 | tables / état initial (23 ≈ `SAUP.MAU`) |
| 4, 18 | échantillons longs (présentation ? musique numérisée ?) |
| 7, 8 | **textes** (table + données) |
| 10, 13, 14/56, 15/57, 20, 21, 22 | images : interface, portraits, sprites, objets |
| 16/58 | banque de phonèmes ; 17/59 = script phonétique |
| 24-52, 63, 64 | **lieux** : décor, sprites, petit bloc, zones (4 entrées chacun) |
| 54/60, 62, 11 | **musique** : partitions `CO` pour la puce YM2149 |
| 55 ⊂ 61 | **146 sons numérisés 4 bits** (absents du PC) |

---

## 12. Points ouverts

* Fin des banques de sprites (51 octets) et 4e entrée des lieux Atari (zones ?).
* Bloc « logique » (G3/11 entrée 1 ; Atari archive 9), en-tête de G3/11.
* `SAUP.MAU`, `DEMOFR.MAU`, sauvegardes.
* Flux musical `CO` et lecteur YM2149 ; fréquence des sons numérisés.
* Rôle exact de chacun des 51 services du moteur (docs/architecture.md).
