# Moteur

C++20 et SDL2. État : visionneuse (étape 5 du plan) — lieux Atari avec leurs
trois éclairages, cadre de l'interface, textes avec la police d'origine.

## Construire (MSYS2, environnement UCRT64)

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2 make
cd engine && make
```

## Préparer les données puis lancer

```sh
python tools/extract_atari.py originals/atari/set1/disk_a.stx originals/atari/set1/disk_b.stx out/atari/set1
python tools/extract_pc.py originals/pc out/pc
python tools/convert_atari.py out/atari/set1 originals/pc out/pc
python tools/build_pack.py out/atari/set1 originals/pc out/pack
engine/build/maupiti.exe --pack out/pack
```

Touches : ←/→ lieu, 1/2/3 palette A/B/C (A par défaut : vérifiée sur une capture Atari), ↑/↓ texte, S sprites
(premier état / dernier état / tous / aucun), A proportions 4:3 ou pixels carrés,
T écran titre, V voix oui/non, R rejouer, PgPréc/PgSuiv ±10 textes,
Tab/Maj+Tab voix, E style d'intonation, Espace jouer, H barre d'outils
(cliquable : texte, voix, style, JOUER),
F11 ou Alt+Entrée plein écran, Échap quitter.

Voix (POC) : générer d'abord les WAV avec `tools/tts/generate.py` dans
`out/pack/voice/` (voir `tools/tts/README.md`). `--say N` joue la voix du texte N.

Affichage : les décors Atari ont des « réserves » peintes en noir, couvertes
par les sprites ; chaque emplacement a plusieurs états (porte ouverte/fermée…) ; la
visionneuse montre le premier ou le dernier (le vrai choix dépendra de l'état
du jeu, étape 6 ; les éléments animés comme le ventilateur viendront aussi
de la logique). L'image est
affichée en 4:3, comme sur un moniteur de l'époque (pixels 1:1,2).

## Tests

```sh
engine/build/maupiti.exe --pack out/pack --dump-indices out/test_idx
python tools/tests/test_engine_decoder.py out/pack out/test_idx
```

## Organisation

| Fichier | Rôle |
|---|---|
| `src/lankhor_image.*` | décodeur d'images Lankhor (dérivé de ScummVM, GPL v3+) |
| `src/pack.*` | lecture du paquet de données |
| `src/screen.*` | écran logique 320×200, palettes ST, texte, capture BMP |
| `src/audio.*` | lecture des voix (WAV, conversion SDL) |
| `src/main.cpp` | fenêtre SDL, boucle, visionneuse |

## Voix des personnages

Si `voix/personnages/` et `voix/speakers.txt` existent (ou, à défaut, `pack/voice_chars/` et `pack/speakers.txt`) (générés par `generer_voix.bat` et
`tools/emu/attribution.py`), la visionneuse lit pour chaque réplique (textes 1138-1587) la voix du
personnage : **Tab / Maj+Tab** ou les boutons − + de la barre changent de locuteur parmi ceux qui
peuvent la dire (« Maguy (1/3) »). Fichiers `.ogg` (OGG Vorbis, décodés par `third_party/stb_vorbis.c`, domaine public) ou `.wav` : si l'un manque, l'autre extension est essayée. Sans fichier : `voix/voix manquante.ogg`. Les autres textes
(interface, narrateur, documents) restent muets. **B** bascule vers les variantes de test (POC).
`maupiti --pack out/pack --say 1139 [--speaker 0]` joue une réplique et quitte.

## Démarrage

Sans option, le moteur s'ouvre sur l'écran titre Atari ; un clic ou une touche lance une **nouvelle
partie** comme l'original : état de départ `start.sav` (SAUP.MAU), cabine du Brisban, panneau (lampe
« Vous êtes SEUL », forme, horloge à 8 h, AM 31.01), texte « Ça sent le roussi... », fondu d'ouverture.
`--viewer` garde la visionneuse seule ; `--screenshot title|start F.bmp` capture ces écrans.
La musique (Atari) viendra avec l'étape 7.

## Musique et bruitages Atari, juke-box

Le moteur joue la musique et les bruitages de la version Atari ST **en exécutant le pilote son d'origine**
(archive 2, code 68000) avec l'émulateur Musashi (`third_party/musashi`, licence MIT) : 4 timers du MFP,
VBL à 50 Hz, sortie par les volumes de la puce YM (`src/atari_sound.cpp`, docs/architecture.md §12).
Aucune conversion préalable : les archives Atari utiles (`pack/atari/arcNN/`, `pack/atari/map.bin`, copiées
par `tools/build_pack.py`) suffisent ; le rendu coûte environ 1/150 du temps réel.

* Écran titre : musique du titre ; nouvelle partie : musique du lieu (table du jeu, archive 0 en `$BAF0`).
* Navigateur de médias, une page par touche :
  * **F1** lieux (visionneuse : décors, palettes A/B/C, sprites, **animations** (touche N), textes, voix) ;
  * **F2** musiques (titre, banques 0-41, avec les lieux qui les jouent), **F3** bruitages (ambiances sonores des lieux, banques 42-75) :
    Haut/Bas choisir, Entrée jouer, Espace arrêter, clic pour jouer ;
  * **F4** sons numérisés (archive 61, 146 sons, décodés par le pilote ; +/- : fréquence de lecture) ;
  * **F5** sprites et images d'animation de chaque lieu (Haut/Bas lieu, Gauche/Droite image, Entrée : sur le décor) ;
  * **F6** personnages (portrait, grand portrait d'interrogatoire et ses morceaux d'animation) ;
  * **F7** objets de l'inventaire (archive 22) ; **F8** documents, scènes, carte de l'île, interface ;
  * pages d'images : P/O palette d'un autre lieu, 1-3 palette A/B/C, I palette intégrée oui/non. Les
    portraits et objets n'ont pas de palette propre (ils prennent celle du lieu affiché) ; par défaut, celle
    du lieu 2 (archive 24).
* Contrôle hors ligne : `maupiti --pack out/pack --son titre|N|bN|sN fichier.wav [secondes]` (N : musique, bN : bruitage, sN : son numérisé, aN : ambiance du lieu de l'archive N).
* Capture : `--screenshot musiques|bruitages|page4 … page8 fichier.bmp` ; diagnostic : `--anims` (animations chargées par lieu).

## Partie (logique d'origine)

Après l'écran titre, un clic lance la partie (reprend la sauvegarde `sauvegardes/partie.mau` s'il y en a une ;
touche N : nouvelle partie) : la logique de la version PC (pack/pc) est exécutée par le
8086 du moteur (`src/logic`) et pilote l'écran par les services réécrits (`src/game.cpp`), avec les décors,
la musique et les voix Atari / générées. Clic gauche et droit transmis à la logique (menus en haut : Dép,
Invent, Action, Self, Fic). F2-F8 restent disponibles (juke-box et médias).
Contrôles : `--screenshot partie f.bmp` (avec `MAUPITI_CLICKS="x,y;…"`), `--logique N trace.txt`,
`MAUPITI_DEBUG=1` (services non écrits et leurs paramètres).
