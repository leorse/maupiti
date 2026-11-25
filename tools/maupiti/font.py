"""Polices bitmap du jeu (en-tête de type GEM, version PC : dans maup.exe).

Deux polices dans le segment de données de maup.exe (DS:0x1522, 8 pixels,
et DS:0x1F11, 7 pixels). En-tête (mots little-endian sur PC) :
  +0x4E hauteur, +0x50 nombre de glyphes, +0x5C offset du bitmap,
  +0x60 largeur du bitmap en octets, +0x62 offset de la table (début, largeur),
  +0x66 offset de la table des avances (un mot par glyphe).
Bitmap : 1 bit par pixel, une ligne après l'autre ; sur PC, les octets de
chaque mot 16 bits sont inversés (données Atari big-endian).
Les glyphes sont numérotés à partir de 1, dans l'ordre utilisé par les textes
(voir text.GLYPHS).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

DS_FILE = 0x80 * 16 + 0xA420
PC_FONTS = {"font8": 0x1522, "font7": 0x1F11}


@dataclass
class Font:
    height: int
    glyphs: list[tuple[int, int]]   # (x, largeur) dans le bitmap, glyphe n° i+1
    advances: list[int]             # avance horizontale, glyphe n° i+1
    bitmap: bytes                   # 1 octet par pixel (0/1), largeur = row_bytes*8
    width: int

    def pixel(self, x: int, y: int) -> int:
        return self.bitmap[y * self.width + x]


def load_pc_font(maup_exe: bytes, name: str) -> Font:
    base = DS_FILE + PC_FONTS[name]

    def w(o):
        return struct.unpack_from("<H", maup_exe, base + o)[0]

    height, count, bm_off, row_bytes, tab_off, adv_off = w(0x4E), w(0x50), w(0x5C), w(0x60), w(0x62), w(0x66)
    if not (4 <= height <= 16 and 50 <= count <= 200 and row_bytes % 2 == 0):
        raise ValueError("en-tête de police inattendu")
    raw = maup_exe[base + bm_off:base + bm_off + row_bytes * height]
    sw = bytearray(raw)
    sw[0::2], sw[1::2] = raw[1::2], raw[0::2]
    width = row_bytes * 8
    bitmap = bytes((sw[y * row_bytes + (x >> 3)] >> (7 - (x & 7))) & 1 for y in range(height) for x in range(width))
    glyphs = [struct.unpack_from("<HH", maup_exe, base + tab_off + 4 * i) for i in range(count)]
    advances = [struct.unpack_from("<H", maup_exe, base + adv_off + 2 * i)[0] for i in range(count)]
    return Font(height, glyphs, advances, bitmap, width)
