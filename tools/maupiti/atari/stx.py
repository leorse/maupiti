"""Lecture des images de disquette Pasti (.STX) — format Atari ST.

Seules les informations utiles à Maupiti Island sont exposées :
pour chaque piste, la liste des secteurs (avec leurs données) et
l'image brute de la piste telle que la renvoie la commande
« Read Track » du contrôleur WD1772.

Référence du format : documentation Pasti / implémentation de Hatari (stx.c).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

TRACK_FLAG_SECTOR_BLOCK = 0x01   # descripteurs de secteurs présents
TRACK_FLAG_IMAGE = 0x40          # image brute de la piste présente
TRACK_FLAG_IMAGE_SYNC = 0x80     # l'image est précédée d'un mot « first sync »


@dataclass
class Sector:
    track: int
    head: int
    number: int
    size: int
    fdc_status: int
    data: bytes


@dataclass
class Track:
    cylinder: int
    side: int
    flags: int
    sectors: list[Sector] = field(default_factory=list)
    image: bytes | None = None

    def sector(self, number: int) -> Sector | None:
        for s in self.sectors:
            if s.number == number:
                return s
        return None


class StxDisk:
    def __init__(self, path: str):
        self.path = path
        data = open(path, "rb").read()
        if data[:4] != b"RSY\0":
            raise ValueError(f"{path}: pas une image STX")
        self.version, self.tool = struct.unpack_from("<HH", data, 4)
        ntracks = data[10]
        self.tracks: dict[tuple[int, int], Track] = {}
        pos = 16
        for _ in range(ntracks):
            (block_size, fuzzy_size, nsec, flags, track_len,
             track_num, _ttype) = struct.unpack_from("<IIHHHBB", data, pos)
            q = pos + 16
            track = Track(cylinder=track_num & 0x7F, side=track_num >> 7, flags=flags)
            if flags & TRACK_FLAG_SECTOR_BLOCK:
                descs = [struct.unpack_from("<IHHBBBBHBB", data, q + 16 * i) for i in range(nsec)]
                q += 16 * nsec + fuzzy_size
                track_data = q
                if flags & TRACK_FLAG_IMAGE:
                    if flags & TRACK_FLAG_IMAGE_SYNC:
                        q += 2
                    (img_size,) = struct.unpack_from("<H", data, q)
                    track.image = data[q + 2:q + 2 + img_size]
                for (off, _bitpos, _rt, it, ih, isec, isz, _crc, fdc, _r) in descs:
                    size = 128 << (isz & 3)
                    start = track_data + off
                    track.sectors.append(Sector(it, ih, isec, size, fdc, data[start:start + size]))
            else:
                # piste standard : nsec secteurs de 512 octets à la suite
                for i in range(nsec):
                    start = q + 512 * i
                    track.sectors.append(Sector(track.cylinder, track.side, i + 1, 512, 0,
                                                data[start:start + 512]))
            self.tracks[(track.cylinder, track.side)] = track
            pos += block_size

    def track(self, cylinder: int, side: int) -> Track | None:
        return self.tracks.get((cylinder, side))
