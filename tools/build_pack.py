#!/usr/bin/env python3
"""Construit le paquet de données lu par le moteur (engine/).

Usage :
    python tools/build_pack.py OUT_ATARI PC_GAME_DIR PACK_DIR

Contenu du paquet (formats simples, little-endian) :
    rooms.txt               une ligne par lieu : "<id> <archive Atari> <lieu PC ou ->"
    rooms/NN_room.bin       entrée Atari brute : palettes A et B + image (palette C intégrée) (format Lankhor)
    rooms/NN_sprites.bin    banque de sprites Atari brute (si présente)
    ui/title.bin            écran titre (image Lankhor 320x200, Atari arc10/2)
    ui/frame.bin            morceaux du cadre de l'interface (banque d'images, Atari arc10/0)
    ui/menubar.bin          barre de menu (Atari arc10/1)
    font8.fnt, font7.fnt    "FNT1", u16 hauteur, u16 nb, u16 largeur atlas,
                            nb × (u16 x, u16 largeur, u16 avance), puis largeur*hauteur octets 0/1
    texts.bin               "TXT1", u16 nb, puis pour chaque texte : u16 longueur, indices de glyphes
    rooms/NN_patch.bin      "PTC1", u16 nb, puis nb × (u16 x, y, l, h + l*h indices de palette Atari) : zones
                            réservées en noir sur Atari (éléments animés, ex. le ventilateur de la cabine) que
                            l'on remplit avec le dessin fixe du décor PC, converti vers la palette Atari
    lieux.txt               "<ressource> <lieu du paquet>" : n° de ressource décor utilisé par la logique
                            (lieu logique - 1, service 24 de maup.exe) -> index dans rooms.txt
    start.sav               état de départ d'une nouvelle partie (SAUP.MAU, 2467 octets, little-endian :
                            mot 0 = lieu, mot 1 = heure)
    atari/arcNN/entryKKK.bin  archives Atari telles qu'extraites : son (pilote 68000, tables, banques,
                            échantillons ; src/atari_sound.cpp) et médias du navigateur (portraits, objets,
                            documents, interface ; src/media.cpp)
    atari/map.bin           table MAP du chargeur Atari (n° de requête + $1E -> n° d'archive)
    pc/                     logique de la version PC, exécutée par le moteur en attendant sa réécriture
                            (src/logic) : maupiti1.img (MAUPITI1.EXE décompressé), maup.img (image de
                            maup.exe : tables des ressources), G1.CON … G6.CON, SAUP.MAU, DEMOFR.MAU
"""
from __future__ import annotations

import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from maupiti import text  # noqa: E402
from maupiti.font import PC_FONTS, load_pc_font  # noqa: E402
from maupiti.pc.container import parse, walk  # noqa: E402

N_TEXTS = 1589


def _black_zones(pixels, w, h, values):
    """Zones rectangulaires (composantes connexes remplies à 95 %) d'une couleur noire, >= 100 pixels."""
    seen, out = bytearray(w * h), []
    for i in range(w * h):
        if pixels[i] in values and not seen[i]:
            stack, xs, ys = [i], [], []
            seen[i] = 1
            while stack:
                j = stack.pop()
                x, y = j % w, j // w
                xs.append(x)
                ys.append(y)
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if 0 <= nx < w and 0 <= ny < h:
                        k = ny * w + nx
                        if not seen[k] and pixels[k] in values:
                            seen[k] = 1
                            stack.append(k)
            bw, bh = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
            if len(xs) >= 100 and len(xs) >= 0.95 * bw * bh and bw >= 6 and bh >= 6:
                out.append((min(xs), min(ys), bw, bh))
    return out


def write_patches(atari, pc_images, pack, rooms):
    """Remplit les zones noires réservées d'un décor Atari avec le décor PC quand celui-ci y montre un objet."""
    from maupiti import gfx
    try:
        from PIL import Image
    except ImportError:
        return 0
    n = 0
    for i, r in enumerate(rooms):
        pc = r.get("pc")
        png = os.path.join(pc_images, pc.split("/")[0], pc.replace("/", "_") + "_A.png") if pc else None
        if not png or not os.path.exists(png):
            continue
        d = open(os.path.join(atari, f"arc{r['archive']:02d}", "entry000.bin"), "rb").read()
        im = gfx.decode_image(d[64:])
        pal = struct.unpack(">16H", d[:32])
        rgb = [(((w >> 8) & 7) * 255 // 7, ((w >> 4) & 7) * 255 // 7, (w & 7) * 255 // 7) for w in pal]
        pcimg = Image.open(png).convert("RGB")
        if pcimg.size != (im.width, im.height):
            continue
        blacks = {k for k, w in enumerate(pal) if w & 0x777 == 0}
        patches = []
        for x, y, w, h in _black_zones(im.pixels, im.width, im.height, blacks):
            px = [pcimg.getpixel((x + i2, y + j)) for j in range(h) for i2 in range(w)]
            lit = sum(1 for c in px if sum(c) > 120)
            if lit < 0.3 * len(px):                      # le PC est sombre aussi : vraie ombre, on garde
                continue
            # fond : le mur Atari juste au-dessus (ou au-dessous) de la zone, même colonne ;
            # premier plan : les pixels PC qui diffèrent nettement du mur PC au même endroit (l'objet).
            # mur de référence : la bande de même hauteur sous la zone (sinon au-dessus), si elle n'est pas noire
            oy = None
            for cand in (y + h, y - h):
                if 0 <= cand and cand + h <= im.height:
                    band = [im.pixels[(cand + j) * im.width + x + i2] for j in range(h) for i2 in range(w)]
                    if sum(1 for v in band if v in blacks) < 0.1 * len(band):
                        oy = cand
                        break
            if oy is None:
                continue
            idx = bytearray()
            for j in range(h):
                for i2 in range(w):
                    fg = pcimg.getpixel((x + i2, y + j))
                    wall = pcimg.getpixel((x + i2, oy + j))
                    if sum((a - b) ** 2 for a, b in zip(fg, wall)) > 80 ** 2:
                        idx.append(min(range(16), key=lambda k: sum((a - b) ** 2 for a, b in zip(fg, rgb[k]))))
                    else:
                        idx.append(im.pixels[(oy + j) * im.width + x + i2])
            patches.append(struct.pack("<4H", x, y, w, h) + bytes(idx))
        path = os.path.join(pack, "rooms", f"{i:02d}_patch.bin")
        if patches:
            with open(path, "wb") as f:
                f.write(b"PTC1" + struct.pack("<H", len(patches)) + b"".join(patches))
            n += len(patches)
        elif os.path.exists(path):
            os.remove(path)
    return n


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    atari, pc_game, pack = argv[1:]
    os.makedirs(os.path.join(pack, "rooms"), exist_ok=True)
    os.makedirs(os.path.join(pack, "ui"), exist_ok=True)
    exe = open(os.path.join(pc_game, "maup.exe"), "rb").read()

    # lieux (correspondance établie par convert_atari.py)
    rooms = json.load(open(os.path.join(atari, "rooms.json"), encoding="utf-8"))
    lines = []
    for i, r in enumerate(rooms):
        arc = r["archive"]
        for k, suffix in ((0, "room"), (1, "sprites")):
            src = os.path.join(atari, f"arc{arc:02d}", f"entry{k:03d}.bin")
            if os.path.exists(src):
                with open(os.path.join(pack, "rooms", f"{i:02d}_{suffix}.bin"), "wb") as f:
                    f.write(open(src, "rb").read())
        lines.append(f"{i} {arc} {r.get('pc') or '-'}")
    with open(os.path.join(pack, "rooms.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    # ressource décor (n° utilisé par la logique) -> lieu du paquet. maup.exe range les ressources par
    # groupe (fichier G1..G6.CON) : identifiants listés en DS:0xA44 (séparés par 0xFFFF), le rang dans le
    # groupe désigne le n-ième lieu de la liste chaînée du conteneur.
    hdr = struct.unpack("<14H", exe[:28])
    img = exe[hdr[4] * 16:]
    ids = struct.unpack("<68H", img[0xA420 + 0xA44:0xA420 + 0xA44 + 136])
    where, group, rank = {}, 1, 0
    for v in ids:
        if v == 0xFFFF:
            group, rank = group + 1, 0
        else:
            where[(group, rank)] = v
            rank += 1
    node_rank = {}
    for g in range(1, 7):
        root = parse(f"G{g}", open(os.path.join(pc_game, f"G{g}.CON"), "rb").read())
        nodes = sorted((n.offset, n.path) for n in walk(root) if n.kind == "room")
        for r, (_, path) in enumerate(nodes):
            node_rank[path] = (g, r)
    lieux = []
    for i, r in enumerate(rooms):
        key = node_rank.get(r.get("pc") or "")
        if key and key in where:
            lieux.append(f"{where[key]} {i}")
    with open(os.path.join(pack, "lieux.txt"), "w") as f:
        f.write("\n".join(lieux) + "\n")
    with open(os.path.join(pack, "start.sav"), "wb") as f:
        f.write(open(os.path.join(pc_game, "SAUP.MAU"), "rb").read())

    # zones réservées des décors Atari (animations encore non décodées) -> dessin fixe du décor PC
    patched = write_patches(atari, os.path.join(os.path.dirname(pack), "pc", "images"), pack, rooms)

    # son Atari : archives lues par le pilote d'origine (docs/architecture.md §12)
    from maupiti.atari.loader import find_tables
    snd = os.path.join(pack, "atari")
    os.makedirs(snd, exist_ok=True)
    tables = find_tables(open(os.path.join(atari, "loader.bin"), "rb").read())
    with open(os.path.join(snd, "map.bin"), "wb") as f:
        f.write(bytes(tables.archive_map))
    import shutil
    for arc in (0, 2, 3, 4, 5, 10, 11, 12, 13, 14, 15, 18, 19, 20, 21, 22, 54, 55, 60, 61, 62, 63, 64,
                *range(24, 53)):   # son, médias, lieux (ambiance sonore : entrée 2 ; animations : entrée 3)
        src = os.path.join(atari, f"arc{arc:02d}")
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(snd, f"arc{arc:02d}"), dirs_exist_ok=True)

    # logique PC (moteur hybride, docs/architecture.md §14)
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "re"))
    from unlzexe import unlzexe
    pcd = os.path.join(pack, "pc")
    os.makedirs(pcd, exist_ok=True)
    with open(os.path.join(pcd, "maupiti1.img"), "wb") as f:
        f.write(unlzexe(open(os.path.join(pc_game, "MAUPITI1.EXE"), "rb").read()))
    hdr_par = struct.unpack("<H", exe[8:10])[0]
    with open(os.path.join(pcd, "maup.img"), "wb") as f:
        f.write(exe[hdr_par * 16:])
    for name in ("G1.CON", "G2.CON", "G3.CON", "G4.CON", "G5.CON", "G6.CON", "SAUP.MAU", "DEMOFR.MAU"):
        shutil.copyfile(os.path.join(pc_game, name), os.path.join(pcd, name))

    # interface
    for k, name in ((0, "frame"), (1, "menubar"), (2, "title")):
        with open(os.path.join(pack, "ui", name + ".bin"), "wb") as f:
            f.write(open(os.path.join(atari, "arc10", f"entry{k:03d}.bin"), "rb").read())

    # polices
    for name in PC_FONTS:
        fnt = load_pc_font(exe, name)
        out = bytearray(b"FNT1")
        out += struct.pack("<HHH", fnt.height, len(fnt.glyphs), fnt.width)
        for (x, w), adv in zip(fnt.glyphs, fnt.advances):
            out += struct.pack("<HHH", x, w, adv)
        out += fnt.bitmap
        with open(os.path.join(pack, name + ".fnt"), "wb") as f:
            f.write(out)

    # textes, sous forme d'indices de glyphes
    g3 = parse("G3", open(os.path.join(pc_game, "G3.CON"), "rb").read())
    bank = g3.children[11].data
    table = bank[172:172 + 2 * N_TEXTS]
    sub = bank[172 + 2 * N_TEXTS:]
    (o0, o1) = struct.unpack_from("<II", sub, 0)
    data = sub[o0:o1]
    alphabet = text.load_alphabet(exe)
    lengths = struct.unpack(f">{N_TEXTS}H", table)
    out = bytearray(b"TXT1") + struct.pack("<H", N_TEXTS)
    pos = 0
    for i, ln in enumerate(lengths):
        pos += text.ALIGN_BREAKS.get(i, 0)
        glyphs = text.decode_glyphs(data[pos:pos + ln], alphabet)
        pos += ln
        out += struct.pack("<H", len(glyphs)) + bytes(glyphs)
    with open(os.path.join(pack, "texts.bin"), "wb") as f:
        f.write(out)
    print(f"paquet écrit dans {pack} : {len(rooms)} lieux, {N_TEXTS} textes, {len(PC_FONTS)} polices")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
