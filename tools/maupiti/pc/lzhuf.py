"""Décompression LZHUF (H. Yoshizaki / H. Okumura, 1988) — fichiers .HUF du PC.

Routine identifiée dans maup.exe ($9C6A) : LZSS (fenêtre 4096, F=60, seuil 2)
+ Huffman adaptatif sur 314 symboles. En-tête : u32 LE = taille décompressée.
Les tables d_code / d_len sont celles, standard, de LZHUF.C.
"""
from __future__ import annotations

N = 4096
F = 60
THRESHOLD = 2
N_CHAR = 256 - THRESHOLD + F   # 314
T = N_CHAR * 2 - 1             # 627
R = T - 1
MAX_FREQ = 0x8000


def _d_tables():
    d_code = bytearray()
    for value, count in ((0x00, 32), (0x01, 16), (0x02, 16), (0x03, 16), (0x04, 8), (0x05, 8), (0x06, 8),
                         (0x07, 8), (0x08, 8), (0x09, 8), (0x0A, 8), (0x0B, 8)):
        d_code += bytes([value]) * count
    v = 0x0C
    while len(d_code) < 256:
        n = 4 if v < 0x18 else (2 if v < 0x30 else 1)
        d_code += bytes([v]) * n
        v += 1
    d_len = bytearray()
    for length, count in ((3, 32), (4, 48), (5, 64), (6, 48), (7, 48), (8, 16)):
        d_len += bytes([length]) * count
    return bytes(d_code[:256]), bytes(d_len)


D_CODE, D_LEN = _d_tables()


class LzhufError(Exception):
    pass


def decompress(data: bytes, size: int | None = None) -> bytes:
    if len(data) < 4:
        raise LzhufError("flux trop court")
    textsize = int.from_bytes(data[:4], "little") if size is None else size
    src = data[4:]
    sp = 0
    getbuf = 0
    getlen = 0

    def fill():
        nonlocal sp, getbuf, getlen
        while getlen <= 8:
            b = src[sp] if sp < len(src) else 0
            sp += 1
            getbuf |= (b << (8 - getlen)) & 0xFFFF
            getlen += 8

    def get_bit():
        nonlocal getbuf, getlen
        fill()
        i = getbuf
        getbuf = (getbuf << 1) & 0xFFFF
        getlen -= 1
        return (i >> 15) & 1

    def get_byte():
        nonlocal getbuf, getlen
        fill()
        i = getbuf
        getbuf = (getbuf << 8) & 0xFFFF
        getlen -= 8
        return i >> 8

    freq = [0] * (T + 1)
    prnt = [0] * (T + N_CHAR)
    son = [0] * T
    for i in range(N_CHAR):
        freq[i] = 1
        son[i] = i + T
        prnt[i + T] = i
    i, j = 0, N_CHAR
    while j <= R:
        freq[j] = freq[i] + freq[i + 1]
        son[j] = i
        prnt[i] = prnt[i + 1] = j
        i += 2
        j += 1
    freq[T] = 0xFFFF
    prnt[R] = 0

    def reconst():
        j = 0
        for i in range(T):
            if son[i] >= T:
                freq[j] = (freq[i] + 1) // 2
                son[j] = son[i]
                j += 1
        i, j = 0, N_CHAR
        while j < T:
            k = i + 1
            f = freq[i] + freq[k]
            freq[j] = f
            k = j - 1
            while f < freq[k]:
                k -= 1
            k += 1
            freq[k + 1:j + 1] = freq[k:j]
            freq[k] = f
            son[k + 1:j + 1] = son[k:j]
            son[k] = i
            i += 2
            j += 1
        for i in range(T):
            k = son[i]
            if k >= T:
                prnt[k] = i
            else:
                prnt[k] = prnt[k + 1] = i

    def update(c):
        if freq[R] == MAX_FREQ:
            reconst()
        c = prnt[c + T]
        while True:
            freq[c] += 1
            k = freq[c]
            l = c + 1
            if k > freq[l]:
                l += 1
                while k > freq[l]:
                    l += 1
                l -= 1
                freq[c] = freq[l]
                freq[l] = k
                i = son[c]
                prnt[i] = l
                if i < T:
                    prnt[i + 1] = l
                j = son[l]
                son[l] = i
                prnt[j] = c
                if j < T:
                    prnt[j + 1] = c
                son[c] = j
                c = l
            c = prnt[c]
            if c == 0:
                break

    def decode_char():
        c = son[R]
        while c < T:
            c += get_bit()
            c = son[c]
        c -= T
        update(c)
        return c

    def decode_position():
        i = get_byte()
        c = D_CODE[i] << 6
        j = D_LEN[i] - 2
        while j > 0:
            i = (i << 1) + get_bit()
            j -= 1
        return c | (i & 0x3F)

    text = bytearray(b" ") * N
    r = N - F
    out = bytearray()
    while len(out) < textsize:
        c = decode_char()
        if c < 256:
            out.append(c)
            text[r] = c
            r = (r + 1) & (N - 1)
        else:
            i = (r - decode_position() - 1) & (N - 1)
            for k in range(c - 255 + THRESHOLD):
                c2 = text[(i + k) & (N - 1)]
                out.append(c2)
                text[r] = c2
                r = (r + 1) & (N - 1)
        if sp > len(src) + 8:
            raise LzhufError("données compressées épuisées")
    return bytes(out[:textsize])
