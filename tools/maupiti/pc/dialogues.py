"""Attribution statique des répliques aux personnages, lue directement dans le bloc « logique ».

Établi par le désassemblage de l'écran d'interrogatoire (MAUPITI1 : fonction 0x87D4) puis vérifié
contre l'oracle (tools/emu) : la réponse d'un personnage est un mot big-endian d'une **matrice de
9 colonnes** (une par personnage, dans l'ordre du menu : Maguy, Sue, Anita, Bruce, Roy, Chris, Bob,
Anton, Juste) :

    bloc 1..3, 5  : ligne = choix du menu, colonne = locuteur
    bloc 4        : ligne = locuteur, colonne = personne dont on parle (« votre avis sur... »)
    bloc 135      : 202 situations (réactions aux objets montrés/donnés...), colonne = locuteur
    0 = pas de réponse ; 289-291 = « je ne sais pas » (tirage au hasard si la case est 0)
ATTENTION : les numéros de la logique (matrices, service 30) commencent à 1 : la valeur u désigne la
réplique k = u - 1 du script phonétique, c'est-à-dire le texte 1137 + u = 1138 + k. Les fonctions de ce
module renvoient k (numérotation des fichiers de voix : <k>.wav = texte 1138 + k).
Les autres blocs de règles (types 1 à 5 du moteur 0x63A3 : blocs 0xC8-0xE7 et 0x1EA-0x1F3) restent à lire.
"""
from __future__ import annotations

from .logic import parse_blocks, words

MATRICES = (1, 2, 3, 4, 5, 135)
GREETING = 368          # valeur logique de l'accueil = 368 + indice du personnage (k = 367 + indice)
FALLBACKS = (289, 290, 291, 297, 362, 375, 381, 382)       # esquives génériques


def static_attribution(logic: bytes) -> dict[int, dict[int, list[tuple[int, int]]]]:
    """{personnage: {réplique: [(bloc, ligne ou colonne), ...]}}"""
    blocks = parse_blocks(logic)
    out: dict[int, dict[int, list[tuple[int, int]]]] = {c: {} for c in range(9)}
    for n in MATRICES:
        w = words(blocks[n])
        for r in range(len(w) // 9):
            for c in range(9):
                v = w[r * 9 + c]
                if 0 < v <= 450:
                    spk, item = (r, c) if n == 4 else (c, r)
                    out[spk].setdefault(v - 1, []).append((n, item))       # u (1-based) -> k
    for c in range(9):
        out[c].setdefault(GREETING + c - 1, []).append((0, c))
    return out


# --- Règles « Argumenter » (moteur 0x63A3) ---------------------------------------------------------
# Type t : en-tête RULE_HEADERS[t-1] = [N, arité de chacune des N+1 règles] ; bloc suivant = clés (mots) ;
# puis pour chaque étape s (0, 1, 2) : un bloc d'octets (type de la déclaration opposée, une valeur par
# règle d'arité > s) et un bloc de mots [clé opposée × n][réplique opposée × n][nouvelle réponse × n].
# Une règle se déclenche quand on oppose au personnage, sur la question « clé », la déclaration décrite
# (type, clé, réplique), ou sous la contrainte (tabasser, soudoyer…) : étape suivante à chaque fois.
RULE_HEADERS = (0xCA, 0xCE, 0xD6, 0xDC, 0xE4)
QUESTION_TYPES = {1: "Votre emploi du temps", 2: "Qu'avez-vous remarqué", 3: "Que faites-vous là ?",
                  4: "Votre avis sur", 5: "Sujet / avis sur Marie, Lucie"}
KEY_STRIDE = {1: 40, 2: 40, 3: 74, 4: 9, 5: 22}     # clé = personnage × pas + élément (code de 0xADD4)


def split_key(t: int, key: int) -> tuple[int, int]:
    """Clé d'une question -> (personnage, élément : créneau, heure - 20, personne, ligne du bloc 5)."""
    return divmod(key, KEY_STRIDE[t])


def rules(logic: bytes) -> list[dict]:
    blocks = parse_blocks(logic)
    out = []
    for t, h in enumerate(RULE_HEADERS, 1):
        head = blocks[h]
        n = head[0] + 1
        arity = list(head[1:1 + n])
        keys = words(blocks[h + 1])
        for stage in range(3):
            members = [r for r in range(n) if arity[r] > stage]
            if not members:
                break
            ctype = blocks[h + 2 + 2 * stage]
            data = words(blocks[h + 3 + 2 * stage])
            cnt = len(members)
            for i, r in enumerate(members):
                spk, item = split_key(t, keys[r])
                ct = ctype[i] if i < len(ctype) else 0
                ckey = data[i] if i < len(data) else 0
                cu = data[cnt + i] if cnt + i < len(data) else 0
                ans = data[2 * cnt + i] if 2 * cnt + i < len(data) else 0
                cspk, citem = split_key(ct, ckey) if ct in KEY_STRIDE else (None, None)
                out.append({"type": t, "regle": r, "etape": stage, "personnage": spk, "element": item,
                            "contre_type": ct, "contre_personnage": cspk, "contre_element": citem,
                            "contre_replique": cu - 1 if cu else None, "reponse": ans - 1 if ans else None})
    return out
