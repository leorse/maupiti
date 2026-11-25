"""Écriture PNG minimale (palette 8 bits), sans dépendance externe."""
from __future__ import annotations

import struct
import zlib


def _chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def write_indexed(path: str, width: int, height: int, pixels: bytes,
                  palette: list[tuple[int, int, int]], transparent: int | None = None,
                  scale: int = 1) -> None:
    pal = list(palette) + [(255, 0, 255)] * (256 - len(palette))
    rows = bytearray()
    for y in range(height):
        line = pixels[y * width:(y + 1) * width]
        if scale > 1:
            line = bytes(b for b in line for _ in range(scale))
        for _ in range(scale):
            rows.append(0)
            rows += line
    w, h = width * scale, height * scale
    out = b"\x89PNG\r\n\x1a\n"
    out += _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0))
    out += _chunk(b"PLTE", b"".join(bytes(c) for c in pal[:256]))
    if transparent is not None and 0 <= transparent < 256:
        trns = bytes([255] * transparent + [0])
        out += _chunk(b"tRNS", trns)
    out += _chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    out += _chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(out)
