#!/usr/bin/env python3
"""gen_png_cases.py: tests/png_cases.h, PNGs of every kind for tests/png_unit.c to read.

Each case is a small picture of made-up samples, written here as a PNG by an encoder of this
script's own (every colour type and bit depth, Adam7 or not, each filter in turn, the IDAT split
at odd places, chunks a reader must skip) and deflated by Python's zlib at each of its strategies
(stored, fixed codes, dynamic codes, Huffman only, runs). With it go the hashes of what a reader
must make of it, worked out from the samples by the specification's rules, not by decoding: its
pixels as 0xAARRGGBB, and its palette indexes when it is indexed.

    python3 tools/gen_png_cases.py > tests/png_cases.h
"""
import struct
import zlib

ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)


def rng(seed):
    x = seed or 1
    while True:
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        yield x


def pack_row(samples, depth):
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        return b''.join(struct.pack('>H', s) for s in samples)
    out, acc, n = bytearray(), 0, 0
    for s in samples:
        acc = acc << depth | s
        n += depth
        if n == 8:
            out.append(acc)
            acc, n = 0, 0
    if n:
        out.append(acc << (8 - n))
    return bytes(out)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def filt(row, prior, bpp, t):
    out = bytearray()
    for i, x in enumerate(row):
        a = row[i - bpp] if i >= bpp else 0
        b = prior[i] if prior else 0
        c = prior[i - bpp] if prior and i >= bpp else 0
        out.append((x - [0, a, b, (a + b) >> 1, paeth(a, b, c)][t]) & 0xFF)
    return bytes(out)


def make(w, h, ctype, depth, interlace, strategy, seed, trns=False, pal_n=None, split=0, extra=True):
    """(png bytes, the expected rgba, the expected indexes or None)"""
    ch = CHANNELS[ctype]
    top = (1 << depth) - 1
    r = rng(seed)
    pal = []
    if ctype == 3:
        pal_n = pal_n or (1 << depth)
        pal = [(next(r) & 0xFF, next(r) & 0xFF, next(r) & 0xFF) for _ in range(pal_n)]
    px = []
    for y in range(h):
        row = []
        for x in range(w):
            if ctype == 3:
                row.append([(x * 3 + y * 5 + next(r) % 3) % pal_n])
            else:   # (gradients and noise: runs for the matches, noise for the literals)
                row.append([((x * 37 + y * 11 + c * 91) * (top // 31 + 1) + (next(r) % 7 if (x + y) % 3 else 0)) & top for c in range(ch)])
        px.append(row)
    trns_v = None
    if trns and ctype in (0, 2):
        trns_v = px[h // 2][w // 2][:ch if ctype == 0 else 3]   # (a value that is there)
    alpha = []
    if trns and ctype == 3:
        alpha = [(i * 53) & 0xFF for i in range(min(pal_n, 5))]
    # the expected pixels, by the rules
    exp, idx = [], []
    for y in range(h):
        for x in range(w):
            s = px[y][x]
            hi = (lambda v: v >> 8) if depth == 16 else (lambda v: v)
            if ctype == 3:
                i = s[0]
                a = alpha[i] if i < len(alpha) else 255
                rr, gg, bb = pal[i]
                exp.append(a << 24 | rr << 16 | gg << 8 | bb)
                idx.append(i)
            elif ctype in (0, 4):
                g = hi(s[0]) * 255 // (255 if depth >= 8 else top)
                a = hi(s[1]) if ctype == 4 else (0 if trns_v is not None and s[0] == trns_v[0] else 255)
                exp.append(a << 24 | g * 0x010101)
            else:
                a = hi(s[3]) if ctype == 6 else (0 if trns_v is not None and s[:3] == trns_v else 255)
                exp.append(a << 24 | hi(s[0]) << 16 | hi(s[1]) << 8 | hi(s[2]))
    # the raw stream: each pass's rows, filtered, the filter types in turn
    bits = ch * depth
    bpp = max(1, bits // 8)
    raw = bytearray()
    passes = ADAM7 if interlace else [(0, 0, 1, 1)]
    k = 0
    for x0, y0, dx, dy in passes:
        xs, ys = list(range(x0, w, dx)), list(range(y0, h, dy))
        if not xs or not ys:
            continue
        prior = None
        for y in ys:
            row = pack_row([v for x in xs for v in px[y][x]], depth)
            t = k % 5
            k += 1
            raw += bytes([t]) + filt(row, prior, bpp, t)
            prior = row
    if strategy == 'stored':
        z = zlib.compress(bytes(raw), 0)
    else:
        st = {'fixed': zlib.Z_FIXED, 'dynamic': zlib.Z_DEFAULT_STRATEGY, 'huffman': zlib.Z_HUFFMAN_ONLY, 'rle': zlib.Z_RLE}[strategy]
        c = zlib.compressobj(9, zlib.DEFLATED, 15, 9, st)
        z = c.compress(bytes(raw)) + c.flush()
    out = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, ctype, 0, 0, interlace))
    if extra:
        out += chunk(b'gAMA', struct.pack('>I', 45455)) + chunk(b'tEXt', b'Comment\0made for the tests')
    if ctype == 3:
        out += chunk(b'PLTE', b''.join(bytes(p) for p in pal))
        if alpha:
            out += chunk(b'tRNS', bytes(alpha))
    if trns_v is not None:
        out += chunk(b'tRNS', b''.join(struct.pack('>H', v) for v in trns_v))
    if split:   # (the stream in several IDATs, cut at odd places)
        parts = [z[i:i + split] for i in range(0, len(z), split)]
    else:
        parts = [z]
    for p in parts:
        out += chunk(b'IDAT', p)
    out += chunk(b'IEND', b'')
    return out, exp, (idx if ctype == 3 else None)


def fnv(words, size):
    h = 0xCBF29CE484222325
    for v in words:
        for k in range(size):
            h = ((h ^ (v >> (8 * k) & 0xFF)) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def cases():
    out = []
    n = 0
    kinds = [(0, 1), (0, 2), (0, 4), (0, 8), (0, 16), (2, 8), (2, 16), (3, 1), (3, 2), (3, 4), (3, 8), (4, 8), (4, 16), (6, 8), (6, 16)]
    strategies = ['stored', 'fixed', 'dynamic', 'huffman', 'rle']
    for ctype, depth in kinds:
        for interlace in (0, 1):
            for trns in ((False, True) if ctype in (0, 2, 3) else (False,)):
                n += 1
                w, h = 1 + (n * 7) % 23, 1 + (n * 5) % 19
                strategy = strategies[n % len(strategies)]
                pal_n = None if ctype != 3 else max(1, (1 << depth) - (n % 3))
                png, exp, idx = make(w, h, ctype, depth, interlace, strategy, n, trns, pal_n, split=(n % 4) * 9)
                name = 'type %d, %d bits, %s, %s%s, %dx%d' % (ctype, depth, 'Adam7' if interlace else 'not interlaced', strategy,
                                                             ', tRNS' if trns else '', w, h)
                out.append((name, png, w, h, fnv(exp, 4), fnv(idx, 1) if idx is not None else 0))
    # bigger ones: matches far back, long runs, a dynamic code with many lengths
    for k, (w, h, ctype, depth, st) in enumerate([(64, 48, 2, 8, 'dynamic'), (256, 64, 3, 8, 'dynamic'), (48, 40, 6, 8, 'fixed')]):
        png, exp, idx = make(w, h, ctype, depth, 0, st, 1000 + k, False, 200 if ctype == 3 else None)
        out.append(('%dx%d, type %d, %s' % (w, h, ctype, st), png, w, h, fnv(exp, 4), fnv(idx, 1) if idx is not None else 0))
    return out


def main():
    print('/* PNGs of every kind for tests/png_unit.c, made by tools/gen_png_cases.py (an encoder of its own,')
    print(' * deflated by Python\'s zlib): each with the hashes of the pixels and indexes a reader must make. */')
    print('static const struct png_case { const char *name; const unsigned char *png; unsigned size; int w, h; unsigned long long rgba, index; } png_cases[] = {')
    for name, png, w, h, hr, hi in cases():
        body = ','.join(str(b) for b in png)
        print('    { "%s", (const unsigned char[]){ %s }, %d, %d, %d, 0x%016XULL, 0x%016XULL },' % (name, body, len(png), w, h, hr, hi))
    print('};')


if __name__ == '__main__':
    main()
