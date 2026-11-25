# Voix — synthèse vocale moderne (POC)

Les voix sont **générées à l'avance** (un WAV par texte) puis jouées par le
moteur quand le texte s'affiche.

## Moteurs

| Moteur | Qualité | Installation |
|---|---|---|
| `piper` (défaut) | voix neuronales, naturelles | Piper + un modèle de voix dans `third_party/piper/` (≈ 85 Mo par voix) |
| `windows` | voix SAPI (Hortense), datée | rien : déjà présente dans Windows |

### Installer Piper (Windows)

```sh
mkdir -p third_party/piper/voices && cd third_party/piper
curl -L -o piper.zip https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_windows_amd64.zip
unzip piper.zip
cd voices
for v in siwis tom; do
  curl -L -o fr_FR-$v-medium.onnx      https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/fr/fr_FR/$v/medium/fr_FR-$v-medium.onnx
  curl -L -o fr_FR-$v-medium.onnx.json https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/fr/fr_FR/$v/medium/fr_FR-$v-medium.onnx.json
done
```

`third_party/` n'est pas versionné. Piper est sous licence MIT ; chaque voix a
sa propre licence (voir la fiche du modèle sur huggingface.co/rhasspy/piper-voices)
— à vérifier avant toute distribution.

## Générer

```sh
# toutes les répliques, voix masculine
python tools/tts/generate.py out/pc/texts.json out/pack/voice --engine piper --voice fr_FR-tom-medium
# quelques textes avec une autre voix
python tools/tts/generate.py out/pc/texts.json out/tts/essai --voice fr_FR-siwis-medium --ids 1579-1588
# voix Windows
python tools/tts/generate.py out/pc/texts.json out/tts/windows --engine windows
```

Seuls les textes modifiés (ou le changement de voix) sont régénérés
(`manifest.json`). Nettoyage du texte avant lecture : retours à la ligne →
espaces, « Ca » → « Ça » (pas de Ç majuscule dans la police d'origine),
textes sans mots ignorés.

## Comparer des voix et des intonations

`tools/tts/voices.json` liste les voix (chacune avec son moteur), les styles
d'intonation de chaque moteur et une consigne de jeu par réplique
(`directions`, utilisée par le style « dirigé » de Qwen).

```sh
python tools/tts/variants.py out/pc/texts.json out/pack --ids 1379-1383 [--engines xtts,qwen]
```

→ `out/pack/voices/<voix>__<style>/NNNN.wav` et `voices.txt` (seules les variantes
présentes y figurent ; les fichiers existants ne sont pas régénérés sans `--force`).
Dans le moteur, la barre d'outils en haut permet de choisir texte, voix et style
et de cliquer sur JOUER (clavier : ↑/↓, Tab, E, Espace ; H masque la barre).
Les styles proposés sont ceux du moteur de la voix choisie.

### Moteurs neuronaux (GPU)

| Moteur | Voix sans clonage | Intonation | Licence |
|---|---|---|---|
| `piper` | 6 voix françaises | `noise_scale`, `noise_w`, `length_scale` | MIT (voix : variable) |
| `xtts` (XTTS-v2, coqui-tts) | 58 voix « studio » multilingues | `temperature`, `speed` | **CPML : non commercial** |
| `qwen` (Qwen3-TTS 1.7B) | 9 voix prédéfinies + voix **décrites en texte** (VoiceDesign) | consigne libre (« ton inquiet… ») | Apache-2.0 |
| `pocket` (Kyutai Pocket TTS) | voix prédéfinies, modèle `french_24l` | `temperature` | voir fiche du modèle |
| `chatterbox` (Resemble AI, multilingue) | aucune : imite un extrait ; on lui donne les voix inventées par Qwen (`out/tts/refs/`) | `exaggeration`, `cfg_weight` | MIT |

Chaque moteur a son environnement Python dans `third_party/tts/<moteur>/`
(dépendances incompatibles entre elles) ; `variants.py` y exécute
`tools/tts/engines/<moteur>_engine.py`. Modèles téléchargés dans `third_party/hf_cache/`
et `third_party/tts_home/` (non versionnés).

```sh
export UV_CACHE_DIR=third_party/uv_cache; cd third_party/tts
uv venv qwen --python 3.12   && uv pip install --python qwen   --torch-backend=auto qwen-tts torch torchaudio soundfile
uv venv pocket --python 3.12 && uv pip install --python pocket --torch-backend=auto pocket-tts torch soundfile
uv venv chatterbox --python 3.11 && uv pip install --python chatterbox --torch-backend=auto chatterbox-tts soundfile "setuptools<81"
# coqui-tts : torch < 2.9 (sinon torchcodec/FFmpeg obligatoire) et transformers 4.57
uv venv xtts --python 3.12 && uv pip install --python xtts --torch-backend=auto coqui-tts "torch==2.8.*" "torchaudio==2.8.*" "transformers>=4.57,<4.58" six soundfile
```

Écouter les 58 voix XTTS sur une même phrase : `out/tts/audition/xtts/`.

### Contrôle objectif

```sh
third_party/tts/qwen/Scripts/python tools/tts/check.py out/pc/texts.json out/pack/voices
```

Whisper retranscrit chaque fichier (taux d'erreur en caractères = intelligibilité),
et la variation de hauteur de la voix en demi-tons mesure la « mélodie » (une
voix atone varie peu). Résultats détaillés dans `out/pack/voices/check.csv`.

Voix Piper : Siwis (F), Jessica (F) et Pierre (H) du modèle `upmc`, Gilles (H).
Installer en plus `fr_FR-upmc-medium` et `fr_FR-gilles-low` comme ci-dessus.
Le modèle multi-locuteurs `fr_FR-mls-medium` a été écarté : il babille et répète
des syllabes en boucle (constaté au contrôle Whisper).

### Constats du POC (répliques 1379–1383)

- Qwen « design » (voix décrite en texte) réinvente la voix à chaque réplique :
  la hauteur de « Qwen homme » varie de 101 à 279 Hz. Pour un personnage, il faut
  décrire la voix une fois (extrait de référence), puis la cloner avec le modèle
  Base : voix « Qwen stable », les mieux notées (4–6 % d'erreur, hauteur constante).
- Chatterbox ajoute parfois des bruits ou répète la fin de phrase ; XTTS a une
  fois ajouté une fin inventée en anglais. Il faudra contrôler chaque fichier
  généré (Whisper) et régénérer les ratés.
- Les noms propres (Sue, Oualassou, Maguy) sont mal prononcés : prévoir un
  lexique de prononciation (« Sue » → « Siou »…).

## Textes à lire et catalogue

Seules les répliques des personnages (textes 1138–1587) sont lues : ce sont,
dans l'ordre, les 450 répliques du script phonétique d'origine
(`lines.py`). `generate.py` s'y limite par défaut (`--all` pour tout).

```sh
python tools/tts/catalog.py      # -> out/tts/catalogue.md : chaque son, sa voix, son modèle, le texte lu
```

Personnages, voix envisagées et prononciation des noms : `docs/personnages.md`.

## Voix des personnages (génération complète)

```bat
generer_voix.bat --refs-only    :: 1. crée les 9 voix de référence (à écouter : voix\refs\perso_*.wav)
generer_voix.bat                :: 2. génère toutes les répliques attribuées (1174 fichiers, reprenable)
```
`tools/tts/generate_characters.py` : une voix décrite par personnage (`personnages.json`), prononciation
des noms (`lexique.json`), un fichier **OGG Vorbis** par couple (personnage, réplique) dans
`voix/personnages/<id>_<nom>/<réplique>.ogg` (environ 5 fois plus petit qu'en WAV), avec barre de
progression, phrase, destination et estimation du temps restant. Les `.wav` laissés par une version
précédente du générateur sont convertis en `.ogg` au lancement, sans rien régénérer (le son de secours
`voix/voix manquante.wav` aussi). Les voix de référence restent en WAV : elles servent d'entrée au clonage.
Qualité Vorbis : `OGG_COMPRESSION` en tête du script. Options : `--only Maguy,Roy`, `--limit N`, `--force`,
`--dry-run`, `--plain`.
Les couples viennent de `voix/repliques.json` (`tools/emu/attribution.py`). Les couples où le personnage se
nomme lui-même (« Anita : J'étais avec Anita ») sont écartés (31). Autres commandes :
`--list` (écrit `out/tts/repliques_personnages.md` : qui dit quoi, fichier, phrase, prononciation),
`--only Anita --candidates 4` (4 voix candidates dans `out/tts/refs_candidates/`),
`--adopt perso_2_3` (la candidate devient la voix du personnage).
`--nettoyer` : met de côté (out/tts/voix_orphelines/) les fichiers qui ne correspondent plus à aucun
couple (après une correction de l'attribution).
`--tous` : **couverture complète**, les 450 répliques parlées pour chacun des 9 personnages (4050
fichiers, environ 5 h) sans tenir compte de l'attribution, qui reste incertaine (voir
`docs/personnages.md`) ; le moteur jouera la voix du personnage que la logique désigne. Les fichiers dont la phrase dite
(lexique) ou la voix ont changé sont régénérés automatiquement.

## Écouter dans le moteur

`engine/build/maupiti.exe --pack out/pack` : ↑/↓ change de texte et le lit,
V coupe/active la voix, R rejoue. `--say N` joue la réplique N et quitte.

## Suite prévue

1. Relier chaque réplique parlée d'origine (script phonétique, 450 répliques)
   à son texte et à son personnage : la logique appelle la voix depuis une
   fonction de `MAUPITI1.EXE` (vers `0x17xx`, appel en `0x191B`) qui choisit la
   hauteur parmi **9 locuteurs** (+3, +3, +2, −1, 0, −2, −2, −1, −3).
2. Une voix par personnage (plusieurs modèles Piper, ou un modèle
   multi-locuteurs comme `fr_FR-upmc-medium`), réglage du débit par personnage.
3. Option « rétro » : rejouer la synthèse d'origine à partir du script
   phonétique et de la banque de phonèmes.
