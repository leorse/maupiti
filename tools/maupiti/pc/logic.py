"""Bloc « logique » du jeu (G3, noeud 11 ; Atari : archive 9) : 516 blocs adressés par une table.

Format (établi par le désassemblage de MAUPITI1 : fonction 0x63A3 et helper 0xF3F, et de
maup.exe : service 15) :
    8 octets d'en-tête, puis table de N mots big-endian (N = premier mot / 2) :
      - le premier mot est la taille de la table en octets,
      - les suivants sont les tailles de chaque bloc ;
    le moteur les additionne une fois au premier appel (« fix-up ») : bloc n commence
    à la somme des n+1 premiers mots, et se termine au début du suivant.
Les blocs 0xC8-0xE7 et 0x1EA-0x1F3 alimentent le moteur de règles des interrogatoires.
"""
from __future__ import annotations

import struct

HEADER = 8


def parse_blocks(data: bytes) -> list[bytes]:
    body = data[HEADER:]
    n = struct.unpack(">H", body[:2])[0] // 2
    sizes = struct.unpack(f">{n}H", body[:2 * n])
    starts, c = [], 0
    for s in sizes:
        c += s
        starts.append(c)
    starts.append(len(body))
    return [body[starts[i]:starts[i + 1]] for i in range(n)]


def words(block: bytes) -> list[int]:
    return list(struct.unpack(f">{len(block) // 2}H", block[:len(block) // 2 * 2]))
