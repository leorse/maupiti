#!/usr/bin/env python3
"""Vérifie que le décodeur d'images C++ du moteur donne exactement les mêmes
pixels que le décodeur Python de référence (tools/maupiti/gfx.py).

Usage :
    engine/build/maupiti.exe --pack out/pack --dump-indices out/test_idx
    python tools/tests/test_engine_decoder.py out/pack out/test_idx
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from maupiti.gfx import decode_image  # noqa: E402
from maupiti.pc.container import image_run  # noqa: E402


def read_idx(path):
    d = open(path, "rb").read()
    w, h, x, y = struct.unpack_from("<4H", d, 0)
    return w, h, x, y, d[8:]


def main(pack, idx_dir):
    ok = bad = 0
    for line in open(os.path.join(pack, "rooms.txt"), encoding="utf-8"):
        rid = int(line.split()[0])
        room = open(os.path.join(pack, "rooms", f"{rid:02d}_room.bin"), "rb").read()
        refs = [("room_%02d.idx" % rid, decode_image(room[64:]))]
        sp = os.path.join(pack, "rooms", f"{rid:02d}_sprites.bin")
        if os.path.exists(sp):
            bank = open(sp, "rb").read()
            for k, (off, n) in enumerate(image_run(bank)):
                refs.append(("room_%02d_sprite_%03d.idx" % (rid, k), decode_image(bank[off:off + n])))
        for name, ref in refs:
            path = os.path.join(idx_dir, name)
            if not os.path.exists(path):
                print("absent :", name)
                bad += 1
                continue
            w, h, x, y, px = read_idx(path)
            if (w, h, x, y) == (ref.width, ref.height, *ref.offset) and px == ref.pixels:
                ok += 1
            else:
                print("différent :", name)
                bad += 1
    print(f"{ok} images identiques, {bad} différences")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1], sys.argv[2]))
