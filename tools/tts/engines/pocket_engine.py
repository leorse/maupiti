"""Kyutai Pocket TTS — exécuté dans third_party/tts/pocket/ (rapide, tourne aussi sur CPU).

voice.speaker : voix prédéfinie (estelle, jean, marius, javert, fantine, cosette, ...),
                version française calculée par Kyutai pour le modèle « french_24l ».
style.temperature : 0,3 par défaut ; plus haut = intonation plus variée.
"""
from __future__ import annotations

import torch
from pocket_tts import TTSModel

from _common import Progress, groups, load_jobs, save

LANGUAGE = "french_24l"


def main() -> None:
    jobs = load_jobs()
    torch.manual_seed(1991)
    prog = Progress(len(jobs))
    for temp, tjobs in groups(jobs, lambda j: j["style"].get("temperature")):
        model = TTSModel.load_model(language=LANGUAGE, temp=temp)
        states = {}
        for j in tjobs:
            name = j["voice"]["speaker"]
            if name not in states:
                states[name] = model.get_state_for_audio_prompt(name)
            save(j["out"], model.generate_audio(states[name], j["text"]), model.sample_rate)
            prog.done(j)


if __name__ == "__main__":
    main()
