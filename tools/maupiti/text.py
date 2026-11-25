"""Textes du jeu (PC : G3.CON bloc 11 ; Atari : archives 7 et 8).

Format
------
* Table : n × u16 big-endian = longueur en octets de chaque message.
* Données : messages compressés en codes de 5 bits (MSB d'abord).
  - code 0          : fin de chaîne
  - codes 1..29     : alphabet principal (trié par fréquence)
  - code 30 / 31    : le code suivant est lu dans le 2e / 3e alphabet
  Les trois alphabets de 29 entrées sont contigus dans maup.exe (DS:0x429) ;
  ils donnent des indices de glyphes de la police du jeu.
* Les messages sont rangés en groupes alignés sur des adresses paires :
  un octet de remplissage précède le groupe qui commence au message 235
  (noms d'objets). C'est le seul cas rencontré.

Routine d'origine : maup.exe $1402 (appelée en far 0084:0BC2).
"""
from __future__ import annotations

import struct

# Position de l'alphabet dans maup.exe : en-tête MZ (0x80 paragraphes) + DS (0xA42 paragraphes) + 0x429
ALPHABET_FILE_OFFSET = 0x80 * 16 + 0xA420 + 0x429
ALPHABET_LEN = 29 * 3

# Indice de glyphe -> caractère Unicode.
GLYPHS: dict[int, str] = {
    0x35: "0", 0x36: "1", 0x37: "2", 0x38: "3", 0x39: "4", 0x3A: "5", 0x3B: "6", 0x3C: "7", 0x3D: "8", 0x3E: "9",
    0x3F: "â", 0x41: "à", 0x42: "ç", 0x43: "é", 0x44: "ê", 0x46: "è", 0x47: "î", 0x48: "ï", 0x49: "ô",
    0x4C: " ", 0x4E: "û", 0x4F: "ù", 0x52: '"', 0x53: "'", 0x54: ".", 0x55: ",", 0x57: ":", 0x58: "!",
    0x59: "?", 0x5B: "(", 0x5C: ")", 0x5E: "-", 0x60: "°", 0x65: "\n",
    # identifiés d'après le dessin de la police (font.py)
    0x45: "ë", 0x4A: "ö", 0x4B: "ò", 0x50: "ü", 0x51: "%", 0x56: ";", 0x5A: "/", 0x5D: "+", 0x5F: "=",
}
for _i in range(26):
    GLYPHS[1 + _i] = chr(ord("a") + _i)
    GLYPHS[0x1B + _i] = chr(ord("A") + _i)

ALIGN_BREAKS = {235: 1}   # message -> octets de remplissage avant lui


def load_alphabet(maup_exe: bytes) -> bytes:
    a = maup_exe[ALPHABET_FILE_OFFSET:ALPHABET_FILE_OFFSET + ALPHABET_LEN]
    if len(a) != ALPHABET_LEN or a[0] != 0x4C or len(set(a)) != ALPHABET_LEN:
        raise ValueError("alphabet introuvable dans maup.exe (version différente ?)")
    return a


def decode_glyphs(data: bytes, alphabet: bytes) -> list[int]:
    out = []
    nbits = len(data) * 8
    bit = 0

    def rd():
        nonlocal bit
        v = 0
        for _ in range(5):
            v = (v << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1)
            bit += 1
        return v

    while bit + 5 <= nbits:
        v = rd()
        base = 0
        if v in (0x1E, 0x1F):
            if bit + 5 > nbits:
                break
            base = 29 * (v - 0x1D)
            v = rd()
        if v == 0:
            break
        idx = base + v - 1
        out.append(alphabet[idx] if idx < len(alphabet) else 0)
    return out


def glyphs_to_str(glyphs: list[int]) -> str:
    return "".join(GLYPHS.get(g, f"{{{g:02x}}}") for g in glyphs)


def decode_bank(table: bytes, data: bytes, alphabet: bytes) -> list[str]:
    n = len(table) // 2
    lengths = struct.unpack(f">{n}H", table[:2 * n])
    texts = []
    pos = 0
    for i, ln in enumerate(lengths):
        pos += ALIGN_BREAKS.get(i, 0)
        texts.append(glyphs_to_str(decode_glyphs(data[pos:pos + ln], alphabet)))
        pos += ln
    return texts
