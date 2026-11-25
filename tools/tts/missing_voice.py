#!/usr/bin/env python3
"""Crée le son de secours « voix manquante.ogg » : joué par le moteur quand une réplique parlée
n'a pas de fichier de voix (texte 1138-1587 sans son correspondant).

    python tools/tts/missing_voice.py [DOSSIER]        (défaut : voix/)

Voix neutre Piper (fr_FR-siwis-medium), hors ligne. Remplacez le fichier par un autre son si besoin.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from generate import ROOT, run_piper  # noqa: E402

pack = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "voix")      # versionné avec les voix
os.makedirs(pack, exist_ok=True)
wav = os.path.join(pack, "voix manquante.wav")
run_piper([("Voix manquante.", wav)], "fr_FR-siwis-medium")
dest = wav
try:                                    # le moteur lit l'OGG (comme les répliques) ; à défaut, il garde le WAV
    import soundfile as sf
    a, sr = sf.read(wav, dtype="float32")
    dest = os.path.join(pack, "voix manquante.ogg")
    sf.write(dest, a, sr, format="OGG", subtype="VORBIS")
    os.remove(wav)
except Exception:
    pass
print(dest, "créé" if os.path.exists(dest) else "ÉCHEC")
