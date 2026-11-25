#!/usr/bin/env python3
"""Trace le démarrage du jeu d'origine dans l'oracle : images, textes, musique, lieu, attentes de clic.

    python tools/emu/startup.py [nombre_de_clics] [x y]

Quand la logique attend une action (boucle de lecture des entrées), un clic est injecté (par défaut au
centre du décor). Les images sont rattachées au bloc de ressource chargé juste avant (service 25 :
identifiant, sous-bloc), ce qui permet de les retrouver dans les données extraites.
Résultat : out/re/startup.json + résumé à l'écran.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import Oracle, ROOT  # noqa: E402
import services  # noqa: E402

DS = 0x114C0


def main():
    nclicks = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    cx, cy = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (130, 90)
    o = Oracle()
    log, loads = [], []                     # loads : (début, fin, id, sous-bloc)
    texts = [t["text"].replace("\n", " ") for t in
             json.load(open(os.path.join(ROOT, "out", "pc", "texts.json"), encoding="utf-8"))]

    def location():
        return int.from_bytes(bytes(o.uc.mem_read(DS + 0x166, 2)), "little")

    def resource_at(addr):
        for a, b, rid, sub in reversed(loads):
            if a <= addr < b:
                return f"id {rid:#x}/{sub} +{addr - a:#x}"
        return f"{addr:#07x}"

    orig_load = o.services[25]

    def load(o_, w):
        data = services.read_resource(w[0] + 0x1E, w[2])
        dest = services.ptr(w, 4)
        loads.append((dest, dest + len(data), w[0] + 0x1E, w[2]))
        return orig_load(o_, w)

    o.services[25] = load

    def hook(n, w):
        loc = location()
        if n == 14:
            if len(w) < 8:
                return
            log.append({"appel": o.calls, "lieu": loc, "type": "image", "source": resource_at(services.ptr(w, 6))})
        elif n in (9, 20):
            tid = w[0]
            log.append({"appel": o.calls, "lieu": loc, "type": "texte", "id": tid,
                        "texte": texts[tid] if tid < len(texts) else "?"})
        elif n in (44, 45, 46, 47, 48, 49, 50, 51):
            log.append({"appel": o.calls, "lieu": loc, "type": f"son/musique (service {n})", "args": w[:4]})
        elif n == 11:
            log.append({"appel": o.calls, "lieu": loc, "type": "chargement de partie"})
        elif n in (16, 24, 42):
            log.append({"appel": o.calls, "lieu": loc, "type": f"service {n}", "args": w[:6]})

    def observer(n, w):
        if n == -1:
            log.append({"appel": o.calls, "lieu": location(), "type": "CLIC", "args": w})
        else:
            hook(n, w)

    o.observer = observer
    o.state["auto_clicks"] = nclicks
    o.state["auto_xy"] = (cx, cy)
    o.run(400_000_000)
    json.dump(log, open(os.path.join(ROOT, "out", "re", "startup.json"), "w", encoding="utf-8"),
              ensure_ascii=False, indent=1)
    last = None
    for e in log:
        key = (e["type"], e.get("id"), e.get("source"))
        if key == last:
            continue
        last = key
        desc = e.get("texte") or e.get("source") or e.get("args", "")
        print(f"[{e['appel']:8d}] lieu {e['lieu']:3d}  {e['type']:28s} {e.get('id', '') or ''} {str(desc)[:80]}")


if __name__ == "__main__":
    main()
