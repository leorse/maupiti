"""XTTS-v2 (coqui-tts) — exécuté dans third_party/tts/xtts/.

voice.speaker : une des voix « studio » intégrées au modèle (liste : python xtts_engine.py --list).
style : temperature (0,75 par défaut ; plus haut = plus varié), speed, repetition_penalty.
Licence du modèle : Coqui Public Model License (usage NON commercial uniquement).
"""
from __future__ import annotations

import os
import sys

os.environ.setdefault("COQUI_TOS_AGREED", "1")   # licence CPML, non commerciale

import torch
from TTS.api import TTS

from _common import Progress, load_jobs, save

MODEL = "tts_models/multilingual/multi-dataset/xtts_v2"


def main() -> None:
    tts = TTS(MODEL).to("cuda")
    if sys.argv[1] == "--list":
        print("\n".join(tts.speakers))
        return
    jobs = load_jobs()
    torch.manual_seed(1991)
    prog = Progress(len(jobs))
    sr = tts.synthesizer.output_sample_rate
    for j in jobs:
        s = j["style"]
        kw = {k: s[k] for k in ("temperature", "speed", "repetition_penalty", "top_p") if k in s}
        wav = tts.tts(text=j["text"], speaker=j["voice"]["speaker"], language="fr", **kw)
        save(j["out"], wav, sr)
        prog.done(j)


if __name__ == "__main__":
    main()
