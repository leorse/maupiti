"""Tables du chargeur Lankhor (secteurs 2-9 de la piste 0, chargés en $148E8)."""
from __future__ import annotations

import struct
from dataclasses import dataclass

LOADER_BASE = 0x148E8


@dataclass
class LoaderTables:
    archive_map: bytes          # n° d'archive -> index dans la table des positions
    positions: list[int]        # positions brutes des archives sur le flux disquette
    map_addr: int
    pos_addr: int


def find_tables(loader: bytes) -> LoaderTables:
    """Repère les tables à partir du code de la routine $15002.

        lea     MAP.l,a2            45F9 xxxxxxxx
        cmpi.w  #$1d,d0             0C40 001D
        ...
        move.b  (a2,d0.w),d0        1032 0000
        lea     POS(pc),a2          45FA dddd
        lsl.w   #2,d0               E548
    """
    i = loader.find(bytes.fromhex("0c40001d"))
    while i >= 0:
        if loader[i - 6:i - 4] == b"\x45\xf9":
            (map_addr,) = struct.unpack_from(">I", loader, i - 4)
            j = loader.find(bytes.fromhex("10320000"), i)
            if j > 0 and loader[j + 4:j + 6] == b"\x45\xfa" and loader[j + 8:j + 10] == b"\xe5\x48":
                (disp,) = struct.unpack_from(">h", loader, j + 6)
                pos_addr = LOADER_BASE + j + 4 + 2 + disp
                break
        i = loader.find(bytes.fromhex("0c40001d"), i + 1)
    else:
        raise ValueError("tables du chargeur introuvables")
    mo = map_addr - LOADER_BASE
    po = pos_addr - LOADER_BASE
    positions = []
    p = po
    while p + 4 <= len(loader):
        (v,) = struct.unpack_from(">I", loader, p)
        if v == 0xFFFFFFFF:
            break
        positions.append(v)
        p += 4
    # la table de correspondance s'arrête là où commence la suivante (octets < nb de positions)
    amap = bytearray()
    q = mo
    while q < po and loader[q] < len(positions):
        amap.append(loader[q])
        q += 1
    return LoaderTables(bytes(amap), positions, map_addr, pos_addr)
