"""Animations des lieux Atari : entrée 3 de l'archive d'un lieu (docs/architecture.md §13).

Table de décalages (mots, terminée par 0 ; relatifs au début - 2), puis des enregistrements typés. Type 0 =
animation (moteur : archive 0, $128C8) :
    +0 type, +2 taille de l'espace de travail, +4 emplacement de l'image décodée dans cet espace,
    +6 décalages de descripteurs (relatifs : position après le mot - 2 + valeur) jusqu'à FFFF,
       puis décalage de l'image en bande (format Lankhor : toutes les images côte à côte).
Descripteur : programme (entrées de 4 mots ; FFFE = boucle ; FFFF = fin), tables de mots terminées par un
double FFFF, puis position écran (mot : décalage dans la mémoire vidéo ST, 160 octets par ligne, 8 octets par
bloc de 16 pixels), largeur en blocs - 1, hauteur - 1, et décalages des images dans la bande (16 octets par bloc
de 16 pixels) jusqu'à un mot négatif.
"""
from __future__ import annotations

import struct


def _w(d, o):
    return struct.unpack_from(">H", d, o)[0]


def parse(d: bytes):
    """Renvoie [{x, y, w, h, frames: [décalages], image: position de la bande, record}] ; une image k est en
    (décalage % octets_par_ligne) // 8 * 16, décalage // octets_par_ligne dans la bande, avec
    octets_par_ligne = ceil(largeur de la bande / 16) * 8."""
    out = []
    offsets = []
    o = 0
    while o + 2 <= len(d):
        v = _w(d, o)
        o += 2
        if v == 0:
            break
        offsets.append(v - 2)
    for r in offsets:
        if r + 8 > len(d) or _w(d, r) != 0:
            continue
        o = r + 6
        descs = []
        while o + 2 <= len(d):
            v = _w(d, o)
            o += 2
            if v & 0x8000:
                break
            descs.append(o - 2 + v)
        image = o + _w(d, o) if o + 2 <= len(d) else None
        for a in descs:
            p = a
            # programme : entrées de 4 mots jusqu'à FFFF
            while p + 8 <= len(d) and _w(d, p) != 0xFFFF:
                p += 8
            p += 2
            # tables jusqu'au double FFFF
            while p + 2 <= len(d) and not (_w(d, p) == 0xFFFF and _w(d, p - 2) == 0xFFFF):
                p += 2
            p += 2
            if p + 6 > len(d):
                continue
            scr, wb, h = _w(d, p), _w(d, p + 2), _w(d, p + 4)
            p += 6
            frames = []                  # décalages en octets dans l'image décodée (plans ST, lignes complètes)
            while p + 2 <= len(d) and not (_w(d, p) & 0x8000):
                frames.append(_w(d, p))
                p += 2
            out.append({"x": (scr % 160) // 8 * 16, "y": scr // 160, "w": (wb + 1) * 16, "h": h + 1,
                        "frames": frames, "image": image, "record": r})
    return out
