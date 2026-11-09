#!/usr/bin/env python3
"""Writes the PNG decoder's test cases (tests/png_test.c) into tests/data/png/:
NAME.png and NAME.rgba (the pixels it must decode to, w x h RGBA). Encoded
here with Python's zlib, so the decoder meets another encoder's output:
every filter type, colour type and bit depth, tRNS, Adam7, stored,
fixed and dynamic Huffman blocks. Re-run after changing a case."""
import os, struct, zlib, random

OUT = os.path.join(os.path.dirname(__file__), '..', 'tests', 'data', 'png')

def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)

def paeth(a, b, c):
    p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c

def filt(rows, bpp, ftype):
    out, prev = b'', bytes(len(rows[0])) if rows else b''
    for i, r in enumerate(rows):
        f = ftype if ftype >= 0 else i % 5
        o = bytearray()
        for x in range(len(r)):
            a = r[x - bpp] if x >= bpp else 0
            b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
            pred = [0, a, b, (a + b) // 2, paeth(a, b, c)][f]
            o.append((r[x] - pred) & 255)
        out += bytes([f]) + bytes(o); prev = r
    return out

def pack_row(samples, depth):
    if depth == 8: return bytes(samples)
    if depth == 16: return b''.join(struct.pack('>H', s) for s in samples)
    bits = ''.join(format(s, '0%db' % depth) for s in samples)
    bits += '0' * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))

def png(w, h, ctype, depth, px, plte=None, trns=None, interlace=0, ftype=-1, level=9):
    """px: rows of per-pixel sample tuples as stored"""
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, ch * depth // 8)
    passes = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)] if interlace else [(0, 0, 1, 1)]
    raw = b''
    for xs, ys, dx, dy in passes:
        rows = []
        for y in range(ys, h, dy):
            samples = [s for x in range(xs, w, dx) for s in px[y][x]]
            if samples: rows.append(pack_row(samples, depth))
        if rows: raw += filt(rows, bpp, ftype)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, ctype, 0, 0, interlace))
    if plte: data += chunk(b'PLTE', b''.join(bytes(c) for c in plte))
    if trns is not None: data += chunk(b'tRNS', trns)
    z = zlib.compress(raw, level)
    # two IDATs: the decoder must join them
    data += chunk(b'IDAT', z[:len(z) // 2]) + chunk(b'IDAT', z[len(z) // 2:]) + chunk(b'IEND', b'')
    return data

def to8(v, d): return v >> 8 if d == 16 else v if d == 8 else v * 255 // ((1 << d) - 1)

def write(name, w, h, ctype, depth, px, plte=None, trns=None, **kw):
    data = png(w, h, ctype, depth, px, plte, trns, **kw)
    rgba = bytearray()
    for y in range(h):
        for x in range(w):
            s = px[y][x]
            if ctype == 0:
                g = to8(s[0], depth); a = 0 if trns is not None and s[0] == struct.unpack('>H', trns)[0] else 255
                rgba += bytes([g, g, g, a])
            elif ctype == 2:
                a = 0 if trns is not None and tuple(s) == struct.unpack('>HHH', trns) else 255
                rgba += bytes([to8(v, depth) for v in s] + [a])
            elif ctype == 3:
                c = plte[s[0]]; a = trns[s[0]] if trns and s[0] < len(trns) else 255
                rgba += bytes(list(c) + [a])
            elif ctype == 4:
                g = to8(s[0], depth); rgba += bytes([g, g, g, to8(s[1], depth)])
            else:
                rgba += bytes(to8(v, depth) for v in s)
    open(os.path.join(OUT, name + '.png'), 'wb').write(data)
    open(os.path.join(OUT, name + '.rgba'), 'wb').write(struct.pack('>II', w, h) + bytes(rgba))

def main():
    os.makedirs(OUT, exist_ok=True)
    R = random.Random(7)
    # a 32x32 RGBA icon-ish gradient: big enough for dynamic Huffman
    px = [[((x * 8) & 255, (y * 8) & 255, (x * y) & 255, 255 if (x - 16) ** 2 + (y - 16) ** 2 < 200 else (x * 3) & 255) for x in range(32)] for y in range(32)]
    write('rgba8_dynamic', 32, 32, 6, 8, px)
    write('rgba8_stored', 5, 4, 6, 8, [[tuple(R.randrange(256) for _ in range(4)) for x in range(5)] for y in range(4)], level=0)
    write('rgba8_fixed', 3, 5, 6, 8, [[(10 * x, 20 * y, 7, 200) for x in range(3)] for y in range(5)], level=1)
    for f in range(5):
        write('rgb8_filter%d' % f, 7, 6, 2, 8, [[tuple(R.randrange(256) for _ in range(3)) for x in range(7)] for y in range(6)], ftype=f)
    write('rgb16_trns', 4, 3, 2, 16, [[(1000 * x, 65535 - 999 * y, 4242 if x == 1 and y == 1 else 5) for x in range(4)] for y in range(3)], trns=struct.pack('>HHH', 1000, 65535 - 999, 4242))
    for d in (1, 2, 4, 8, 16):
        write('grey%d' % d, 11, 3, 0, d, [[((x + y * 5) % (1 << d),) for x in range(11)] for y in range(3)], trns=struct.pack('>H', 1) if d == 2 else None)
    plte = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (9, 9, 9), (200, 100, 50)]
    for d in (1, 2, 4, 8):
        n = min(len(plte), 1 << d)
        write('plte%d' % d, 9, 4, 3, d, [[((x * 3 + y) % n,) for x in range(9)] for y in range(4)], plte=plte[:n], trns=bytes([128, 0]))
    write('ga8', 3, 3, 4, 8, [[(x * 80, y * 100) for x in range(3)] for y in range(3)])
    write('ga16', 2, 2, 4, 16, [[(x * 30000, 65535 - y * 30000) for x in range(2)] for y in range(2)])
    write('rgba16', 3, 2, 6, 16, [[tuple(R.randrange(65536) for _ in range(4)) for x in range(3)] for y in range(2)])
    write('adam7_rgba8', 9, 11, 6, 8, [[(x * 25, y * 20, (x + y) * 10, 255 - x) for x in range(9)] for y in range(11)], interlace=1)
    write('adam7_grey1', 13, 5, 0, 1, [[((x ^ y) & 1,) for x in range(13)] for y in range(5)], interlace=1)
    write('adam7_tiny', 1, 1, 6, 8, [[(1, 2, 3, 4)]], interlace=1)

main()
