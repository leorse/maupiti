#!/usr/bin/env python3
"""Génère des variantes de voix (voix × style d'intonation) pour comparer à l'écoute.

Usage :
    python tools/tts/variants.py TEXTS_JSON PACK_DIR --ids 1379-1383
                                 [--engines piper,xtts,...] [--force] [--config tools/tts/voices.json]

Moteurs (voir tools/tts/README.md) :
  piper       third_party/piper/ (exécutable autonome)
  xtts        XTTS-v2 (coqui-tts), voix « studio » intégrées
  qwen        Qwen3-TTS : voix prédéfinies (CustomVoice) et voix décrites en texte (VoiceDesign)
  pocket      Kyutai Pocket TTS, voix prédéfinies
  chatterbox  Chatterbox multilingue (voix de référence = extrait synthétique)
Les moteurs neuronaux tournent chacun dans leur environnement Python
third_party/tts/<moteur>/ ; le script tools/tts/engines/<moteur>_engine.py y est exécuté.

Produit :
    PACK_DIR/voices/<voix>__<style>/NNNN.wav
    PACK_DIR/voices/voices.txt   une ligne par variante disponible : dossier;libellé voix;libellé style
Le moteur du jeu (engine/) lit voices.txt et propose ces variantes au choix.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

from generate import parse_ids, run_piper, speakable  # noqa: E402

ENV_DIR = os.path.join(ROOT, "third_party", "tts")


def env_python(engine: str) -> str:
    exe = os.path.join(ENV_DIR, engine, "Scripts", "python.exe")
    if not os.path.exists(exe):
        exe = os.path.join(ENV_DIR, engine, "bin", "python")
    if not os.path.exists(exe):
        sys.exit(f"environnement {engine} absent : {os.path.join(ENV_DIR, engine)} (voir tools/tts/README.md)")
    return exe


def run_engine(engine: str, jobs: list[dict]) -> None:
    """Exécute tools/tts/engines/<engine>_engine.py dans son environnement, avec la liste de tâches."""
    env = dict(os.environ)
    env.setdefault("HF_HOME", os.path.join(ROOT, "third_party", "hf_cache"))     # modèles téléchargés
    env.setdefault("TTS_HOME", os.path.join(ROOT, "third_party", "tts_home"))    # XTTS (coqui)
    env.setdefault("PYTHONWARNINGS", "ignore")
    env["PYTHONIOENCODING"] = "utf-8"
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "jobs.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(jobs, f, ensure_ascii=False, indent=1)
        subprocess.run([env_python(engine), os.path.join(HERE, "engines", engine + "_engine.py"), path],
                       check=True, env=env)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("texts")
    ap.add_argument("pack")
    ap.add_argument("--ids", required=True)
    ap.add_argument("--engines", help="ne générer que ces moteurs (liste séparée par des virgules)")
    ap.add_argument("--force", action="store_true", help="régénérer même si le fichier existe")
    ap.add_argument("--config", default=os.path.join(HERE, "voices.json"))
    a = ap.parse_args()
    cfg = json.load(open(a.config, encoding="utf-8"))
    directions = {int(k): v for k, v in cfg.get("directions", {}).items() if not k.startswith("_")}
    texts = [t["text"] for t in json.load(open(a.texts, encoding="utf-8"))]
    ids = [i for i in parse_ids(a.ids, len(texts)) if speakable(texts[i])]
    wanted = set(a.engines.split(",")) if a.engines else None
    root = os.path.join(a.pack, "voices")

    variants = []        # (dossier, voix, style)
    per_engine: dict[str, list[dict]] = {}
    for v in cfg["voices"]:
        engine = v.get("engine", "piper")
        styles = cfg["engines"][engine]["styles"]
        if "styles" in v:                       # la voix peut restreindre les styles du moteur
            styles = [s for s in styles if s["id"] in v["styles"]]
        for s in styles:
            d = f"{v['id']}__{s['id']}"
            variants.append((d, v, s))
            if wanted and engine not in wanted:
                continue
            os.makedirs(os.path.join(root, d), exist_ok=True)
            for i in ids:
                out = os.path.abspath(os.path.join(root, d, f"{i:04d}.wav"))
                if a.force or not os.path.exists(out):
                    per_engine.setdefault(engine, []).append(
                        {"id": i, "text": speakable(texts[i]), "out": out, "voice": v, "style": s,
                         "direction": directions.get(i)})

    for engine, jobs in per_engine.items():
        print(f"== {engine} : {len(jobs)} fichier(s)", flush=True)
        if engine == "piper":
            groups: dict[tuple, list] = {}
            for j in jobs:
                v, s = j["voice"], j["style"]
                key = (v["model"], v.get("speaker"), s.get("noise_scale"), s.get("noise_w"), s.get("length_scale"))
                groups.setdefault(key, []).append((j["text"], j["out"]))
            for key, g in groups.items():
                run_piper(g, *key)
        else:
            run_engine(engine, jobs)

    index = []
    for d, v, s in variants:
        n = sum(os.path.exists(os.path.join(root, d, f"{i:04d}.wav")) for i in ids)
        print(f"{d:28s} {n}/{len(ids)}")
        if n:
            index.append(f"{d};{v['label']};{s['label']}")
    with open(os.path.join(root, "voices.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(index) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
