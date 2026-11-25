"""Outils partagés par les scripts de moteur (exécutés dans third_party/tts/<moteur>/).

Chaque script reçoit un fichier JSON : liste de tâches
    {"id": 1379, "text": "...", "out": "chemin.wav", "voice": {...}, "style": {...}, "direction": "..."|null}
où voice/style sont les entrées de tools/tts/voices.json.
"""
from __future__ import annotations

import json
import os
import sys
import time

import numpy as np
import soundfile as sf

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
REF_DIR = os.path.join(ROOT, "out", "tts", "refs")     # extraits de référence synthétiques


def load_jobs() -> list[dict]:
    return json.load(open(sys.argv[1], encoding="utf-8"))


def save(path: str, audio, sr: int, normalize: float = 0.9) -> None:
    """Écrit un WAV 16 bits mono ; crête ramenée à `normalize` (les moteurs ont des niveaux différents)."""
    if hasattr(audio, "detach"):
        audio = audio.detach().float().cpu().numpy()
    a = np.asarray(audio, dtype=np.float32).squeeze()
    if a.ndim > 1:
        a = a.mean(axis=0)
    peak = float(np.abs(a).max()) if a.size else 0.0
    if peak > 0:
        a = a * (normalize / peak)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    sf.write(path, a, sr, subtype="PCM_16")


def groups(jobs: list[dict], key):
    out: dict = {}
    for j in jobs:
        out.setdefault(key(j), []).append(j)
    return out.items()


class Progress:
    def __init__(self, n: int):
        self.n, self.k, self.t0 = n, 0, time.time()

    def done(self, job: dict) -> None:
        self.k += 1
        print(f"  [{self.k}/{self.n}] {job['voice']['id']}__{job['style']['id']} {job['id']:04d}"
              f"  ({time.time() - self.t0:.0f} s)", flush=True)
