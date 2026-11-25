#!/usr/bin/env python3
"""Outils d'exploration de l'oracle : instantané du jeu au repos, appel direct d'une fonction.

    python tools/emu/probe.py snapshot            # ~2 min : démarre le jeu, sauve out/re/idle.snap
    python tools/emu/probe.py interrog CHAR       # lance l'interrogatoire d'un personnage (0..8)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import Oracle, ROOT  # noqa: E402

SNAP = os.path.join(ROOT, "out", "re", "idle.snap")


def main():
    if sys.argv[1] == "snapshot":
        o = Oracle()
        o.run(60_000_000)
        o.snapshot(SNAP)
        print("instantané :", SNAP)
    elif sys.argv[1] == "interrog":
        o = Oracle(trace=True)
        o.quiet = {3, 4, 5, 15, 31, 32, 33, 12, 13, 36, 37, 22, 39, 14}
        o.restore(SNAP)
        o.calls = 0
        o.state["events"] = []
        ok = o.call_near(0xADD4, [int(sys.argv[2]) & 0xFFFF, 0], max_insn=20_000_000)
        print("retourné :", ok, "| voix :", o.state.get("voice_log"))


if __name__ == "__main__":
    main()
