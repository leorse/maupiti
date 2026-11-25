"""Décodeur d'images Lankhor (Mortevielle / Maupiti Island).

Port Python de GfxSurface::decode de ScummVM (engines/mortevielle/graphics.cpp),
lui-même basé sur le code source DOS d'origine de Lankhor.
Code d'origine : ScummVM, GPL v3 ou ultérieure — ce module est donc sous GPL v3+.

Format (tout en big-endian) :
  +0  octet  0 = pas de transparence (et 30 octets de palette à sauter), sinon
             l'octet +2 donne l'indice de couleur transparent
  +1  octet  nombre de morceaux
  +2  2 octets
  [+4 30 octets si octet0 == 0]
  morceaux : u16 lookupBytes, u16 srcSize, u16 x, u16 y,
             u16 decomCode, u16 largeur-1, u16 hauteur-1, données
  Les coordonnées x sont en pixels d'un écran de 320 de large.

Particularité Maupiti : les quartets décodés doivent être « bit-inversés »
(0b0001 -> 0b1000…) pour donner l'indice de palette ; decode_image le fait.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

WIDTH = 320            # DEFAULT_WIDTH = SCREEN_WIDTH / 2 dans ScummVM
BUF = 65536
MARGIN = 8192          # garde-fou contre les débordements de pointeur
BUFFER_SIZE = 40000
BITREV = [int(f"{v:04b}"[::-1], 2) for v in range(16)]
BITREV_TABLE = bytes(BITREV + list(range(16, 256)))


class GfxError(Exception):
    pass


@dataclass
class Image:
    width: int
    height: int
    pixels: bytes            # indices de palette, width*height
    transparency: int        # 16 = aucune
    offset: tuple[int, int]  # position d'origine à l'écran
    palette: list[int] | None = None   # mots ST 0x0RGB si présents


def _be16(b, o):
    return (b[o] << 8) | b[o + 1]


class _Decoder:
    def __init__(self, src: bytes):
        self.src = src

    # --- lecteurs -------------------------------------------------------------
    def next_nibble(self) -> int:
        v = self.mem[self.ps]
        if self.nibble_flag:
            self.ps += 1
            self.lookup_index += 1
            self.nibble_flag = False
            return v & 0xF
        self.nibble_flag = True
        return v >> 4

    def next_byte(self) -> int:
        while not self.lookup_value:
            while True:
                v = self.next_nibble() & 0xFF
                self.lookup_value += v
                if v != 0xF:
                    break
            self.pl += 1
        self.lookup_value -= 1
        return self.lookup[self.pl]

    def desanalyse(self) -> int:
        total = 0
        v = self.next_nibble()
        if v == 0xF:
            while True:
                v2 = self.next_nibble()
                total += v2
                if v2 != 0xF:
                    break
            total *= 15
            v = self.next_nibble()
        return total + v

    def put(self, v):
        self.out[self.pd + MARGIN] = v

    # --- décodage ------------------------------------------------------------
    def decode(self) -> Image:
        s = self.src
        self.w = self.h = 0
        transparency = 16 if s[0] == 0 else s[2]
        offset_flag = s[0] == 0
        entry_count = s[1]
        pos = 4
        palette = None
        if offset_flag:
            palette = [_be16(s, 2)] + [_be16(s, 4 + 2 * i) for i in range(15)]
            pos += 30
        if entry_count == 0:
            raise GfxError("image vide")
        # passe 1 : décalage
        p = pos
        ox = oy = 999
        for _ in range(entry_count):
            ox = min(ox, _be16(s, p + 4))
            oy = min(oy, _be16(s, p + 6))
            size = _be16(s, p) + _be16(s, p + 2)
            if size & 1:
                size += 1
            p += size + 14
        self.out = bytearray([transparency]) * (BUF + 2 * MARGIN)
        # mémoire de travail : la source, puis les deux tampons décompressés
        lookup_table = bytearray(BUFFER_SIZE)
        src_buffer = bytearray(BUFFER_SIZE)
        ps_start = pos
        cur = pos
        for _entry in range(entry_count):
            self.mem = s
            self.ps = cur
            lookup_bytes = _be16(s, self.ps)
            src_size = _be16(s, self.ps + 2)
            self.xp = _be16(s, self.ps + 4) - ox
            self.yp = _be16(s, self.ps + 6) - oy
            if not (0 <= self.xp < 640 and 0 <= self.yp < 400):
                raise GfxError("position de morceau invalide")
            self.ps += 8
            decom_code = _be16(s, self.ps)
            self.x_size = _be16(s, self.ps + 2) + 1
            self.y_size = _be16(s, self.ps + 4) + 1
            # boîte englobante de tous les morceaux (les visages Atari ne forment pas une grille régulière)
            self.w = max(self.w, self.xp + self.x_size)
            self.h = max(self.h, self.yp + self.y_size)
            self.ps += 6
            self.pd = 0
            self.lookup_index = 0
            self.nibble_flag = False
            decom_index = 0
            self.lookup = lookup_table
            self.pl = -1
            if decom_code >> 8:
                table_offset = 0
                if decom_code & 1:
                    while True:
                        outer = self.desanalyse()
                        inner = self.desanalyse()
                        saved = (self.ps, self.nibble_flag, self.lookup_index)
                        while True:
                            self.ps, self.nibble_flag, self.lookup_index = saved
                            for _ in range(inner):
                                lookup_table[table_offset] = self.next_nibble()
                                table_offset += 1
                            outer -= 1
                            if outer <= 0:
                                break
                        if not (self.lookup_index < lookup_bytes - 1):
                            break
                else:
                    for i in range(lookup_bytes * 2):
                        lookup_table[i] = self.next_nibble()
                if self.nibble_flag:
                    self.ps += 1
                    self.nibble_flag = False
                if (lookup_bytes + src_size) & 1:
                    self.ps += 1
                table_offset = 0
                self.lookup_index = 0
                if decom_code & 2:
                    while True:
                        outer = self.desanalyse()
                        inner = self.desanalyse()
                        self.lookup_index += inner
                        if self.nibble_flag:
                            self.ps += 1
                            self.lookup_index += 1
                            self.nibble_flag = False
                        start = self.ps
                        while True:
                            self.ps = start
                            for _ in range(inner):
                                src_buffer[table_offset] = s[self.ps]
                                table_offset += 1
                                self.ps += 1
                            outer -= 1
                            if outer <= 0:
                                break
                        if not (self.lookup_index < src_size - 1):
                            break
                else:
                    src_buffer[0:src_size] = s[self.ps:self.ps + src_size]
                    self.ps += src_size
                if self.nibble_flag:
                    self.ps += 1
                ps_start = self.ps
                self.pd = self.yp * WIDTH + self.xp
                self.mem = src_buffer
                self.ps = 0
                self.pl = -1
                self.lookup_value = self.lookup_index = 0
                self.nibble_flag = False
                decom_index = decom_code >> 8
            self.lookup_value = 0
            getattr(self, f"_m{decom_index}", self._unknown)(decom_index, lookup_bytes)
            if decom_index == 0:
                ps_start = self._ps_start0
            cur = ps_start
        # copie dans l'image finale
        if self.w <= 0 or self.h <= 0:
            raise GfxError("taille d'image nulle")
        pix = bytearray()
        for y in range(self.h):
            o = MARGIN + y * WIDTH
            pix += self.out[o:o + self.w]
        # Les quartets décodés sont dans l'ordre des plans de bits Atari : l'indice de
        # palette s'obtient en inversant les 4 bits (même table dans maup.exe, DS:0xEA6).
        # Vérifié sur une capture d'écran de la version Atari.
        pix = pix.translate(BITREV_TABLE)
        tr = BITREV[transparency] if transparency < 16 else transparency
        return Image(self.w, self.h, bytes(pix), tr, (ox, oy), palette)

    def _unknown(self, idx, _lb):
        raise GfxError(f"méthode de compression inconnue {idx}")

    def _incr_xsize(self):
        if self.x_size & 1:
            self.x_size += 1

    # --- méthodes ----------------------------------------------------------------
    def _m0(self, _i, lookup_bytes):
        self.pd = self.yp * WIDTH + self.xp
        start = self.ps
        self._incr_xsize()
        for _y in range(self.y_size):
            line = self.pd
            for _x in range(self.x_size):
                self.out[line + MARGIN] = self.next_nibble()
                line += 1
            self.pd += WIDTH
        self._ps_start0 = start + lookup_bytes + (1 if lookup_bytes & 1 else 0)

    def _m1(self, _i, _lb):
        self._incr_xsize()
        for y in range(self.y_size):
            if y % 2 == 0:
                for _ in range(self.x_size):
                    self.put(self.next_byte())
                    self.pd += 1
            else:
                for _ in range(self.x_size):
                    self.pd -= 1
                    self.put(self.next_byte())
            self.pd += WIDTH

    def _m2(self, _i, _lb):
        for x in range(self.x_size):
            if x % 2 == 0:
                for _ in range(self.y_size):
                    self.put(self.next_byte())
                    self.pd += WIDTH
            else:
                for _ in range(self.y_size):
                    self.pd -= WIDTH
                    self.put(self.next_byte())
            self.pd += 1

    def _m3(self, _i, _lb):
        self.thickness = 2
        self._horizontal()

    def _m4(self, _i, _lb):
        self.thickness = 2
        self._vertical()

    def _m5(self, _i, _lb):
        self.thickness = 3
        self._horizontal()

    def _m6(self, _i, _lb):
        self.thickness = 4
        self._vertical()

    def _m7(self, _i, _lb):
        self._incr_xsize()
        for _y in range(self.y_size):
            line = self.pd
            for _x in range(self.x_size):
                self.out[line + MARGIN] = self.next_byte()
                line += 1
            self.pd += WIDTH

    def _m8(self, _i, _lb):
        for _x in range(self.x_size):
            line = self.pd
            for _y in range(self.y_size):
                self.out[line + MARGIN] = self.next_byte()
                line += WIDTH
            self.pd += 1

    def _m9(self, _i, _lb):
        self.thickness = 4
        self._horizontal()

    def _m10(self, _i, _lb):
        self.thickness = 6
        self._horizontal()

    def _m11(self, _i, _lb):
        self._decom11()

    def _diag_setup(self, incr, thickness, yinc, yend, xinc, xend):
        if incr:
            self._incr_xsize()
        self.thickness = thickness if thickness is not None else self.x_size
        self.y_inc, self.x_inc = yinc, xinc
        self.y_end = yend() if callable(yend) else yend
        self.x_end = xend() if callable(xend) else xend
        self._diag()

    def _m12(self, _i, _lb):
        self._incr_xsize()
        self.thickness = self.x_inc = 1
        self.y_inc = WIDTH
        self.y_end, self.x_end = self.y_size, self.x_size
        self._diag()

    def _m13(self, _i, _lb):
        self._incr_xsize()
        self.thickness = self.x_size
        self.y_inc = 1
        self.y_end = self.x_size
        self.x_inc = WIDTH
        self.x_end = self.y_size
        self._diag()

    def _m14(self, _i, _lb):
        self.thickness = self.y_inc = 1
        self.y_end = self.x_size
        self.x_inc = WIDTH
        self.x_end = self.y_size
        self._diag()

    def _m15(self, _i, _lb):
        self._incr_xsize()
        self.thickness = 2
        self.y_inc = WIDTH
        self.y_end = self.y_size
        self.x_inc = 1
        self.x_end = self.x_size
        self._diag()

    def _m16(self, _i, _lb):
        self.thickness = 3
        self.y_inc = 1
        self.y_end = self.x_size
        self.x_inc = WIDTH
        self.x_end = self.y_size
        self._diag()

    def _m17(self, _i, _lb):
        self._incr_xsize()
        self.thickness = 3
        self.y_inc = WIDTH
        self.y_end = self.y_size
        self.x_inc = 1
        self.x_end = self.x_size
        self._diag()

    def _m18(self, _i, _lb):
        self._incr_xsize()
        self.thickness = 5
        self.y_inc = WIDTH
        self.y_end = self.y_size
        self.x_inc = 1
        self.x_end = self.x_size
        self._diag()

    # --- routines de parcours ----------------------------------------------------
    def _horizontal(self):
        self._incr_xsize()
        end = self.pd + (self.y_size - 1) * WIDTH + self.x_size
        while True:
            if (self.thickness - 1) * WIDTH + self.pd >= end:
                self.thickness -= 1
                if self.thickness == 0:
                    break
                continue
            cont = False
            while True:
                for xi in range(self.x_size):
                    if xi % 2 == 0:
                        if xi:
                            self.pd += 1
                        for _ in range(self.thickness):
                            self.put(self.next_byte())
                            self.pd += WIDTH
                        self.pd += 1
                    else:
                        for _ in range(self.thickness):
                            self.pd -= WIDTH
                            self.put(self.next_byte())
                if self.x_size % 2 == 0:
                    bs = self.thickness * WIDTH
                    self.pd += bs
                    bs -= WIDTH
                    if end < self.pd + bs:
                        while True:
                            self.thickness -= 1
                            if self.thickness == 0:
                                return
                            if not (self.pd + (self.thickness - 1) * WIDTH >= end):
                                break
                else:
                    while self.pd + (self.thickness - 1) * WIDTH >= end:
                        self.thickness -= 1
                        if self.thickness == 0:
                            return
                for xi in range(self.x_size):
                    if xi % 2 == 0:
                        for _ in range(self.thickness):
                            self.put(self.next_byte())
                            self.pd += WIDTH
                    else:
                        for _ in range(self.thickness):
                            self.pd -= WIDTH
                            self.put(self.next_byte())
                    self.pd -= 1
                if self.x_size % 2 == 1:
                    self.pd += 1
                    if self.pd + (self.thickness - 1) * WIDTH < end:
                        cont = True
                        break
                else:
                    self.pd += self.thickness * WIDTH + 1
                    cont = True
                    break
                self.pd += 1
                if not ((self.thickness - 1) * WIDTH + self.pd < end):
                    break
            if cont:
                continue
            self.thickness -= 1
            if self.thickness == 0:
                break

    def _vertical(self):
        draw = 0
        while True:
            while draw + self.thickness > self.x_size:
                self.thickness -= 1
                if self.thickness == 0:
                    return
            for y in range(self.y_size):
                if y % 2 == 0:
                    if y > 0:
                        self.pd += WIDTH
                    draw += self.thickness
                    for _ in range(self.thickness):
                        self.put(self.next_byte())
                        self.pd += 1
                else:
                    self.pd += WIDTH
                    draw -= self.thickness
                    for _ in range(self.thickness):
                        self.pd -= 1
                        self.put(self.next_byte())
            if self.y_size % 2 == 0:
                self.pd += self.thickness
                draw += self.thickness
            while self.x_size < draw + self.thickness:
                self.thickness -= 1
                if self.thickness == 0:
                    return
            for y in range(self.y_size):
                if y % 2 == 0:
                    if y > 0:
                        self.pd -= WIDTH
                    draw += self.thickness
                    for _ in range(self.thickness):
                        self.put(self.next_byte())
                        self.pd += 1
                else:
                    self.pd -= WIDTH
                    draw -= self.thickness
                    for _ in range(self.thickness):
                        self.pd -= 1
                        self.put(self.next_byte())
            if self.y_size % 2 == 0:
                self.pd += self.thickness
                draw += self.thickness

    def _next_ptr(self):
        self.pd += self.x_inc + self.y_inc

    def _decom11(self):
        ypos = draw = 0
        self.y_inc = WIDTH
        self.x_inc = -1
        self.x_size -= 1
        self.y_size -= 1
        area = 0
        while area != -1:
            if area == 0:
                self.put(self.next_byte())
                area = 1
            elif area == 1:
                self._next_ptr()
                if not draw:
                    self.x_inc, self.y_inc = -self.x_inc, -self.y_inc
                    if ypos == self.y_size:
                        self._next_ptr()
                        draw += 1
                    else:
                        ypos += 1
                    self.pd += 1
                    self.put(self.next_byte())
                    area = 2
                elif ypos != self.y_size:
                    ypos += 1
                    draw -= 1
                    area = 0
                else:
                    self.x_inc, self.y_inc = -self.x_inc, -self.y_inc
                    self._next_ptr()
                    draw += 1
                    self.pd += 1
                    self.put(self.next_byte())
                    area = -1 if draw == self.x_size else 2
            elif area == 2:
                self._next_ptr()
                if not ypos:
                    self.x_inc, self.y_inc = -self.x_inc, -self.y_inc
                    if draw == self.x_size:
                        self._next_ptr()
                        ypos += 1
                    else:
                        draw += 1
                    self.pd += WIDTH
                    area = 0
                elif draw != self.x_size:
                    draw += 1
                    ypos -= 1
                    self.put(self.next_byte())
                    area = 2
                else:
                    self.pd += WIDTH
                    ypos += 1
                    self.x_inc, self.y_inc = -self.x_inc, -self.y_inc
                    self._next_ptr()
                    self.put(self.next_byte())
                    area = -1 if ypos == self.y_size else 1

    def _tfp(self, v) -> bool:
        diff = self.y_end - v
        if not diff:
            return True
        if diff < self.thickness + 1:
            self.thickness = diff - 1
        return False

    def _tf1(self, v):
        v += self.thickness + 1
        self.pd += (self.thickness + 1) * self.y_inc
        return v

    def _tf2(self, v):
        v += self.thickness + 1
        for _ in range(self.thickness + 1):
            self.put(self.next_byte())
            self.pd += self.y_inc
        return v

    def _diag(self):
        diag = draw = 0
        self.x_end -= 1
        while not self._tfp(diag):
            while True:
                self.x_inc = -self.x_inc
                for _ in range(self.thickness + 1):
                    self.put(self.next_byte())
                    self.x_inc = -self.x_inc
                    self._next_ptr()
                self.y_inc = -self.y_inc
                self.pd += self.y_inc
                for _ in range(self.thickness + 1):
                    self.put(self.next_byte())
                    self.x_inc = -self.x_inc
                    self._next_ptr()
                self.x_inc, self.y_inc = -self.x_inc, -self.y_inc
                self._next_ptr()
                draw += 1
                if self.x_end < draw + 1:
                    diag = self._tf1(diag)
                    break
                self.pd += self.x_inc
                draw += 1
                if self.x_end < draw + 1:
                    diag = self._tf2(diag)
                    break
            if self._tfp(diag):
                break
            while True:
                for _ in range(self.thickness + 1):
                    self.put(self.next_byte())
                    self.x_inc = -self.x_inc
                    self._next_ptr()
                self.y_inc = -self.y_inc
                self.pd += self.y_inc
                for _ in range(self.thickness + 1):
                    self.put(self.next_byte())
                    self.x_inc = -self.x_inc
                    self._next_ptr()
                self.x_inc, self.y_inc = -self.x_inc, -self.y_inc
                self._next_ptr()
                draw -= 1
                if draw == 0:
                    diag = self._tf1(diag)
                    self.x_inc = -self.x_inc
                    break
                self.pd += self.x_inc
                draw -= 1
                if draw == 0:
                    diag = self._tf2(diag)
                    self.x_inc = -self.x_inc
                    break
                self.x_inc = -self.x_inc


def decode_image(data: bytes) -> Image:
    try:
        return _Decoder(data).decode()
    except (IndexError, ValueError) as e:
        raise GfxError(f"données d'image incohérentes : {e}") from None


def image_size(data: bytes) -> int:
    """Nombre d'octets occupés par une image (pour la retrouver dans un bloc)."""
    count = data[1]
    p = 4 + (30 if data[0] == 0 else 0)
    for _ in range(count):
        size = _be16(data, p) + _be16(data, p + 2)
        if size & 1:
            size += 1
        p += size + 14
    return p


def st_palette_to_rgb(words: list[int]) -> list[tuple[int, int, int]]:
    """Palette Atari ST (3 bits par composante, 0x0RGB) vers RGB 8 bits."""
    out = []
    for w in words:
        r, g, b = (w >> 8) & 7, (w >> 4) & 7, w & 7
        out.append((r * 255 // 7, g * 255 // 7, b * 255 // 7))
    return out


def parse_palette(data: bytes, off: int = 0) -> list[int]:
    return list(struct.unpack_from(">16H", data, off))
