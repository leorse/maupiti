"""Script phonétique de la synthèse vocale d'origine.

PC : G4.CON, nœud 8/28/10/1/0 ; Atari : archive 17 (identique, +2 octets).

Format : 460 × u16 big-endian = offset de chaque réplique, relatif à la fin de
la table (0x398 octets). Une réplique est une suite de mots u16 big-endian :
  bits 0-5   code de phonème (0x3C pause courte, 0x3D pause longue, 0x3E fin)
  bits 6-8   prosodie (paramètre A)
  bits 9-11  prosodie (paramètre B)
  bits 12-15 prosodie (paramètre C, hauteur probable)
Lecteur d'origine : maup.exe $66ED (préparation) et $5FA8 (lecture).
"""
from __future__ import annotations

import struct

TABLE_SIZE = 0x398
N_SLOTS = TABLE_SIZE // 2
PAUSE_SHORT, PAUSE_LONG, END = 0x3C, 0x3D, 0x3E


def parse_script(data: bytes) -> list[list[tuple[int, int, int, int]]]:
    offs = list(struct.unpack_from(f">{N_SLOTS}H", data, 0))
    body = data[TABLE_SIZE:]
    out = []
    for i, o in enumerate(offs):
        if i > 0 and o == 0:
            out.append([])
            continue
        nxt = [x for x in offs[i + 1:] if x > o]
        end = nxt[0] if nxt else len(body)
        words = struct.unpack_from(f">{(end - o) // 2}H", body, o)
        utt = []
        for w in words:
            code = w & 0x3F
            utt.append((code, (w >> 6) & 7, (w >> 9) & 7, w >> 12))
            if code == END:
                break
        out.append(utt)
    return out
