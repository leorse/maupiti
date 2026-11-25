#!/usr/bin/env python3
"""Extrait toutes les archives des disquettes Atari ST de Maupiti Island.

Usage :
    python tools/extract_atari.py DISK1.stx DISK2.stx OUT_DIR

Produit dans OUT_DIR :
    boot.bin, loader.bin            secteurs de la piste 0
    arcNN/entryMM.bin               données décompressées
    manifest.json                   tables du chargeur, tailles, SHA-1, erreurs
"""
from __future__ import annotations

import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from maupiti.atari.archive import ArchiveError, decompress, parse_header  # noqa: E402
from maupiti.atari.lankhor_disk import LankhorDiskSet, TrackError  # noqa: E402
from maupiti.atari.loader import find_tables  # noqa: E402


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(__doc__)
        return 2
    disk1, disk2, out = argv[1:]
    os.makedirs(out, exist_ok=True)
    ds = LankhorDiskSet(disk1, disk2)
    boot = ds.boot_sectors()
    open(os.path.join(out, "boot.bin"), "wb").write(boot[:512])
    open(os.path.join(out, "loader.bin"), "wb").write(boot[512:])
    tables = find_tables(boot[512:])

    manifest = {
        "disks": [os.path.basename(disk1), os.path.basename(disk2)],
        "track_errors": {str(k): v for k, v in sorted(ds.errors.items())},
        "loader": {"map_addr": hex(tables.map_addr), "pos_addr": hex(tables.pos_addr),
                   "archive_map": list(tables.archive_map)},
        "archives": [],
    }
    n_ok = n_bad = 0
    for idx, pos in enumerate(tables.positions):
        arc = {"index": idx, "position": pos,
               "archive_numbers": [n for n, m in enumerate(tables.archive_map) if m == idx]}
        try:
            hdr = parse_header(ds.read(pos, 1000))
        except (ArchiveError, TrackError) as e:
            arc["error"] = str(e)
            manifest["archives"].append(arc)
            n_bad += 1
            continue
        arc.update(compressed=hdr.compressed, rle_marker=hdr.marker, header_raw_size=hdr.header_raw_size,
                   entries=[])
        d = os.path.join(out, f"arc{idx:02d}")
        os.makedirs(d, exist_ok=True)
        for k, e in enumerate(hdr.entries):
            ent = {"entry": k, "unpacked_size": e.unpacked_size, "packed_raw_size": e.packed_raw_size}
            if e.unpacked_size:
                try:
                    data = decompress(hdr, ds.read(pos + e.raw_offset, e.packed_raw_size), e.unpacked_size)
                    fn = f"entry{k:03d}.bin"
                    open(os.path.join(d, fn), "wb").write(data)
                    ent.update(file=f"arc{idx:02d}/{fn}", sha1=hashlib.sha1(data).hexdigest())
                    n_ok += 1
                except (ArchiveError, TrackError) as ex:
                    ent["error"] = str(ex)
                    n_bad += 1
            arc["entries"].append(ent)
        manifest["archives"].append(arc)
    with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1, ensure_ascii=False)
    print(f"{out}: {n_ok} entrées extraites, {n_bad} erreurs, pistes en erreur : {sorted(ds.errors)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
