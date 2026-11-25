#!/usr/bin/env python3
"""Génère les voix des personnages (Qwen3-TTS « stable ») pour toutes les répliques attribuées.

À lancer dans l'environnement Qwen (voir generer_voix.bat) :
    third_party\\tts\\qwen\\Scripts\\python tools\\tts\\generate_characters.py [options]

Entrées : out/pc/texts.json, voix/repliques.json (tools/emu/attribution.py),
          tools/tts/personnages.json (voix), tools/tts/lexique.json (prononciation).
Sortie  : voix/personnages/<id>_<nom>/<réplique>.ogg   (OGG Vorbis, un fichier par couple personnage/réplique, versionné)
          voix/manifest.json                              (reprise : seuls les fichiers manquants ou modifiés sont refaits)

Options : --only Maguy,Roy   --limit N   --refs-only   --force   --dry-run   --plain   --out DOSSIER
Ctrl+C est sans danger : relancez, la génération reprend où elle s'est arrêtée.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
os.environ.setdefault("HF_HOME", os.path.join(ROOT, "third_party", "hf_cache"))
os.environ.setdefault("PYTHONWARNINGS", "ignore")
os.environ.setdefault("TRANSFORMERS_VERBOSITY", "error")

from generate import speakable  # noqa: E402
from lines import SPOKEN_FIRST  # noqa: E402

MODEL_DESIGN = "Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign"
MODEL_BASE = "Qwen/Qwen3-TTS-12Hz-1.7B-Base"
TEMPERATURE = 0.9
REF_DIR = os.path.join(ROOT, "voix", "refs")                    # voix de référence (versionnées)
MANIFEST = os.path.join(ROOT, "voix", "manifest.json")          # suivi de génération (versionné)
CAND_DIR = os.path.join(ROOT, "out", "tts", "refs_candidates")
OGG_COMPRESSION = 0.4      # Vorbis (libsndfile) : 0 = meilleure qualité, 1 = plus petit fichier


def write_list(jobs, ecartes, manifest, lexique):
    """Liste lisible de toutes les répliques : qui dit quoi, dans quel fichier, avec quelle prononciation."""
    out_md = os.path.join(ROOT, "out", "tts", "repliques_personnages.md")
    md = ["# Répliques des personnages : qui dit quoi", "",
          "Généré par `generer_voix.bat --list` (non versionné : contient les textes du jeu). "
          "« dite » = phrase après application de `tools/tts/lexique.json`.", ""]
    by = {}
    for j in jobs:
        by.setdefault(j["perso"]["id"], []).append(j)
    for c, js in sorted(by.items()):
        md += [f"## {js[0]['perso']['nom']} ({len(js)} répliques)", "", "| Réplique | Fichier | État | Phrase | Dite |", "|---|---|---|---|---|"]
        for j in js:
            link = os.path.relpath(j["out"], os.path.dirname(out_md)).replace(os.sep, "/")
            etat = "généré" if os.path.exists(j["out"]) else "à faire"
            dite = "" if j["dit"] == j["affiche"] else j["dit"].replace("|", "/")
            md.append(f"| {j['replique']} (texte {j['texte_id']}) | [{os.path.basename(j['out'])}]({link}) | {etat} | "
                      f"{j['affiche'].replace('|', '/')} | {dite} |")
        md.append("")
    if ecartes:
        md += ["## Écartés : le personnage se nomme lui-même", "", "| Personnage | Réplique | Phrase |", "|---|---|---|"]
        md += [f"| {p['nom']} | {u} | {sh} |" for p, u, sh in ecartes]
    os.makedirs(os.path.dirname(out_md), exist_ok=True)
    open(out_md, "w", encoding="utf-8").write("\n".join(md) + "\n")
    print(f"{rel(out_md)} : {len(jobs)} répliques, {len(ecartes)} écartées")


def write_ogg(path, samples, sr):
    """Écrit un fichier OGG Vorbis (format des répliques lu par le moteur), sans jamais laisser de fichier à moitié écrit."""
    import soundfile as sf
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    sf.write(tmp, samples, sr, format="OGG", subtype="VORBIS", compression_level=OGG_COMPRESSION)
    os.replace(tmp, path)


def convert_wavs(jobs, manifest, dry):
    """Convertit en OGG les répliques déjà générées en .wav (anciennes versions du générateur) et le son de
    secours ; le suivi (manifest) est conservé, le .wav est supprimé. Renvoie le nombre de fichiers concernés."""
    import soundfile as sf
    pairs = [(j["out"][:-4] + ".wav", j["out"]) for j in jobs]
    pairs.append((os.path.join(ROOT, "voix", "voix manquante.wav"), os.path.join(ROOT, "voix", "voix manquante.ogg")))
    n = 0
    for wav, ogg in pairs:
        if not os.path.exists(wav):
            continue
        n += 1
        if dry:
            continue
        if not os.path.exists(ogg):
            a, sr = sf.read(wav, dtype="float32")
            write_ogg(ogg, a, sr)
        entry = manifest.pop(rel(wav), None)
        if entry is not None:
            manifest.setdefault(rel(ogg), entry)
        os.remove(wav)
    if n and not dry:
        json.dump(manifest, open(MANIFEST, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    return n


def rel(p: str) -> str:
    return os.path.relpath(p, ROOT)


def fmt_time(s: float) -> str:
    s = int(max(0, s))
    return f"{s // 3600:02d}:{s % 3600 // 60:02d}:{s % 60:02d}"


def sha(*parts) -> str:
    return hashlib.sha1("|".join(map(str, parts)).encode("utf-8")).hexdigest()[:16]


class Display:
    """Affichage : un bloc de lignes redessiné sur place (ou ligne à ligne avec --plain)."""

    def __init__(self, plain: bool, total: int):
        self.plain, self.total, self.lines = plain, total, 0
        if not plain and os.name == "nt":
            os.system("")                        # active les séquences ANSI de la console Windows
        try:
            self.width = max(60, os.get_terminal_size().columns - 1)
        except OSError:
            self.width = 100

    def bar(self, done: int, width: int = 40) -> str:
        frac = done / self.total if self.total else 1
        n = int(width * frac)
        return "#" * n + "-" * (width - n) + f" {100 * frac:5.1f} %"

    def clip(self, s: str) -> str:
        return s if len(s) <= self.width else s[: self.width - 3] + "..."

    def show(self, rows: list[str]):
        rows = [self.clip(r) for r in rows]
        if self.plain:
            print("\n".join(rows), flush=True)
            return
        if self.lines:
            sys.stdout.write(f"\x1b[{self.lines}A")
        sys.stdout.write("".join(f"\x1b[2K{r}\n" for r in rows))
        sys.stdout.flush()
        self.lines = len(rows)


def apply_lexicon(text, lexique):
    if not lexique:
        return text
    pat = re.compile(r"(" + "|".join(map(re.escape, sorted(lexique, key=len, reverse=True))) + r")")
    return pat.sub(lambda m: lexique[m.group(1)], text)


def load_jobs(args, texts, speakers, persos, lexique):
    only = {x.strip().lower() for x in args.only.split(",")} if args.only else None
    pat = (re.compile(r"\b(" + "|".join(map(re.escape, sorted(lexique, key=len, reverse=True))) + r")\b")
           if lexique else None)
    jobs, ecartes = [], []
    for u, info in sorted(speakers.items(), key=lambda kv: int(kv[0])):
        u = int(u)
        text_id = SPOKEN_FIRST + u
        shown = speakable(texts[text_id])
        if not shown:
            continue
        spoken = pat.sub(lambda m: lexique[m.group(1)], shown) if pat else shown
        for c in info["personnages"]:
            p = persos[c]
            if only and p["nom"].lower() not in only and str(c) not in only:
                continue
            if re.search(r"(?<!\w)" + re.escape(p["nom"]) + r"(?!\w)", shown):
                ecartes.append((p, u, shown))          # un personnage ne se nomme pas lui-même à la troisième personne
                continue
            out = os.path.join(args.out, f"{c}_{p['nom'].lower()}", f"{u:03d}.ogg")
            jobs.append({"perso": p, "replique": u, "texte_id": text_id, "affiche": shown, "dit": spoken, "out": out})
    jobs.sort(key=lambda j: (j["perso"]["id"], j["replique"]))
    return jobs, ecartes


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(ROOT, "voix", "personnages"))
    ap.add_argument("--only", help="personnages (noms ou numéros, séparés par des virgules)")
    ap.add_argument("--limit", type=int, help="ne générer que N fichiers (essai)")
    ap.add_argument("--refs-only", action="store_true", help="créer seulement les voix de référence, pour les écouter")
    ap.add_argument("--force", action="store_true", help="refaire même les fichiers déjà générés")
    ap.add_argument("--dry-run", action="store_true", help="lister ce qui serait généré, sans rien faire")
    ap.add_argument("--list", action="store_true", help="écrire out/tts/repliques_personnages.md (qui dit quoi) et quitter")
    ap.add_argument("--candidates", type=int, metavar="N", help="créer N voix candidates par personnage (--only) à écouter")
    ap.add_argument("--adopt", metavar="NOM_FICHIER", help="adopter une candidate, ex. perso_2_3 : elle devient la voix du personnage")
    ap.add_argument("--plain", action="store_true", help="affichage simple (sans redessin sur place)")
    ap.add_argument("--nettoyer", action="store_true",
                    help="déplacer dans out/tts/voix_orphelines/ les fichiers qui ne correspondent plus à aucun couple")
    ap.add_argument("--tous", action="store_true",
                    help="chaque réplique parlée (450) avec la voix de chacun des 9 personnages, sans tenir compte de "
                         "l'attribution (le moteur jouera la voix du personnage désigné par la logique)")
    args = ap.parse_args()

    cfg = json.load(open(os.path.join(HERE, "personnages.json"), encoding="utf-8"))
    persos = {p["id"]: p for p in cfg["personnages"]}
    ref_text = cfg["texte_de_reference"]
    lexique = {k: v for k, v in json.load(open(os.path.join(HERE, "lexique.json"), encoding="utf-8")).items()
               if not k.startswith("_")}
    texts = [t["text"] for t in json.load(open(os.path.join(ROOT, "out", "pc", "texts.json"), encoding="utf-8"))]
    speakers = json.load(open(os.path.join(ROOT, "voix", "repliques.json"), encoding="utf-8"))
    manifest = json.load(open(MANIFEST, encoding="utf-8")) if os.path.exists(MANIFEST) else {}

    if args.tous:      # couverture complète : toutes les répliques parlées pour tous les personnages, rien d'écarté
        from lines import spoken_ids
        speakers = {str(t - SPOKEN_FIRST): {"personnages": list(range(9))} for t in spoken_ids()}
    jobs, ecartes = load_jobs(args, texts, speakers, persos, lexique)
    if args.tous:
        for p, u, shown in ecartes:            # le nom du personnage peut figurer : on ne présume rien
            jobs.append({"perso": p, "replique": u, "texte_id": SPOKEN_FIRST + u, "affiche": shown,
                         "dit": apply_lexicon(shown, lexique),
                         "out": os.path.join(args.out, f"{p['id']}_{p['nom'].lower()}", f"{u:03d}.ogg")})
        ecartes = []
        jobs.sort(key=lambda j: (j["perso"]["id"], j["replique"]))
    if not args.nettoyer:                      # les anciens .wav deviennent des .ogg (rien n'est régénéré)
        n_wav = convert_wavs(jobs, manifest, dry=args.dry_run or args.list)
        if n_wav:
            print(f"{n_wav} fichier(s) .wav {'à convertir' if args.dry_run or args.list else 'converti(s)'} en .ogg")
    ref_of = lambda p: os.path.join(REF_DIR, f"perso_{p['id']}.wav")
    accent = cfg.get("accent", "")
    full_voice = lambda p: (p["voix"].rstrip(". ") + ". " + accent).strip()     # description réellement envoyée
    ref_sig = lambda p: sha(full_voice(p), ref_text, MODEL_DESIGN, 1991 + p["id"])
    ref_ok = lambda p: (os.path.exists(ref_of(p)) and os.path.exists(ref_of(p)[:-4] + ".sig")
                        and open(ref_of(p)[:-4] + ".sig").read().strip() == ref_sig(p))
    vsig = {p["id"]: sha(full_voice(p), ref_text, MODEL_BASE, TEMPERATURE) for p in persos.values()}
    todo = [j for j in jobs if args.force or not os.path.exists(j["out"])
            or manifest.get(rel(j["out"]), {}).get("cle") != sha(j["dit"], vsig[j["perso"]["id"]])]
    if args.limit:
        todo = todo[: args.limit]
    needed = sorted({j["perso"]["id"] for j in (jobs if args.refs_only else todo)})

    if args.nettoyer:
        import shutil
        wanted = {os.path.normcase(os.path.abspath(j["out"])) for j in jobs}
        moved = 0
        for d in sorted(os.listdir(args.out)) if os.path.isdir(args.out) else []:
            for f in sorted(os.listdir(os.path.join(args.out, d))):
                src = os.path.join(args.out, d, f)
                if f.endswith((".ogg", ".wav")) and os.path.normcase(os.path.abspath(src)) not in wanted:
                    dst = os.path.join(ROOT, "out", "tts", "voix_orphelines", d, f)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.move(src, dst)
                    manifest.pop(rel(src), None)
                    moved += 1
        json.dump(manifest, open(MANIFEST, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
        print(f"{moved} fichier(s) sans couple déplacé(s) dans out/tts/voix_orphelines/")
        return 0
    if args.adopt:
        import shutil
        name = os.path.splitext(args.adopt)[0]
        m = re.fullmatch(r"perso_(\d+)_\d+", name)
        src = os.path.join(CAND_DIR, name + ".wav")
        if not m or not os.path.exists(src):
            sys.exit(f"candidate introuvable : {src}")
        p = persos[int(m.group(1))]
        shutil.copy(src, ref_of(p))
        open(ref_of(p)[:-4] + ".txt", "w", encoding="utf-8").write(ref_text)
        open(ref_of(p)[:-4] + ".sig", "w").write(ref_sig(p))
        print(f"{p['nom']} : voix de référence remplacée par {name}. Ses répliques seront régénérées au prochain lancement "
              f"(generer_voix.bat --only {p['nom']} --force).")
        return 0
    if args.list:
        write_list(jobs, ecartes, manifest, lexique)
        return 0
    print(f"{len(jobs)} répliques attribuées ; {len(todo)} à générer ; sortie : {rel(args.out)}")
    if ecartes:
        print(f"{len(ecartes)} couples écartés : le personnage se nomme lui-même (détail : generer_voix.bat --list)")
    print(f"personnages concernés : {', '.join(persos[c]['nom'] for c in needed) or '-'}")
    if args.dry_run:
        for j in todo[:20]:
            print(f"  {j['perso']['nom']:6s} {j['replique']:3d} -> {rel(j['out'])} : {j['dit'][:70]}")
        print("  ... (dry-run : rien généré)" if len(todo) > 20 else "  (dry-run : rien généré)")
        return 0
    if not todo and not args.refs_only:
        print("Rien à faire : tout est déjà généré (utiliser --force pour refaire).")
        return 0

    import numpy as np
    import soundfile as sf
    import torch

    # qwen_tts importe SoX, utile seulement à un autre modèle (tokenizer 25 Hz) : on fournit un module vide
    # pour éviter les messages « 'sox' n'est pas reconnu » (rien n'est redirigé, la console reste intacte).
    import types
    if "sox" not in sys.modules:
        fake = types.ModuleType("sox")
        fake.Transformer = type("Transformer", (), {})
        sys.modules["sox"] = fake
    from qwen_tts import Qwen3TTSModel

    def normalize(audio):
        a = np.asarray(audio.detach().float().cpu().numpy() if hasattr(audio, "detach") else audio,
                       dtype=np.float32).squeeze()
        peak = float(np.abs(a).max()) if a.size else 0.0
        return a * (0.9 / peak) if peak > 0 else a

    def write_wav(path, audio, sr):          # voix de référence et candidates : WAV (entrée du clonage)
        a = normalize(audio)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".tmp.wav"
        sf.write(tmp, a, sr, subtype="PCM_16")
        os.replace(tmp, path)                                    # jamais de fichier à moitié écrit

    if args.candidates:
        if not args.only:
            sys.exit("--candidates demande --only <personnage>")
        os.makedirs(CAND_DIR, exist_ok=True)
        design = Qwen3TTSModel.from_pretrained(MODEL_DESIGN, device_map="cuda:0", dtype=torch.bfloat16,
                                               attn_implementation="sdpa")
        for c in sorted({j["perso"]["id"] for j in jobs}):
            p = persos[c]
            for k in range(1, args.candidates + 1):
                torch.manual_seed(7000 + 100 * c + k)
                wavs, sr = design.generate_voice_design(text=ref_text, instruct=full_voice(p), language="French")
                write_wav(os.path.join(CAND_DIR, f"perso_{c}_{k}.wav"), wavs[0], sr)
                print(f"  {p['nom']} candidate {k}/{args.candidates} -> {rel(CAND_DIR)}\\perso_{c}_{k}.wav")
        print("\nÉcoutez-les, puis : generer_voix.bat --adopt perso_<n>_<k>   (ex. perso_2_3)")
        return 0

    # --- 1. voix de référence (créées une fois, modifiables)
    missing = [c for c in needed if not ref_ok(persos[c])]
    if missing:
        print(f"\nCréation des voix de référence (nouvelles ou dont la description a changé) : "
              f"{', '.join(persos[c]['nom'] for c in missing)}")
        design = Qwen3TTSModel.from_pretrained(MODEL_DESIGN, device_map="cuda:0", dtype=torch.bfloat16,
                                               attn_implementation="sdpa")
        for k, c in enumerate(missing, 1):
            p = persos[c]
            torch.manual_seed(1991 + c)
            wavs, sr = design.generate_voice_design(text=ref_text, instruct=full_voice(p), language="French")
            write_wav(ref_of(p), wavs[0], sr)
            open(ref_of(p)[:-4] + ".txt", "w", encoding="utf-8").write(ref_text)
            open(ref_of(p)[:-4] + ".sig", "w").write(ref_sig(p))
            print(f"  [{k}/{len(missing)}] {p['nom']:6s} -> {rel(ref_of(p))}")
        del design
        torch.cuda.empty_cache()
    if args.refs_only:
        print("\nVoix de référence prêtes : écoutez voix\\refs\\perso_*.wav, ajustez tools\\tts\\personnages.json "
              "(supprimez le .wav d'un personnage pour le refaire), puis relancez sans --refs-only.")
        return 0

    # --- 2. génération des répliques par clonage des voix de référence
    print("\nChargement du modèle de synthèse...")
    base = Qwen3TTSModel.from_pretrained(MODEL_BASE, device_map="cuda:0", dtype=torch.bfloat16,
                                         attn_implementation="sdpa")
    prompts = {c: base.create_voice_clone_prompt(ref_audio=ref_of(persos[c]), ref_text=ref_text) for c in needed}
    disp = Display(args.plain, len(todo))
    t0, ok, failed, audio_s = time.time(), 0, [], 0.0
    print()
    try:
        for n, j in enumerate(todo, 1):
            p = j["perso"]
            elapsed = time.time() - t0
            eta = elapsed / (n - 1) * (len(todo) - n + 1) if n > 1 else 0
            disp.show([
                f"[{n}/{len(todo)}] {disp.bar(n - 1)}   écoulé {fmt_time(elapsed)}   "
                f"reste ~{fmt_time(eta) if n > 1 else '--:--:--'}",
                f"  personnage : {p['nom']} ({p['id']})   réplique {j['replique']} = texte {j['texte_id']}   voix : {rel(ref_of(p))}",
                f"  phrase     : {j['affiche']}",
                f"  dite       : {j['dit']}" if j["dit"] != j["affiche"] else "  dite       : (identique)",
                f"  destination: {rel(j['out'])}",
                f"  terminés : {ok}   échecs : {len(failed)}   audio produit : {fmt_time(audio_s)}",
            ])
            try:
                torch.manual_seed(int(sha(p["id"], j["replique"]), 16) % (2 ** 31))
                wavs, sr = base.generate_voice_clone(text=j["dit"], language="French",
                                                     voice_clone_prompt=prompts[p["id"]], temperature=TEMPERATURE)
                write_ogg(j["out"], normalize(wavs[0]), sr)          # répliques : OGG Vorbis
                audio_s += len(wavs[0]) / sr
                manifest[rel(j["out"])] = {"cle": sha(j["dit"], vsig[p["id"]]), "texte": j["dit"],
                                           "personnage": p["nom"], "replique": j["replique"],
                                           "duree_s": round(len(wavs[0]) / sr, 2)}
                ok += 1
            except Exception as e:                                # un échec n'arrête pas les autres fichiers
                failed.append((rel(j["out"]), repr(e)))
            if n % 10 == 0 or n == len(todo):
                os.makedirs(os.path.dirname(MANIFEST), exist_ok=True)
                json.dump(manifest, open(MANIFEST, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    except KeyboardInterrupt:
        print("\n\nInterrompu : relancez la même commande pour reprendre.")
    finally:
        os.makedirs(os.path.dirname(MANIFEST), exist_ok=True)
        json.dump(manifest, open(MANIFEST, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    done = ok + len(failed)
    disp.show([f"[{done}/{len(todo)}] {disp.bar(done)}   durée {fmt_time(time.time() - t0)}"])
    print(f"\nTerminé : {ok} fichier(s) générés, {len(failed)} échec(s), {fmt_time(audio_s)} d'audio.")
    if failed:
        log = os.path.join(ROOT, "out", "tts", "voice_chars_echecs.txt")
        open(log, "w", encoding="utf-8").write("\n".join(f"{f}\t{e}" for f, e in failed))
        print(f"Échecs listés dans {rel(log)} ; relancez pour les retenter.")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
