"""Archives compressées du chargeur Lankhor (Maupiti Island, Atari ST).

Port fidèle des routines 68000 du chargeur ($15126 construction des tables,
$1531c décompression, $152ba sélection d'une entrée).

Structure d'une archive (lue à une position brute P du flux disquette)
------------------------------------------------------------------------
  +0   octet   drapeau (≠0 : compressé Huffman)
  +1   octet   marqueur RLE
  +2   n × (u16 taille décompressée, u16 taille compressée BRUTE), terminé par FFFFFFFF
       octet   ignoré
       octet   paramètres : bits 0-2 = longueur minimale de code,
                            bits 3-7 = nombre de bits par longueur
       256 longueurs de code (bitstream MSB d'abord, lu par mots de 32 bits)
       u16     taille brute de l'en-tête (début des données)
  données : entrée i à P + taille_en_tête + Σ tailles compressées précédentes

Décompression : Huffman canonique (codes les plus longs numérotés à partir
de 0), lu MSB d'abord par mots de 16 bits ; le marqueur RLE introduit
  marqueur n        (n < 2)  -> n+1 fois le marqueur
  marqueur n v      (n >= 2) -> n+1 fois v
"""
from __future__ import annotations

import struct
from dataclasses import dataclass


class ArchiveError(Exception):
    pass


@dataclass
class Entry:
    unpacked_size: int
    packed_raw_size: int
    raw_offset: int        # depuis le début de l'archive, en octets bruts


@dataclass
class ArchiveHeader:
    compressed: bool
    marker: int
    entries: list[Entry]
    min_len: int
    code_lengths: list[int]
    header_raw_size: int


def parse_header(h: bytes) -> ArchiveHeader:
    if len(h) < 8:
        raise ArchiveError("en-tête trop court")
    flag, marker = h[0], h[1]
    p = 2
    pairs = []
    while True:
        if p + 4 > len(h):
            raise ArchiveError("table des entrées non terminée")
        (v,) = struct.unpack_from(">I", h, p)
        p += 4
        if v == 0xFFFFFFFF:
            break
        pairs.append((v >> 16, v & 0xFFFF))
        if len(pairs) > 200:
            raise ArchiveError("table des entrées trop longue")
    if not flag:
        return ArchiveHeader(False, marker, [Entry(u, c, 0) for u, c in pairs], 0, [], p)
    p += 1                      # clr.b (a4)+
    params = h[p]
    p += 1
    min_len = params & 7
    field_bits = params >> 3
    if field_bits == 0:
        raise ArchiveError("paramètres Huffman invalides")
    # bitstream de 256 * field_bits bits, lu par mots longs
    nbits = 256 * field_bits
    nlongs = (nbits + 31) // 32
    bs = h[p:p + 4 * nlongs]
    if len(bs) < 4 * nlongs:
        raise ArchiveError("en-tête tronqué")
    bitval = int.from_bytes(bs, "big")
    total = 32 * nlongs
    lengths = []
    for i in range(256):
        shift = total - (i + 1) * field_bits
        v = (bitval >> shift) & ((1 << field_bits) - 1)
        lengths.append(v + min_len - 1 if v else 0)
    p += 4 * nlongs
    if nbits % 32 == 0:
        pass            # le chargeur relit le mot long suivant puis recule de 4 : même position
    else:
        p -= 4          # cas jamais rencontré : on reproduit le « subq #4,a4 »
    (header_raw_size,) = struct.unpack_from(">H", h, p)
    entries = []
    off = header_raw_size
    for u, c in pairs:
        entries.append(Entry(u, c, off))
        off += c
    return ArchiveHeader(True, marker, entries, min_len, lengths, header_raw_size)


def build_codes(lengths: list[int]) -> dict[tuple[int, int], int]:
    """Reproduit l'attribution des codes du chargeur ($15194-$15212)."""
    remaining = {s: l for s, l in enumerate(lengths) if l}
    order = []
    while remaining:
        best = None
        for s in sorted(remaining):
            if best is None or remaining[s] <= remaining[best]:
                best = s
        order.append((remaining.pop(best), best))
    codes: dict[tuple[int, int], int] = {}
    if not order:
        return codes
    cur_len = order[-1][0]
    code = 0
    for idx in range(len(order) - 1, -1, -1):
        length, sym = order[idx]
        while length != cur_len:
            cur_len -= 1
            code >>= 1
        codes[(length, code)] = sym
        code += 1
    return codes


class _BitReader:
    def __init__(self, data: bytes):
        if len(data) % 2:
            data += b"\0"
        self.data = data
        self.pos = 0

    def bit(self) -> int:
        byte = self.pos >> 3
        if byte >= len(self.data):
            raise ArchiveError("fin des données compressées")
        b = (self.data[byte] >> (7 - (self.pos & 7))) & 1
        self.pos += 1
        return b


def decompress(hdr: ArchiveHeader, packed: bytes, size: int) -> bytes:
    if not hdr.compressed:
        return packed[:size]
    codes = build_codes(hdr.code_lengths)
    max_len = max(l for l, _ in codes) if codes else 0
    br = _BitReader(packed)

    def symbol() -> int:
        v = 0
        length = 0
        for _ in range(hdr.min_len):
            v = (v << 1) | br.bit()
            length += 1
        while True:
            s = codes.get((length, v))
            if s is not None:
                return s
            if length >= max_len:
                raise ArchiveError("code Huffman invalide")
            v = (v << 1) | br.bit()
            length += 1

    out = bytearray()
    while len(out) < size:
        s = symbol()
        if s != hdr.marker:
            out.append(s)
            continue
        n = symbol()
        if n < 2:
            out += bytes([hdr.marker]) * (n + 1)
        else:
            v = symbol()
            out += bytes([v]) * (n + 1)
    return bytes(out[:size])
