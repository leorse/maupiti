#!/usr/bin/env python3
"""Décompresse un exécutable LZEXE 0.91 (MAUPITI1.EXE) en image mémoire brute.

Usage : python tools/re/unlzexe.py MAUPITI1.EXE sortie.img
La table de relocation n'est pas reconstruite : l'image sert au désassemblage.
"""
import struct
import sys


def unlzexe(d: bytes) -> bytes:
    hdr = struct.unpack("<14H", d[:28])
    if d[0x1C:0x20] != b"LZ91":
        raise ValueError("pas un exécutable LZEXE 0.91")
    src = d[hdr[4] * 16:]
    p = 0
    state = {"buf": 0, "cnt": 0}

    def word():
        nonlocal p
        w = src[p] | (src[p + 1] << 8)
        p += 2
        return w

    state["buf"], state["cnt"] = word(), 16

    def bit():
        b = state["buf"] & 1
        state["buf"] >>= 1
        state["cnt"] -= 1
        if state["cnt"] == 0:
            state["buf"], state["cnt"] = word(), 16
        return b

    def byte():
        nonlocal p
        b = src[p]
        p += 1
        return b

    out = bytearray()
    while True:
        if bit():
            out.append(byte())
            continue
        if not bit():
            ln = bit() * 2 + bit() + 2
            span = byte() - 256
        else:
            lo, hi = byte(), byte()
            span = (((hi & 0xF8) << 5) | lo) - 0x2000
            ln = hi & 7
            if ln == 0:
                ln = byte()
                if ln == 0:
                    break
                if ln == 1:
                    continue
                ln += 1
            else:
                ln += 2
        for _ in range(ln):
            out.append(out[span])
    return bytes(out)


if __name__ == "__main__":
    data = unlzexe(open(sys.argv[1], "rb").read())
    open(sys.argv[2], "wb").write(data)
    print(f"{len(data)} octets")
