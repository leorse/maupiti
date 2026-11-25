"""Qwen3-TTS (Apache-2.0) — exécuté dans third_party/tts/qwen/.

voice.mode :
  "custom"  voix prédéfinie du modèle CustomVoice (voice.speaker), consignes de ton possibles
  "design"  voix décrite en texte (voice.instruct) par le modèle VoiceDesign ; attention, la voix
            est réinventée à chaque réplique (hauteur et timbre varient d'une réplique à l'autre)
  "clone"   voix stable : le modèle Base imite l'extrait de référence d'une voix « design »
            (voice.ref) ; pas de consigne de ton possible
style.direct : true => la consigne de jeu de la réplique (directions de voices.json) est ajoutée.

Les voix « design » produisent aussi un extrait de référence out/tts/refs/<id>.wav (+ .txt),
réutilisé par Chatterbox : une voix inventée, sans personne réelle à cloner.
"""
from __future__ import annotations

import os

import torch
from qwen_tts import Qwen3TTSModel

from _common import REF_DIR, Progress, groups, load_jobs, save

MODELS = {"custom": "Qwen/Qwen3-TTS-12Hz-1.7B-CustomVoice", "design": "Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign",
          "clone": "Qwen/Qwen3-TTS-12Hz-1.7B-Base"}
REF_TEXT = ("Bonjour. Je vis ici depuis longtemps, et je connais tout le monde sur cette île. "
            "Demandez-moi ce que vous voulez, je vous répondrai.")


def instruction(job: dict) -> str:
    v, s = job["voice"], job["style"]
    parts = [v.get("instruct", "")] if v["mode"] == "design" else []
    if s.get("direct") and job.get("direction"):
        parts.append(job["direction"])
    if s.get("instruct"):
        parts.append(s["instruct"])
    return " ".join(p.strip().rstrip(".") + "." for p in parts if p)


def main() -> None:
    jobs = load_jobs()
    torch.manual_seed(1991)
    for mode, mjobs in groups(jobs, lambda j: j["voice"]["mode"]):
        model = Qwen3TTSModel.from_pretrained(MODELS[mode], device_map="cuda:0", dtype=torch.bfloat16,
                                              attn_implementation="sdpa")
        print("langues :", model.get_supported_languages(), "| voix :", model.get_supported_speakers(), flush=True)
        if mode == "design":
            os.makedirs(REF_DIR, exist_ok=True)
            for vid, vj in groups(mjobs, lambda j: j["voice"]["id"]):
                ref = os.path.join(REF_DIR, vid + ".wav")
                if not os.path.exists(ref):
                    wavs, sr = model.generate_voice_design(text=REF_TEXT, instruct=vj[0]["voice"]["instruct"],
                                                           language="French")
                    save(ref, wavs[0], sr)
                    open(ref[:-4] + ".txt", "w", encoding="utf-8").write(REF_TEXT)
                    print("  référence", ref, flush=True)
        prompts = {}
        if mode == "clone":
            for ref in {j["voice"]["ref"] for j in mjobs}:
                wav = os.path.join(REF_DIR, ref + ".wav")
                if not os.path.exists(wav):
                    raise SystemExit(f"extrait de référence absent : {wav} (générer d'abord la voix design {ref})")
                prompts[ref] = model.create_voice_clone_prompt(
                    ref_audio=wav, ref_text=open(wav[:-4] + ".txt", encoding="utf-8").read())
        prog = Progress(len(mjobs))
        for j in mjobs:
            kw = dict(text=j["text"], language="French", temperature=j["style"].get("temperature", 0.9))
            if mode == "custom":
                wavs, sr = model.generate_custom_voice(speaker=j["voice"]["speaker"], instruct=instruction(j), **kw)
            elif mode == "clone":
                wavs, sr = model.generate_voice_clone(voice_clone_prompt=prompts[j["voice"]["ref"]], **kw)
            else:
                wavs, sr = model.generate_voice_design(instruct=instruction(j), **kw)
            save(j["out"], wavs[0], sr)
            prog.done(j)
        del model
        torch.cuda.empty_cache()


if __name__ == "__main__":
    main()
