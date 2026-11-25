"""Conteneurs .CON de la version PC (G1.CON … G6.CON).

Un bloc conteneur commence par une table de n offsets u32 little-endian
(le premier vaut 4*n) ; chaque entrée va jusqu'à l'offset suivant (ou la fin).
Les entrées sont elles-mêmes des conteneurs ou des « feuilles ».
Les feuilles reprennent tel quel le format Atari ST (big-endian, palettes ST).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

from ..gfx import GfxError, decode_image, image_size

HUF_MAGIC = b"\xc6\xe3"


@dataclass
class Node:
    path: str                 # ex. "G1/2/2/0"
    offset: int               # dans le fichier
    data: bytes
    children: list["Node"] = field(default_factory=list)
    kind: str = "unknown"
    info: dict = field(default_factory=dict)


def container_offsets(b: bytes) -> list[int] | None:
    if len(b) < 8:
        return None
    (first,) = struct.unpack_from("<I", b, 0)
    if first % 4 or not (4 <= first <= 4 * 400) or first > len(b):
        return None
    n = first // 4
    offs = list(struct.unpack_from(f"<{n}I", b, 0))
    if offs[0] != first:
        return None
    for a, c in zip(offs, offs[1:]):
        if c < a or c > len(b):
            return None
    return offs


def image_run(b: bytes, start: int = 0) -> list[tuple[int, int]]:
    """Suite d'images consécutives : [(offset, taille)], s'arrête à la première invalide."""
    out = []
    p = start
    while p + 18 <= len(b):
        if b[p] not in (0, 1) or b[p + 1] == 0:
            break
        try:
            n = image_size(b[p:])
            if n > len(b) - p:
                break
            decode_image(b[p:p + n])
        except (GfxError, IndexError, ValueError):
            break
        out.append((p, n))
        p += n
    return out


def classify(node: Node) -> None:
    b = node.data
    if len(b) >= 6 and b[4:6] == HUF_MAGIC:
        node.kind = "huf"
        return
    # bloc de lieu : palettes A et B puis grande image avec palette intégrée (C)
    if len(b) > 100 and b[64] == 0:
        try:
            n = image_size(b[64:])
            if 64 + n <= len(b) and decode_image(b[64:64 + n]).width >= 200:
                node.kind = "room"
                node.info = {"image_size": n, "trailing": len(b) - 64 - n}
                return
        except (GfxError, IndexError, ValueError):
            pass
    run = image_run(b)
    if run:
        end = run[-1][0] + run[-1][1]
        node.kind = "images"
        node.info = {"count": len(run), "trailing": len(b) - end}
        return


def parse(name: str, data: bytes, path: str | None = None, offset: int = 0, depth: int = 0) -> Node:
    node = Node(path or name, offset, data)
    offs = container_offsets(data) if depth < 64 else None
    if offs is not None:
        node.kind = "container"
        ends = offs[1:] + [len(data)]
        for i, (a, e) in enumerate(zip(offs, ends)):
            node.children.append(parse(name, data[a:e], f"{node.path}/{i}", offset + a, depth + 1))
    else:
        classify(node)
    return node


def walk(node: Node):
    yield node
    for c in node.children:
        yield from walk(c)
