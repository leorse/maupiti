#!/usr/bin/env python3
"""Génère les voix des textes du jeu avec une synthèse vocale moderne (POC).

Usage :
    python tools/tts/generate.py TEXTS_JSON OUT_DIR [--engine piper|windows]
                                 [--voice NOM] [--ids 1500-1588,35,41 | --all] [--force]

TEXTS_JSON : out/pc/texts.json (produit par extract_pc.py)
OUT_DIR    : dossier de sortie, un fichier NNNN.wav par texte (ex. out/pack/voice)
Par défaut, seules les répliques parlées des personnages sont générées (tools/tts/lines.py) ;
l'interface, le narrateur, les documents et le quiz étaient muets dans le jeu d'origine.

Moteurs :
  piper    voix neuronales hors ligne (third_party/piper/, voir README de tools/tts)
           --voice : nom du modèle, ex. fr_FR-tom-medium (défaut) ou fr_FR-siwis-medium
  windows  voix SAPI installées (ex. « Microsoft Hortense Desktop »), rien à installer

Un fichier manifest.json garde le texte prononcé et le moteur utilisés, pour ne
régénérer que ce qui a changé.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lines import spoken_ids  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PIPER_DIR = os.path.join(ROOT, "third_party", "piper")


def speakable(text: str) -> str | None:
    """Texte du jeu -> texte à prononcer (None si rien à dire)."""
    t = text.replace("\n", " ")
    t = re.sub(r"\bCa\b", "Ça", t)               # pas de Ç majuscule dans la police d'origine
    t = re.sub(r"\s+", " ", t).strip()
    if sum(c.isalpha() for c in t) < 3:          # chiffres, lettres isolées, entrées vides
        return None
    return t


def parse_ids(spec: str | None, n: int) -> list[int]:
    if not spec:
        return list(range(n))
    out = []
    for part in spec.split(","):
        if "-" in part:
            a, b = part.split("-")
            out += range(int(a), int(b) + 1)
        else:
            out.append(int(part))
    return [i for i in out if 0 <= i < n]


def run_piper(jobs: list[tuple[str, str]], voice: str, speaker: int | None = None,
              noise_scale: float | None = None, noise_w: float | None = None,
              length_scale: float | None = None, sentence_silence: float = 0.25) -> None:
    """Réglages d'intonation : noise_scale = variabilité de la prosodie (défaut du modèle 0,667),
    noise_w = variabilité des durées (0,8), length_scale = lenteur du débit (1,0)."""
    exe = os.path.join(PIPER_DIR, "piper", "piper.exe")
    model = os.path.join(PIPER_DIR, "voices", voice + ".onnx")
    if not (os.path.exists(exe) and os.path.exists(model)):
        sys.exit(f"Piper ou la voix {voice} introuvable dans {PIPER_DIR} (voir tools/tts/README.md)")

    def line(t, f):
        d = {"text": t, "output_file": os.path.abspath(f)}
        if speaker is not None:
            d["speaker_id"] = speaker
        return json.dumps(d, ensure_ascii=False) + "\n"

    cmd = [exe, "--model", model, "--json-input", "--sentence_silence", str(sentence_silence)]
    for flag, val in (("--noise_scale", noise_scale), ("--noise_w", noise_w), ("--length_scale", length_scale)):
        if val is not None:
            cmd += [flag, str(val)]
    subprocess.run(cmd, input="".join(line(t, f) for t, f in jobs).encode("utf-8"), check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run_windows(jobs: list[tuple[str, str]], voice: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        lst = os.path.join(tmp, "jobs.json")
        with open(lst, "w", encoding="utf-8") as f:
            json.dump([{"text": t, "file": os.path.abspath(p)} for t, p in jobs], f, ensure_ascii=False)
        ps = (
            "Add-Type -AssemblyName System.Speech;"
            "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer;"
            f"$s.SelectVoice('{voice}');"
            "$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(22050, 16, 1);"
            f"$jobs = Get-Content -Raw -Encoding UTF8 '{lst}' | ConvertFrom-Json;"
            "foreach ($j in $jobs) { $s.SetOutputToWaveFile($j.file, $fmt); $s.Speak($j.text) };"
            "$s.SetOutputToNull()"
        )
        subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps], check=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("texts")
    ap.add_argument("out")
    ap.add_argument("--engine", choices=("piper", "windows"), default="piper")
    ap.add_argument("--voice")
    ap.add_argument("--ids", help="textes à générer (défaut : les 450 répliques parlées, 1138-1587)")
    ap.add_argument("--all", action="store_true", help="tous les textes, interface comprise")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()
    voice = a.voice or ("fr_FR-tom-medium" if a.engine == "piper" else "Microsoft Hortense Desktop")

    texts = [t["text"] for t in json.load(open(a.texts, encoding="utf-8"))]
    os.makedirs(a.out, exist_ok=True)
    man_path = os.path.join(a.out, "manifest.json")
    manifest = json.load(open(man_path, encoding="utf-8")) if os.path.exists(man_path) else {}

    jobs, keys = [], {}
    if a.ids or a.all:
        ids = parse_ids(a.ids, len(texts))
    else:
        ids = spoken_ids()
    for i in ids:
        say = speakable(texts[i])
        if not say:
            continue
        key = hashlib.sha1(f"{a.engine}|{voice}|{say}".encode()).hexdigest()
        wav = os.path.join(a.out, f"{i:04d}.wav")
        if not a.force and manifest.get(str(i), {}).get("key") == key and os.path.exists(wav):
            continue
        jobs.append((say, wav))
        keys[i] = {"key": key, "engine": a.engine, "voice": voice, "text": say}
    print(f"{len(jobs)} répliques à générer avec {a.engine} ({voice})")
    if jobs:
        (run_piper if a.engine == "piper" else run_windows)(jobs, voice)
        manifest.update({str(i): v for i, v in keys.items()})
        with open(man_path, "w", encoding="utf-8") as f:
            json.dump(manifest, f, ensure_ascii=False, indent=1)
    missing = [p for _, p in jobs if not os.path.exists(p)]
    print(f"terminé ; {len(missing)} échec(s)")
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
