"""Format de disquette propriétaire Lankhor (Maupiti Island, Atari ST).

Rétro-ingénierie du chargeur de la piste 0 (secteurs 2-9, chargés en $148E8).

Organisation physique
---------------------
* Piste 0 face 0 de la disquette 1 : 9 secteurs standards (boot + chargeur).
* Toutes les autres pistes : un faux secteur (protection) et, lue par la
  commande FDC « Read Track » ($E0), une zone de données maison :

      ... A1 A1 ... FE 07 TT 07 CH 07 CL xx <5842 octets bruts>

  TT = numéro de piste (bit 7 = disquette 2), CH:CL = somme 16 bits des
  5842 octets bruts. L'octet xx est ignoré par le chargeur.

* Les octets bruts sont « échappés » : 0x07 annonce que l'octet suivant est
  littéral (07 07 -> 07, 07 A1 -> A1 ...). Les positions et longueurs
  utilisées par le chargeur sont exprimées en octets BRUTS.

Adressage linéaire
------------------
position brute P -> piste logique L = P // 5842, décalage = P % 5842
L // 80 : bit 0 = face, bit 1 = disquette ; L % 80 = cylindre.
"""
from __future__ import annotations

from dataclasses import dataclass

from .stx import StxDisk

RAW_PER_TRACK = 0x16D2   # 5842
ESCAPE = 0x07


class TrackError(Exception):
    pass


@dataclass
class LankhorTrack:
    logical: int
    header_track: int
    checksum: int
    raw: bytes          # 5842 octets bruts (non décodés)
    checksum_ok: bool


def parse_track_image(image: bytes) -> tuple[int, int, bytes]:
    """Retourne (TT, somme, 5842 octets bruts) à partir d'une image Read Track."""
    i = image.find(b"\xA1")
    while i >= 0:
        j = i
        while j < len(image) and image[j] == 0xA1:
            j += 1
        hdr = image[j:j + 7]
        if len(hdr) == 7 and hdr[0] == 0xFE and hdr[1] == ESCAPE and hdr[3] == ESCAPE and hdr[5] == ESCAPE:
            tt = hdr[2]
            checksum = (hdr[4] << 8) | hdr[6]
            start = j + 8   # le chargeur saute un octet après CL
            raw = image[start:start + RAW_PER_TRACK]
            if len(raw) < RAW_PER_TRACK:
                raise TrackError("image de piste trop courte")
            return tt, checksum, raw
        i = image.find(b"\xA1", j)
    raise TrackError("en-tête de piste Lankhor introuvable")


class LankhorDiskSet:
    """Les deux disquettes vues comme un flux linéaire d'octets bruts."""

    def __init__(self, disk1: str, disk2: str):
        self.disks = [StxDisk(disk1), StxDisk(disk2)]
        self.tracks: dict[int, LankhorTrack] = {}
        self.errors: dict[int, str] = {}
        for logical in range(4 * 80):
            q, cyl = divmod(logical, 80)
            disk, side = q >> 1, q & 1
            t = self.disks[disk].track(cyl, side)
            if t is None or t.image is None:
                continue
            try:
                tt, checksum, raw = parse_track_image(t.image)
            except TrackError as e:
                if not (disk == 0 and cyl == 0 and side == 0):
                    self.errors[logical] = str(e)
                continue
            ok = (sum(raw) & 0xFFFF) == checksum
            expected_tt = cyl | (0x80 if disk else 0)
            if tt != expected_tt:
                self.errors[logical] = f"numéro de piste {tt:#x} au lieu de {expected_tt:#x}"
            elif not ok:
                self.errors[logical] = "somme de contrôle fausse"
            self.tracks[logical] = LankhorTrack(logical, tt, checksum, raw, ok)

    def boot_sectors(self) -> bytes:
        """Secteurs 1 à 9 de la piste 0 (boot + chargeur)."""
        t = self.disks[0].track(0, 0)
        return b"".join(t.sector(n).data for n in range(1, 10))

    def read_raw(self, pos: int, length: int) -> bytes:
        out = bytearray()
        while length > 0:
            logical, off = divmod(pos, RAW_PER_TRACK)
            n = min(length, RAW_PER_TRACK - off)
            t = self.tracks.get(logical)
            if t is None:
                raise TrackError(f"piste logique {logical} absente ou illisible")
            out += t.raw[off:off + n]
            pos += n
            length -= n
        return bytes(out)

    @staticmethod
    def unescape(raw: bytes) -> bytes:
        out = bytearray()
        i = 0
        while i < len(raw):
            b = raw[i]
            if b == ESCAPE and i + 1 < len(raw):
                i += 1
                b = raw[i]
            out.append(b)
            i += 1
        return bytes(out)

    def read(self, pos: int, raw_length: int) -> bytes:
        return self.unescape(self.read_raw(pos, raw_length))
