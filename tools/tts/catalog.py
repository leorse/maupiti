#!/usr/bin/env python3
"""Catalogue Markdown de tous les sons de voix générés : voix, modèle, fichier (lien), texte lu.

Usage :
    python tools/tts/catalog.py [--out out/tts/catalogue.md]

Le catalogue contient les textes du jeu : il reste dans out/ (non versionné).
Sources lues :
    out/pack/voices/<voix>__<style>/   variantes comparées dans le moteur (voices.json, check.csv)
    out/pack/voice/                    première génération Piper « tom » (manifest.json)
    out/tts/refs/                      extraits de référence des voix inventées (Qwen VoiceDesign)
    out/tts/audition/xtts/             les 58 voix studio de XTTS-v2 sur une même phrase
    out/tts/compare/<moteur>/          premier comparatif Piper / Windows
    out/tts/mls_probe/                 essai des locuteurs du modèle Piper mls
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

from generate import speakable  # noqa: E402
from lines import RANGES, SPOKEN_FIRST, SPOKEN_LAST, category, is_spoken  # noqa: E402

QWEN = "Qwen3-TTS-12Hz-1.7B"
AUDITION_TEXT = ("Sue s'est amourachée de Bob. Allez savoir pourquoi ! "
                 "Après les détonations, on est tous sortis devant chez Maguy.")


def duration(path: str) -> str:
    try:
        with wave.open(path) as w:
            return f"{w.getnframes() / w.getframerate():.1f} s"
    except Exception:
        return "?"


def cell(s) -> str:
    return str(s if s is not None else "").replace("\n", " ").replace("|", "\\|")


def link(path: str, base: str) -> str:
    rel = os.path.relpath(path, base).replace(os.sep, "/")
    return f"[{os.path.basename(path)}]({rel})"


def model_of(v: dict) -> str:
    e = v.get("engine", "piper")
    if e == "piper":
        return f"Piper `{v['model']}`" + (f", locuteur {v['speaker']}" if v.get("speaker") is not None else "")
    if e == "xtts":
        return f"XTTS-v2 (coqui-tts), voix studio « {v['speaker']} »"
    if e == "qwen":
        if v["mode"] == "custom":
            return f"{QWEN}-CustomVoice, voix prédéfinie « {v['speaker']} »"
        if v["mode"] == "design":
            return f"{QWEN}-VoiceDesign, voix décrite : « {v['instruct']} »"
        return f"{QWEN}-Base, clone de l'extrait refs/{v['ref']}.wav"
    if e == "pocket":
        return f"Kyutai Pocket TTS `french_24l`, voix « {v['speaker']} »"
    if e == "chatterbox":
        return f"Chatterbox multilingue (v2), imite l'extrait refs/{v['ref']}.wav"
    return e


def style_of(s: dict) -> str:
    p = [f"{k}={v}" for k, v in s.items() if k not in ("id", "label") and v is not None]
    return s["label"] + (f" ({', '.join(p)})" if p else "")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "tts", "catalogue.md"))
    a = ap.parse_args()
    base = os.path.dirname(os.path.abspath(a.out))
    texts = [t["text"] for t in json.load(open(os.path.join(ROOT, "out", "pc", "texts.json"), encoding="utf-8"))]
    cfg = json.load(open(os.path.join(HERE, "voices.json"), encoding="utf-8"))
    directions = cfg.get("directions", {})
    vdir = os.path.join(ROOT, "out", "pack", "voices")
    checks = {}
    cpath = os.path.join(vdir, "check.csv")
    if os.path.exists(cpath):
        for r in csv.DictReader(open(cpath, encoding="utf-8-sig"), delimiter=";"):
            checks[(r["variante"], int(r["id"]))] = r

    out = ["# Catalogue des voix générées", "",
           "Généré par `tools/tts/catalog.py` (à relancer après chaque génération). "
           "Liens relatifs à ce fichier. Contient les textes du jeu : ne pas versionner.", "",
           "## Quels textes doivent être lus ?", "",
           f"Seules les **répliques des personnages (textes {SPOKEN_FIRST}–{SPOKEN_LAST}, 450 répliques)** "
           "étaient parlées dans le jeu d'origine : la réplique n° k du script phonétique est le texte "
           f"{SPOKEN_FIRST} + k. Le personnage qui dit chaque réplique sera connu à l'étape 4 "
           "(logique du jeu) ; voir `docs/personnages.md`.", "",
           "| Textes | Catégorie | Contenu | Lu ? |", "|---|---|---|---|"]
    for lo, hi, cat, desc in RANGES:
        out.append(f"| {lo}–{hi} | {cat} | {desc} | {'**oui**' if cat == 'réplique' else 'non'} |")

    # --- variantes comparées dans le moteur
    rows_total = 0
    variants = []
    for v in cfg["voices"]:
        e = v.get("engine", "piper")
        styles = [s for s in cfg["engines"][e]["styles"] if "styles" not in v or s["id"] in v["styles"]]
        for s in styles:
            d = os.path.join(vdir, f"{v['id']}__{s['id']}")
            files = sorted(f for f in os.listdir(d) if f.endswith(".wav")) if os.path.isdir(d) else []
            if files:
                variants.append((v, s, d, files))
    out += ["", "## 1. Variantes comparées dans le moteur (barre d'outils)", "",
            "Écoute : `engine/build/maupiti.exe --pack out/pack`. « Erreur » = taux d'erreur en caractères "
            "de la retranscription Whisper (intelligibilité) ; « mélodie » = variation de hauteur en "
            "demi-tons (une voix atone varie peu) ; voir `tools/tts/check.py`.", "",
            "| Voix | Style | Modèle | Fichiers | Erreur moy. | Mélodie moy. |", "|---|---|---|---|---|---|"]
    for v, s, d, files in variants:
        c = [checks.get((os.path.basename(d), int(f[:4]))) for f in files]
        c = [x for x in c if x]
        err = f"{100 * sum(float(x['cer']) for x in c) / len(c):.1f} %" if c else "—"
        mel = f"{sum(float(x['f0_std_dt']) for x in c) / len(c):.2f}" if c else "—"
        out.append(f"| [{cell(v['label'])}](#{v['id']}__{s['id']}) | {cell(style_of(s))} | {cell(model_of(v))} "
                   f"| {len(files)} | {err} | {mel} |")
    for v, s, d, files in variants:
        name = os.path.basename(d)
        out += ["", f"### <a id=\"{name}\"></a>{v['label']} — {s['label']}", "",
                f"Modèle : {model_of(v)}. Style : {style_of(s)}.", "",
                "| Texte | Fichier | Durée | Texte lu | Consigne de jeu | Erreur | Retranscription Whisper |",
                "|---|---|---|---|---|---|---|"]
        for f in files:
            i = int(f[:4])
            c = checks.get((name, i), {})
            direction = directions.get(str(i)) if s.get("direct") else ""
            out.append(f"| {i} | {link(os.path.join(d, f), base)} | {duration(os.path.join(d, f))} "
                       f"| {cell(speakable(texts[i]))} | {cell(direction)} "
                       f"| {cell(c.get('cer', ''))} | {cell(c.get('transcription', ''))} |")
            rows_total += 1

    # --- extraits de référence
    rdir = os.path.join(ROOT, "out", "tts", "refs")
    if os.path.isdir(rdir):
        design = {v["id"]: v for v in cfg["voices"] if v.get("mode") == "design"}
        out += ["", "## 2. Extraits de référence des voix inventées", "",
                "Produits une fois par Qwen VoiceDesign à partir d'une description ; servent de modèle aux "
                "voix « Qwen stable » et Chatterbox (aucune personne réelle).", "",
                "| Voix | Fichier | Durée | Description | Texte lu |", "|---|---|---|---|---|"]
        for f in sorted(os.listdir(rdir)):
            if f.endswith(".wav"):
                vid = f[:-4]
                txt = open(os.path.join(rdir, vid + ".txt"), encoding="utf-8").read() \
                    if os.path.exists(os.path.join(rdir, vid + ".txt")) else ""
                out.append(f"| {vid} | {link(os.path.join(rdir, f), base)} | {duration(os.path.join(rdir, f))} "
                           f"| {cell(design.get(vid, {}).get('instruct', ''))} | {cell(txt)} |")
                rows_total += 1

    # --- audition XTTS
    adir = os.path.join(ROOT, "out", "tts", "audition", "xtts")
    if os.path.isdir(adir):
        files = sorted(f for f in os.listdir(adir) if f.endswith(".wav"))
        out += ["", "## 3. Audition des 58 voix studio XTTS-v2", "",
                f"Modèle : XTTS-v2 (coqui-tts), temperature=0.75. Texte lu par toutes : « {AUDITION_TEXT} »", "",
                "| Voix | Fichier | Durée |", "|---|---|---|"]
        for f in files:
            out.append(f"| {f[:-4].replace('_', ' ')} | {link(os.path.join(adir, f), base)} "
                       f"| {duration(os.path.join(adir, f))} |")
            rows_total += 1

    # --- premier comparatif
    cdir = os.path.join(ROOT, "out", "tts", "compare")
    if os.path.isdir(cdir):
        out += ["", "## 4. Premier comparatif (Piper / Windows)", "",
                "| Moteur | Voix | Texte | Catégorie | Fichier | Durée | Texte lu |", "|---|---|---|---|---|---|---|"]
        for sub in sorted(os.listdir(cdir)):
            sd = os.path.join(cdir, sub)
            man = json.load(open(os.path.join(sd, "manifest.json"), encoding="utf-8")) \
                if os.path.exists(os.path.join(sd, "manifest.json")) else {}
            for f in sorted(x for x in os.listdir(sd) if x.endswith(".wav")):
                m = man.get(str(int(f[:4])), {})
                out.append(f"| {cell(m.get('engine', sub))} | {cell(m.get('voice', ''))} | {int(f[:4])} "
                           f"| {category(int(f[:4]))} | {link(os.path.join(sd, f), base)} "
                           f"| {duration(os.path.join(sd, f))} | {cell(m.get('text', ''))} |")
                rows_total += 1

    # --- essai mls
    mdir = os.path.join(ROOT, "out", "tts", "mls_probe")
    if os.path.isdir(mdir):
        files = sorted((f for f in os.listdir(mdir) if f.endswith(".wav")), key=lambda f: int(f[:-4]))
        out += ["", "## 5. Essai des locuteurs Piper `fr_FR-mls-medium` (écarté)", "",
                "Servait à choisir deux locuteurs par hauteur de voix ; le modèle babille et répète des "
                "syllabes, il a été retiré. Phrase d'essai non conservée.", "",
                "| Locuteur | Fichier | Durée |", "|---|---|---|"]
        for f in files:
            out.append(f"| {f[:-4]} | {link(os.path.join(mdir, f), base)} | {duration(os.path.join(mdir, f))} |")
            rows_total += 1

    # --- génération complète tom
    tdir = os.path.join(ROOT, "out", "pack", "voice")
    if os.path.isdir(tdir):
        man = json.load(open(os.path.join(tdir, "manifest.json"), encoding="utf-8"))
        files = sorted(f for f in os.listdir(tdir) if f.endswith(".wav"))
        n_spoken = sum(is_spoken(int(f[:4])) for f in files)
        out += ["", "## 6. Première génération complète : Piper « tom » (voix écartée)", "",
                f"Modèle : Piper `fr_FR-tom-medium`. {len(files)} fichiers, dont seulement {n_spoken} "
                "répliques parlées : le reste (interface, narrateur, documents, quiz) ne doit pas être lu "
                "et sera supprimé à la régénération.", "",
                "| Texte | Catégorie | Fichier | Durée | Texte lu |", "|---|---|---|---|---|"]
        for f in files:
            i = int(f[:4])
            out.append(f"| {i} | {category(i)}{' ✔' if is_spoken(i) else ''} | {link(os.path.join(tdir, f), base)} "
                       f"| {duration(os.path.join(tdir, f))} | {cell(man.get(str(i), {}).get('text', ''))} |")
            rows_total += 1

    os.makedirs(base, exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as fo:
        fo.write("\n".join(out) + "\n")
    print(f"{a.out} : {rows_total} sons")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
