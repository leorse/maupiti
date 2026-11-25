"""Contrôle objectif des variantes de voix (à défaut d'oreille) :

  - intelligibilité : Whisper retranscrit chaque WAV ; taux d'erreur en caractères (CER)
    par rapport au texte attendu (0 % = parfait) ;
  - mélodie : écart-type de la hauteur de voix (F0) en demi-tons ; une voix « atone »
    varie peu (≈ 1,5–2), une voix expressive davantage (≥ 3) ;
  - débit : caractères par seconde.

Usage (dans un environnement avec torch + transformers, ex. third_party/tts/qwen) :
    third_party/tts/qwen/Scripts/python tools/tts/check.py out/pc/texts.json out/pack/voices [--ids 1379-1383]
Écrit out/pack/voices/check.csv et affiche un résumé par variante.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import unicodedata

import numpy as np
import soundfile as sf


def norm(s: str) -> str:
    s = unicodedata.normalize("NFKD", s.lower())
    s = "".join(c for c in s if not unicodedata.combining(c))
    s = re.sub(r"[^a-z0-9 ]+", " ", s)
    return re.sub(r"\s+", " ", s).strip()


def cer(ref: str, hyp: str) -> float:
    a, b = norm(ref), norm(hyp)
    d = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        prev, d[0] = d[0], i
        for j, cb in enumerate(b, 1):
            prev, d[j] = d[j], min(d[j] + 1, d[j - 1] + 1, prev + (ca != cb))
    return d[len(b)] / max(1, len(a))


def f0_track(x: np.ndarray, sr: int) -> np.ndarray:
    """Hauteur par trames de 40 ms (autocorrélation normalisée, 70–400 Hz, trames voisées seulement)."""
    n, hop = int(0.04 * sr), int(0.01 * sr)
    lo, hi = int(sr / 400), int(sr / 70)
    out = []
    for s in range(0, len(x) - n - hi, hop):
        f = x[s:s + n + hi]
        if np.sqrt(np.mean(f[:n] ** 2)) < 0.02:
            continue
        f = f - f.mean()
        ac = np.correlate(f, f[:n], "valid")[lo:hi]                 # ac[k] = Σ f[t] f[t+k]
        c = np.concatenate(([0.0], np.cumsum(f * f)))
        ek = c[np.arange(lo, hi) + n] - c[np.arange(lo, hi)]           # énergie de f[k:k+n]
        r = ac / (np.sqrt(c[n] * ek) + 1e-9)
        k = int(np.argmax(r))
        if r[k] > 0.6:
            out.append(sr / (lo + k))
    return np.array(out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("texts")
    ap.add_argument("voices")
    ap.add_argument("--ids", default="1379-1383")
    ap.add_argument("--model", default="openai/whisper-large-v3-turbo")
    a = ap.parse_args()
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from generate import parse_ids, speakable

    import torch
    from transformers import pipeline
    asr = pipeline("automatic-speech-recognition", model=a.model, torch_dtype=torch.float16, device="cuda:0")
    texts = [t["text"] for t in json.load(open(a.texts, encoding="utf-8"))]
    ids = [i for i in parse_ids(a.ids, len(texts)) if speakable(texts[i])]
    variants = [l.split(";") for l in open(os.path.join(a.voices, "voices.txt"), encoding="utf-8").read().splitlines() if l]
    rows = []
    for d, voice, style in variants:
        for i in ids:
            p = os.path.join(a.voices, d, f"{i:04d}.wav")
            if not os.path.exists(p):
                continue
            x, sr = sf.read(p, dtype="float32")
            x = x if x.ndim == 1 else x.mean(axis=1)
            x16 = np.interp(np.arange(0, len(x), sr / 16000), np.arange(len(x)), x).astype(np.float32)
            hyp = asr({"raw": x16, "sampling_rate": 16000},
                      return_timestamps=True, generate_kwargs={"language": "french", "task": "transcribe"})["text"]
            f0 = f0_track(x, sr)
            st = 12 * np.log2(f0 / np.median(f0)) if len(f0) > 5 else np.array([0.0])
            ref = speakable(texts[i])
            rows.append({"variante": d, "voix": voice, "style": style, "id": i, "cer": round(cer(ref, hyp), 3),
                         "f0_med": round(float(np.median(f0)) if len(f0) else 0, 0),
                         "f0_std_dt": round(float(np.std(st)), 2),
                         "duree_s": round(len(x) / sr, 1), "car_s": round(len(ref) / (len(x) / sr), 1), "transcription": hyp.strip()})
    with open(os.path.join(a.voices, "check.csv"), "w", encoding="utf-8-sig", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]), delimiter=";")
        w.writeheader()
        w.writerows(rows)
    print(f"{'variante':28s} {'CER':>6s} {'F0 Hz':>6s} {'mélodie':>8s} {'car/s':>6s}")
    for d, voice, style in variants:
        r = [x for x in rows if x["variante"] == d]
        if r:
            print(f"{d:28s} {np.mean([x['cer'] for x in r]) * 100:5.1f}% {np.median([x['f0_med'] for x in r]):6.0f}"
                  f" {np.mean([x['f0_std_dt'] for x in r]):8.2f} {np.mean([x['car_s'] for x in r]):6.1f}")


if __name__ == "__main__":
    main()
