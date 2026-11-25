#!/usr/bin/env python3
"""Convertit les archives Atari extraites (extract_atari.py) en images PNG,
et établit la correspondance avec les lieux de la version PC.

Usage :
    python tools/convert_atari.py OUT_ATARI [PC_GAME_DIR OUT_PC]

OUT_ATARI   : dossier produit par extract_atari.py (manifest.json, arcNN/)
PC_GAME_DIR : fichiers du jeu PC (G1.CON…), facultatif, pour la correspondance
OUT_PC      : dossier produit par extract_pc.py, pour la page de comparaison

Produit dans OUT_ATARI :
    images/arcNN/...png, images/index.json
    sounds/arc61_NNN.wav  sons numérisés (PCM 4 bits ; fréquence provisoire 8 kHz)
    rooms.json         lieux Atari, avec le lieu PC correspondant
    compare.html       comparaison visuelle PC / Atari (si OUT_PC fourni)
"""
from __future__ import annotations

import html
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from maupiti.gfx import decode_image, image_size, parse_palette, st_palette_to_rgb  # noqa: E402
from maupiti.pc.container import Node, classify, image_run  # noqa: E402
from maupiti.png import write_indexed  # noqa: E402

GREY = [(i * 17, i * 17, i * 17) for i in range(16)]


def geometry(bank: bytes) -> list[tuple[int, int, int, int]]:
    out = []
    for off, n in image_run(bank):
        im = decode_image(bank[off:off + n])
        out.append((im.offset[0], im.offset[1], im.width, im.height))
    return out


def geometry_score(g1, g2) -> float:
    if not g1 and not g2:
        return 0.0
    near = sum(1 for (x, y, w, h) in g1
               if any(abs(x - a) <= 4 and abs(y - b) <= 4 and abs(w - c) <= 6 and abs(h - d) <= 6
                      for a, b, c, d in g2))
    return near / max(len(g1), len(g2))


def room_signature(d: bytes):
    n = image_size(d[64:])
    im = decode_image(d[64:64 + n])
    return parse_palette(d, 0) + parse_palette(d, 32) + im.palette, (im.width, im.height)


SOUND_ARCHIVES = (61,)       # 146 sons numérisés (l'archive 55 en est un sous-ensemble)
SOUND_RATE = 8000            # provisoire : à confirmer avec la routine de lecture (étape 7)


def write_wav_4bit(path: str, data: bytes, rate: int = SOUND_RATE) -> None:
    """Quartets non signés (poids fort d'abord) -> WAV 8 bits non signé."""
    pcm = bytearray()
    for b in data:
        pcm.append((b >> 4) * 17)
        pcm.append((b & 15) * 17)
    hdr = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 1, 1, rate, rate, 1, 8) + b"data" + struct.pack("<I", len(pcm))
    with open(path, "wb") as f:
        f.write(hdr + pcm)


class AtariConverter:
    def __init__(self, root: str, pc_game: str | None, pc_root: str | None):
        self.root, self.pc_game, self.pc_root = root, pc_game, pc_root
        self.manifest = json.load(open(os.path.join(root, "manifest.json"), encoding="utf-8"))
        self.index: dict[str, dict] = {}
        self.rooms: list[dict] = []

    def entry(self, arc: int, k: int) -> bytes | None:
        p = os.path.join(self.root, f"arc{arc:02d}", f"entry{k:03d}.bin")
        return open(p, "rb").read() if os.path.exists(p) else None

    def save(self, data: bytes, rel: str, name: str, palette, force=False) -> None:
        img = decode_image(data)
        own = img.palette if img.palette and any(img.palette[1:]) and not force else None
        words = own or palette
        pal = st_palette_to_rgb(words) if words else GREY
        tr = img.transparency if img.transparency < 16 else None
        d = os.path.join(self.root, "images", rel)
        os.makedirs(d, exist_ok=True)
        write_indexed(os.path.join(d, name + ".png"), img.width, img.height, img.pixels, pal, tr)
        self.index[f"{rel}/{name}.png"] = {"x": img.offset[0], "y": img.offset[1], "w": img.width,
                                           "h": img.height, "transparent": tr}

    def run(self) -> None:
        ctx = None
        for arc in self.manifest["archives"]:
            idx = arc["index"]
            entries = arc.get("entries", [])
            room_here = False
            for e in entries:
                if "file" not in e:
                    continue
                data = open(os.path.join(self.root, e["file"]), "rb").read()
                node = Node(e["file"], 0, data)
                classify(node)
                rel = f"arc{idx:02d}"
                if node.kind == "room":
                    n = node.info["image_size"]
                    img_data = data[64:64 + n]
                    im = decode_image(img_data)
                    ctx = parse_palette(data, 0)
                    for label, words in (("A", parse_palette(data, 0)), ("B", parse_palette(data, 32)),
                                         ("C", im.palette)):
                        self.save(img_data, rel, f"room_{label}", words, force=True)
                    self.rooms.append({"archive": idx, "size": [im.width, im.height]})
                    room_here = True
                elif node.kind == "images":
                    sub = f"{rel}/e{e['entry']:03d}"
                    for k, (off, n) in enumerate(image_run(data)):
                        # sprites d'un lieu : affichés dans la palette du lieu
                        self.save(data[off:off + n], sub, f"{k:03d}", ctx, force=(e["entry"] == 1 and room_here))
        with open(os.path.join(self.root, "images", "index.json"), "w", encoding="utf-8") as f:
            json.dump(self.index, f, indent=1)
        self.sounds()
        if self.pc_game:
            self.match_pc()
        with open(os.path.join(self.root, "rooms.json"), "w", encoding="utf-8") as f:
            json.dump(self.rooms, f, indent=1)
        print(f"{len(self.index)} images, {len(self.rooms)} lieux")

    def sounds(self) -> None:
        d = os.path.join(self.root, "sounds")
        os.makedirs(d, exist_ok=True)
        n = 0
        for arc in SOUND_ARCHIVES:
            for k in range(256):
                data = self.entry(arc, k)
                if data:
                    write_wav_4bit(os.path.join(d, f"arc{arc:02d}_{k:03d}.wav"), data)
                    n += 1
        print(f"{n} sons")

    # --- correspondance avec le PC -------------------------------------------------
    def match_pc(self) -> None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from maupiti.pc.container import parse, walk
        pc_game = self.pc_game
        pc_rooms = {}
        for g in range(1, 7):
            nodes = {n.path: n for n in walk(parse(f"G{g}", open(os.path.join(pc_game, f"G{g}.CON"), "rb").read()))}
            for p, n in nodes.items():
                if n.kind == "room":
                    sib = nodes.get(p.rsplit("/", 1)[0] + "/1")
                    geo = geometry(sib.data) if sib is not None and sib.kind == "images" else []
                    pc_rooms[p] = (geo, room_signature(n.data))
        # 1) géométrie des sprites : identique (score 1) => correspondance sûre, doublons permis ;
        #    sinon appariement unique, meilleurs scores d'abord.
        used = set()
        pairs = []
        for r in self.rooms:
            bank = self.entry(r["archive"], 1)
            geo = geometry(bank) if bank else []
            for p, (g, _) in pc_rooms.items():
                s = geometry_score(geo, g)
                if s >= 0.5:
                    pairs.append((s, r["archive"], p))
        by_arc = {r["archive"]: r for r in self.rooms}
        for s, arc, p in sorted(pairs, reverse=True):
            r = by_arc[arc]
            if "pc" in r or (p in used and s < 1.0):
                continue
            r.update(pc=p, match="sprites", score=round(s, 2))
            used.add(p)
        pending = [r for r in self.rooms if "pc" not in r]
        # 2) décors sans sprites : palettes communes + taille identique
        for r in pending:
            sig, size = room_signature(self.entry(r["archive"], 0))
            cands = []
            for p, (_, (psig, psize)) in pc_rooms.items():
                if p in used:
                    continue
                same = sum(1 for a, b in zip(sig, psig) if a == b)
                cands.append((same + (20 if psize == size else 0), p))
            cands.sort(reverse=True)
            if cands and cands[0][0] >= 24:
                r.update(pc=cands[0][1], match="palette+taille", score=cands[0][0])
                used.add(cands[0][1])
            else:
                r.update(pc=None, match="aucune")
        if self.pc_root:
            self.write_compare()

    def write_compare(self) -> None:
        pc_img = os.path.join(self.pc_root, "images")
        rows = []
        for r in self.rooms:
            a = f"images/arc{r['archive']:02d}/room_A.png"
            if r.get("pc"):
                g = r["pc"].split("/")[0]
                p = os.path.relpath(os.path.join(pc_img, g, r["pc"].replace("/", "_") + "_A.png"), self.root)
                pc_cell = f'<img src="{html.escape(p.replace(os.sep, "/"))}"><br>{html.escape(r["pc"])}'
            else:
                pc_cell = "—"
            rows.append(f"<tr><td>arc{r['archive']:02d}<br>{r['match']}</td>"
                        f'<td><img src="{a}"></td><td>{pc_cell}</td></tr>')
        page = ("<!doctype html><meta charset=utf-8><title>PC / Atari</title>"
                "<style>body{background:#222;color:#ddd;font:14px sans-serif}"
                "img{image-rendering:pixelated;width:512px}td{padding:6px;vertical-align:top}</style>"
                "<h1>Décors : Atari ST (gauche) / PC (droite)</h1><table>" + "".join(rows) + "</table>")
        with open(os.path.join(self.root, "compare.html"), "w", encoding="utf-8") as f:
            f.write(page)


def main(argv):
    if len(argv) not in (2, 4):
        print(__doc__)
        return 2
    AtariConverter(argv[1], *(argv[2:4] if len(argv) == 4 else (None, None))).run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
