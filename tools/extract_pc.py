#!/usr/bin/env python3
"""Extrait et convertit les données de la version PC DOS de Maupiti Island.

Usage :
    python tools/extract_pc.py DOSSIER_DU_JEU_PC OUT_DIR

Produit dans OUT_DIR :
    images/Gn/...png        décors (palettes A, B, C — voir docs/formats.md) et sprites, en PNG
    images/index.json       position d'origine, taille, transparence de chaque image
    texts.json              les 1589 messages du jeu
    speech/script.json      script phonétique des répliques (synthèse d'origine)
    speech/phoneme_bank.bin échantillons de phonèmes (DPCM 4 bits, format brut)
    music/mus_NN.mus        morceaux AdLib (format Ad Lib Inc. .MUS)
    music/snd_NN.snd        banques d'instruments AdLib
    raw/...                 blocs non encore interprétés (logique, zones, etc.)
    manifest.json           arborescence complète des fichiers .CON
"""
from __future__ import annotations

import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from maupiti import speech, text  # noqa: E402
from maupiti.font import PC_FONTS, load_pc_font  # noqa: E402
from maupiti.gfx import decode_image, image_size, parse_palette, st_palette_to_rgb  # noqa: E402
from maupiti.pc import lzhuf  # noqa: E402
from maupiti.pc.container import container_offsets, image_run, parse, walk  # noqa: E402
from maupiti.png import write_indexed  # noqa: E402

DS_FILE = 0x80 * 16 + 0xA420            # début du segment de données dans maup.exe
MUS_TABLE, MUS_COUNT = 0x2840, 29       # offsets des morceaux dans MUS.HUF
SND_TABLE, SND_COUNT = 0x28B8, 28       # offsets des banques dans SND.HUF

TEXT_NODE = "G3/11"
SPEECH_SCRIPT_NODE = "G4/8/28/10/1/0"
PHONEME_BANK_NODE = "G4/8/28/10/0"
GREY = [(i * 17, i * 17, i * 17) for i in range(16)]


def safe(path: str) -> str:
    return path.replace("/", "_")


def write(path: str, data: bytes) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


class Extractor:
    def __init__(self, game: str, out: str):
        self.game, self.out = game, out
        self.exe = open(os.path.join(game, "maup.exe"), "rb").read()
        self.image_index: dict[str, dict] = {}
        self.manifest: dict[str, list] = {}
        self.room_paths: set[str] = set()

    def p(self, *parts: str) -> str:
        return os.path.join(self.out, *parts)

    # --- images ----------------------------------------------------------------
    def save_image(self, data: bytes, name: str, rel: str, palette_words, scale=1, force=False) -> dict:
        img = decode_image(data)
        own = img.palette if img.palette and any(img.palette[1:]) and not force else None
        words = own or palette_words
        os.makedirs(self.p("images", rel), exist_ok=True)
        pal = st_palette_to_rgb(words) if words else GREY
        tr = img.transparency if img.transparency < 16 else None
        write_indexed(self.p("images", rel, name + ".png"), img.width, img.height, img.pixels, pal, tr, scale)
        info = {"x": img.offset[0], "y": img.offset[1], "w": img.width, "h": img.height,
                "transparent": tr, "palette": "own" if own else ("context" if words else "grey")}
        self.image_index[f"{rel}/{name}.png"] = info
        return info

    def do_con(self, gname: str) -> None:
        data = open(os.path.join(self.game, f"{gname}.CON"), "rb").read()
        root = parse(gname, data)
        ctx_palette = None
        nodes = []
        for n in walk(root):
            entry = {"path": n.path, "offset": n.offset, "size": len(n.data), "kind": n.kind}
            if n.children:
                nodes.append(entry)
                continue
            if n.path == TEXT_NODE:
                entry["kind"] = "text_bank"
                self.do_texts(n.data)
            elif n.path == SPEECH_SCRIPT_NODE:
                entry["kind"] = "speech_script"
                script = speech.parse_script(n.data)
                os.makedirs(self.p("speech"), exist_ok=True)
                with open(self.p("speech", "script.json"), "w", encoding="utf-8") as f:
                    json.dump([[list(w) for w in u] for u in script], f)
                write(self.p("speech", "script.bin"), n.data)
            elif n.path == PHONEME_BANK_NODE:
                entry["kind"] = "phoneme_bank"
                write(self.p("speech", "phoneme_bank.bin"), n.data)
            elif n.kind == "room":
                rel = gname
                img_data = n.data[64:64 + n.info["image_size"]]
                pals = {"A": parse_palette(n.data, 0), "B": parse_palette(n.data, 32), "C": None}
                img = decode_image(img_data)
                pals["C"] = img.palette
                ctx_palette = pals["A"]
                self.room_paths.add(n.path.rsplit("/", 1)[0])
                for label, words in pals.items():
                    self.save_image(img_data, f"{safe(n.path)}_{label}", rel, words, force=True)
                entry["images"] = [f"{safe(n.path)}_{lbl}.png" for lbl in pals]
            elif n.kind == "images":
                rel = f"{gname}/{safe(n.path)}"
                run = image_run(n.data)
                files = []
                for k, (off, size) in enumerate(run):
                    chunk = n.data[off:off + size]
                    if chunk[0] == 0:
                        pw = [struct.unpack_from(">H", chunk, 2)[0]] + list(struct.unpack_from(">15H", chunk, 4))
                        if any(pw[1:]):
                            ctx_palette = pw if ctx_palette is None else ctx_palette
                    in_room = n.path.rsplit("/", 1)[0] in self.room_paths
                    self.save_image(chunk, f"{k:03d}", rel, ctx_palette, force=in_room)
                    files.append(f"{k:03d}.png")
                end = run[-1][0] + run[-1][1]
                if end < len(n.data):
                    write(self.p("raw", gname, safe(n.path) + "_trailer.bin"), n.data[end:])
                    entry["trailer"] = len(n.data) - end
                entry["images"] = len(files)
            elif n.kind == "huf":
                out = lzhuf.decompress(n.data)
                entry["unpacked"] = len(out)
                write(self.p("raw", gname, safe(n.path) + ".unhuf.bin"), out)
                if out[:2] == b"\x01\x00" and b"\x00\x00" in out[2:6]:
                    write(self.p("music", f"{safe(n.path)}.bin"), out)
            elif len(n.data):
                write(self.p("raw", gname, safe(n.path) + ".bin"), n.data)
            nodes.append(entry)
        self.manifest[gname] = nodes

    # --- textes ----------------------------------------------------------------
    def do_texts(self, bank: bytes) -> None:
        # bloc 11 de G3 : 172 octets inconnus | table (1589 × u16) | conteneur [textes, logique]
        table_off, n_texts = 172, 1589
        table = bank[table_off:table_off + 2 * n_texts]
        sub = bank[table_off + 2 * n_texts:]
        offs = container_offsets(sub)
        if not offs or len(offs) != 2:
            raise ValueError("structure inattendue du bloc de textes")
        data, logic = sub[offs[0]:offs[1]], sub[offs[1]:]
        alphabet = text.load_alphabet(self.exe)
        texts = text.decode_bank(table, data, alphabet)
        with open(self.p("texts.json"), "w", encoding="utf-8") as f:
            json.dump([{"id": i, "text": t} for i, t in enumerate(texts)], f, ensure_ascii=False, indent=1)
        with open(self.p("texts.txt"), "w", encoding="utf-8") as f:
            for i, t in enumerate(texts):
                f.write(f"{i:4d}\t{t.replace(chr(10), ' / ')}\n")
        write(self.p("raw", "G3", "texts_prefix.bin"), bank[:table_off])
        write(self.p("raw", "G3", "logic.bin"), logic)

    # --- musique ---------------------------------------------------------------
    def do_music(self) -> None:
        for fname, table, count, prefix in (("MUS.HUF", MUS_TABLE, MUS_COUNT, "mus"),
                                            ("SND.HUF", SND_TABLE, SND_COUNT, "snd")):
            data = open(os.path.join(self.game, fname), "rb").read()
            offs = [struct.unpack_from("<I", self.exe, DS_FILE + table + 4 * i)[0] for i in range(count + 1)]
            if offs[-1] != len(data):
                raise ValueError(f"table d'offsets de {fname} incohérente")
            for i in range(count):
                out = lzhuf.decompress(data[offs[i]:offs[i + 1]])
                ext = "mus" if prefix == "mus" else "snd"
                write(self.p("music", f"{prefix}_{i:02d}.{ext}"), out)

    def do_fonts(self) -> None:
        os.makedirs(self.p("fonts"), exist_ok=True)
        for name in PC_FONTS:
            f = load_pc_font(self.exe, name)
            write_indexed(self.p("fonts", name + ".png"), f.width, f.height, f.bitmap,
                          [(0, 0, 0), (255, 255, 255)], transparent=0)
            meta = {"height": f.height, "atlas": name + ".png",
                    "glyphs": {str(i + 1): {"x": x, "w": w, "advance": f.advances[i],
                                            "char": text.GLYPHS.get(i + 1)}
                               for i, (x, w) in enumerate(f.glyphs)}}
            with open(self.p("fonts", name + ".json"), "w", encoding="utf-8") as fp:
                json.dump(meta, fp, ensure_ascii=False, indent=1)

    def run(self) -> None:
        self.do_fonts()
        for g in range(1, 7):
            self.do_con(f"G{g}")
        self.do_music()
        for f in ("SAUP.MAU", "DEMOFR.MAU"):
            write(self.p("raw", f), open(os.path.join(self.game, f), "rb").read())
        with open(self.p("images", "index.json"), "w", encoding="utf-8") as f:
            json.dump(self.image_index, f, indent=1)
        with open(self.p("manifest.json"), "w", encoding="utf-8") as f:
            json.dump(self.manifest, f, indent=1)
        print(f"{len(self.image_index)} images, textes, voix et musique extraits dans {self.out}")


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__)
        return 2
    Extractor(argv[1], argv[2]).run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
