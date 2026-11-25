#!/usr/bin/env python3
"""Désassemblage linéaire (aide à l'analyse). Nécessite : pip install capstone

    python tools/re/disasm.py x86 maup.exe maup.asm        # exécutable MZ (image extraite)
    python tools/re/disasm.py x86img maupiti1.img m1.asm   # image brute (après unlzexe)
    python tools/re/disasm.py m68k loader.bin loader.asm 0x148E8
"""
import struct
import sys

import capstone


def main():
    kind, src, dst = sys.argv[1:4]
    base = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0
    d = open(src, "rb").read()
    if kind == "x86":
        h = struct.unpack("<14H", d[:28])
        size = h[2] * 512 - (512 - h[1] if h[1] else 0)
        d = d[h[4] * 16:size]
    mode = (capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16) if kind.startswith("x86")
            else capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_000))
    with open(dst, "w") as out:
        off = 0
        while off < len(d):
            n = 0
            for i in mode.disasm(d[off:], base + off):
                out.write(f"{i.address:06x}: {i.mnemonic:8s} {i.op_str}\n")
                off = i.address - base + i.size
                n += 1
            if off < len(d):
                out.write(f"{base + off:06x}: db       {d[off]:#04x}\n")
                off += 1


if __name__ == "__main__":
    main()
