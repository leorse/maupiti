"""Chatterbox multilingue (Resemble AI, MIT) — exécuté dans third_party/tts/chatterbox/.

Chatterbox n'a pas de voix françaises intégrées : il imite un extrait de référence.
voice.ref : nom d'un extrait de out/tts/refs/ (voix inventées par Qwen3-TTS VoiceDesign,
            donc aucune personne réelle n'est clonée).
style : exaggeration (0,5 par défaut ; plus haut = plus expressif), cfg_weight (0,5 ;
        plus bas = débit plus posé), temperature.
"""
from __future__ import annotations

import os

import torch
from chatterbox.mtl_tts import ChatterboxMultilingualTTS

from _common import REF_DIR, Progress, load_jobs, save


def main() -> None:
    jobs = load_jobs()
    torch.manual_seed(1991)
    model = ChatterboxMultilingualTTS.from_pretrained(device="cuda")
    prog = Progress(len(jobs))
    for j in jobs:
        s = j["style"]
        ref = os.path.join(REF_DIR, j["voice"]["ref"] + ".wav")
        if not os.path.exists(ref):
            raise SystemExit(f"extrait de référence absent : {ref} (générer d'abord les voix qwen « design »)")
        kw = {k: s[k] for k in ("exaggeration", "cfg_weight", "temperature") if k in s}
        wav = model.generate(j["text"], language_id="fr", audio_prompt_path=ref, **kw)
        save(j["out"], wav, model.sr)
        prog.done(j)


if __name__ == "__main__":
    main()
