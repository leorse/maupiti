#!/usr/bin/env python3
"""Balaye l'écran d'interrogatoire : un clic à chaque (x, y) pour un personnage ; relève les répliques dites.

    python tools/emu/scan.py CHAR [xstep ystep]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import Oracle  # noqa: E402
from probe import SNAP  # noqa: E402


def run(o, char, clicks, max_insn=30_000_000):
    o.restore(SNAP)
    o.calls = 0
    o.state["events"] = list(clicks)
    o.state["voice_log"] = []
    o.call_near(0xADD4, [char & 0xFFFF, 0], max_insn=max_insn)
    return [(w[0], w[2] if w[2] < 0x8000 else w[2] - 0x10000) for w in o.state["voice_log"]]


if __name__ == "__main__":
    char = int(sys.argv[1])
    xs, ys = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (40, 10)
    o = Oracle()
    t0 = time.time()
    base = run(o, char, [])
    print("sans clic :", base, f"({time.time() - t0:.1f} s)")
    for y in range(4, 200, ys):
        row = []
        for x in range(10, 320, xs):
            v = run(o, char, [(x, y, 1)])
            extra = [u for u in v if u not in base]
            row.append(f"{extra[0][0]}" if extra else ".")
        print(f"y={y:3d}", " ".join(f"{c:>4s}" for c in row), flush=True)
