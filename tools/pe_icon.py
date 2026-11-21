#!/usr/bin/env python3
"""Extracts a Windows program's icon as a macOS app icon (.icns).

    pe_icon.py GAME.EXE --icns OUT.icns [--png OUT.png] [--group ID]

Reads the icon group Windows shows for the program (the first RT_GROUP_ICON,
or --group), decodes its images (BMP of 1-32 bits with the AND mask, or PNG),
and writes every size an .icns holds (16 to 1024 pixels) with iconutil.
Sizes the program doesn't have are made from its largest image: scaled up by
repeating pixels (old icons are pixel art; smoothing blurs them), then down
by averaging, to the exact size. --png also writes the 1024-pixel image.

Python's standard library only: player/build.sh runs it on the player's
Mac with /usr/bin/python3, on the player's own copy of the game. Exits 1
(with a message) when the program has no icon it can read.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

RT_ICON, RT_GROUP_ICON = 3, 14
PNG_SIG = b"\x89PNG\r\n\x1a\n"


class NoIcon(Exception):
    pass


# --- PE resources --------------------------------------------------------

def resources(data):
    """{type: [(name, bytes)]} in the resource directory's order (names,
    then IDs ascending), each name an int ID or a str; first language only."""
    if data[:2] != b"MZ":
        raise NoIcon("not a Windows program (no MZ header)")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise NoIcon("not a Windows program (no PE header)")
    nsec, opt_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dirs = opt + (96 if magic == 0x10B else 112)
    ndirs = struct.unpack_from("<I", data, dirs - 4)[0]
    if ndirs < 3:
        raise NoIcon("no resources")
    rsrc_rva, rsrc_size = struct.unpack_from("<II", data, dirs + 2 * 8)
    if not rsrc_rva:
        raise NoIcon("no resources")
    sections = []
    for i in range(nsec):
        s = opt + opt_size + 40 * i
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", data, s + 8)
        sections.append((va, max(vsize, rsize), rptr))

    def off(rva):
        for va, size, ptr in sections:
            if va <= rva < va + size:
                return rva - va + ptr
        raise NoIcon("a resource outside the file's sections")

    base = off(rsrc_rva)

    def entries(at):
        named, ids = struct.unpack_from("<HH", data, base + at + 12)
        for i in range(named + ids):
            name, target = struct.unpack_from("<II", data, base + at + 16 + 8 * i)
            if name & 0x80000000:
                p = base + (name & 0x7FFFFFFF)
                n = struct.unpack_from("<H", data, p)[0]
                name = data[p + 2:p + 2 + 2 * n].decode("utf-16-le", "replace")
            yield name, target

    out = {}
    for rtype, t in entries(0):
        if not t & 0x80000000 or rtype not in (RT_ICON, RT_GROUP_ICON):
            continue
        items = []
        for name, n in entries(t & 0x7FFFFFFF):
            while n & 0x80000000:  # the language level: take the first
                n = next(entries(n & 0x7FFFFFFF))[1]
            rva, size = struct.unpack_from("<II", data, base + n)
            p = off(rva)
            items.append((name, data[p:p + size]))
        out[rtype] = items
    return out


def icon_images(data, group=None):
    """[(width, height, bits, bytes)] of the program's icon group."""
    res = resources(data)
    groups = res.get(RT_GROUP_ICON, [])
    if not groups:
        raise NoIcon("no icon in the program")
    if group is not None:
        groups = [g for g in groups if str(g[0]) == str(group)]
        if not groups:
            raise NoIcon("no icon group %s" % group)
    icons = dict(res.get(RT_ICON, []))
    g = groups[0][1]
    count = struct.unpack_from("<H", g, 4)[0]
    images = []
    for i in range(count):
        w, h, _colors, _res, _planes, bits, _size, iid = struct.unpack_from("<BBBBHHIH", g, 6 + 14 * i)
        if iid in icons:
            images.append((w or 256, h or 256, bits, icons[iid]))
    if not images:
        raise NoIcon("the icon group's images are missing")
    return images


# --- images: (w, h, [r, g, b, a, ...]) -------------------------------------

def decode_bmp(b):
    hsize, w, h2, _planes, bits = struct.unpack_from("<IiiHH", b, 0)
    comp = struct.unpack_from("<I", b, 16)[0]
    if comp not in (0, 3):
        raise NoIcon("a compressed icon image")
    h = abs(h2) // 2
    used = struct.unpack_from("<I", b, 32)[0] if hsize >= 36 else 0
    p = hsize + (12 if comp == 3 and hsize == 40 else 0)
    palette = []
    if bits <= 8:
        n = used or (1 << bits)
        palette = [tuple(b[p + 4 * i:p + 4 * i + 3][::-1]) for i in range(n)]
        p += 4 * n
    stride = ((w * bits + 31) // 32) * 4
    mask_stride = ((w + 31) // 32) * 4
    mask = p + stride * h
    px = bytearray(w * h * 4)
    any_alpha = False
    for y in range(h):
        row = p + stride * (h - 1 - y)
        mrow = mask + mask_stride * (h - 1 - y)
        for x in range(w):
            if bits == 32:
                bl, g, r, a = b[row + 4 * x:row + 4 * x + 4]
                any_alpha |= a != 0
            elif bits == 24:
                bl, g, r = b[row + 3 * x:row + 3 * x + 3]
                a = 255
            elif bits == 16:
                v = struct.unpack_from("<H", b, row + 2 * x)[0]
                r, g, bl, a = (v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31, 255
            else:
                per = 8 // bits
                v = b[row + x // per] >> (8 - bits * (x % per + 1)) & ((1 << bits) - 1)
                r, g, bl = palette[v] if v < len(palette) else (0, 0, 0)
                a = 255
            if bits != 32 and mask + mask_stride * h <= len(b):
                if b[mrow + x // 8] >> (7 - x % 8) & 1:
                    a = 0
            i = 4 * (y * w + x)
            px[i:i + 4] = bytes((r, g, bl, a))
    if bits == 32 and not any_alpha:  # alpha all zero: the AND mask says what shows
        for y in range(h):
            mrow = mask + mask_stride * (h - 1 - y)
            for x in range(w):
                visible = mask + mask_stride * h > len(b) or not b[mrow + x // 8] >> (7 - x % 8) & 1
                px[4 * (y * w + x) + 3] = 255 if visible else 0
    return w, h, px


def decode_png(b):
    p, chunks = 8, []
    while p < len(b):
        n, kind = struct.unpack_from(">I4s", b, p)
        chunks.append((kind, b[p + 8:p + 8 + n]))
        p += 12 + n
    ihdr = dict(chunks)[b"IHDR"]
    w, h, depth, ctype, _c, _f, interlace = struct.unpack(">IIBBBBB", ihdr)
    if depth != 8 or ctype not in (2, 6) or interlace:
        raise NoIcon("a PNG icon image this can't read (depth %d, type %d)" % (depth, ctype))
    ch = 4 if ctype == 6 else 3
    raw = zlib.decompress(b"".join(d for k, d in chunks if k == b"IDAT"))
    stride = w * ch
    out, prev, q = bytearray(), bytearray(stride), 0
    for _ in range(h):
        f, line = raw[q], bytearray(raw[q + 1:q + 1 + stride])
        q += 1 + stride
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            up = prev[i]
            c = prev[i - ch] if i >= ch else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + up) & 255
            elif f == 3:
                line[i] = (line[i] + (a + up) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(up - c), abs(a - c), abs(a + up - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else up if pb <= pc else c)) & 255
        out += line if ch == 4 else b"".join(bytes(line[i:i + 3]) + b"\xff" for i in range(0, stride, 3))
        prev = line
    return w, h, out


def decode(img):
    w, h, bits, b = img
    return decode_png(b) if b[:8] == PNG_SIG else decode_bmp(b)


def scale(src, size):
    """src scaled to size x size: up by repeating pixels, then down by averaging."""
    w, h, px = src
    if w != h:  # centre it in a square first
        n = max(w, h)
        sq = bytearray(n * n * 4)
        ox, oy = (n - w) // 2, (n - h) // 2
        for y in range(h):
            sq[4 * ((y + oy) * n + ox):4 * ((y + oy) * n + ox + w)] = px[4 * y * w:4 * (y + 1) * w]
        w = h = n
        px = sq
    k = max(1, -(-size // w))  # repeat each pixel k times
    if k > 1:
        big = bytearray()
        for y in range(h):
            row = b"".join(bytes(px[4 * (y * w + x):4 * (y * w + x) + 4]) * k for x in range(w))
            big += row * k
        w, h, px = w * k, h * k, big
    if w == size:
        return bytes(px)
    out = bytearray(size * size * 4)
    for y in range(size):
        y0, y1 = y * h // size, max((y + 1) * h // size, y * h // size + 1)
        for x in range(size):
            x0, x1 = x * w // size, max((x + 1) * w // size, x * w // size + 1)
            acc = [0, 0, 0, 0]
            for yy in range(y0, y1):
                for xx in range(x0, x1):
                    i = 4 * (yy * w + xx)
                    a = px[i + 3]
                    acc[0] += px[i] * a
                    acc[1] += px[i + 1] * a
                    acc[2] += px[i + 2] * a
                    acc[3] += a
            n = (y1 - y0) * (x1 - x0)
            i = 4 * (y * size + x)
            if acc[3]:
                out[i:i + 4] = bytes((acc[0] // acc[3], acc[1] // acc[3], acc[2] // acc[3], acc[3] // n))
    return bytes(out)


def png(size, px):
    rows = b"".join(b"\0" + px[4 * size * y:4 * size * (y + 1)] for y in range(size))

    def chunk(kind, d):
        return struct.pack(">I", len(d)) + kind + d + struct.pack(">I", zlib.crc32(kind + d))
    return (PNG_SIG + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


ICONSET = [("16x16", 16), ("16x16@2x", 32), ("32x32", 32), ("32x32@2x", 64), ("128x128", 128),
           ("128x128@2x", 256), ("256x256", 256), ("256x256@2x", 512), ("512x512", 512), ("512x512@2x", 1024)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("exe")
    ap.add_argument("--icns", required=True)
    ap.add_argument("--png")
    ap.add_argument("--group")
    a = ap.parse_args()
    try:
        with open(a.exe, "rb") as f:
            images = icon_images(f.read(), a.group)
        decoded = []
        for img in images:
            try:
                decoded.append((img, decode(img)))
            except (NoIcon, struct.error, IndexError, zlib.error):
                pass
        if not decoded:
            raise NoIcon("no icon image it can read")
    except (NoIcon, struct.error, OSError) as e:
        sys.exit("pe_icon: %s: %s" % (a.exe, e))
    # The best image of each size (most colours), and the largest overall.
    best = {}
    for (w, h, bits, _b), im in decoded:
        if w not in best or bits > best[w][0]:
            best[w] = (bits, im)
    largest = best[max(best)][1]
    tmp = tempfile.mkdtemp()
    try:
        iconset = os.path.join(tmp, "icon.iconset")
        os.mkdir(iconset)
        cache = {}
        for name, size in ICONSET:
            if size not in cache:
                src = best[size][1] if size in best and best[size][1][0] == best[size][1][1] else largest
                cache[size] = png(size, scale(src, size))
            with open(os.path.join(iconset, "icon_%s.png" % name), "wb") as f:
                f.write(cache[size])
        r = subprocess.run(["/usr/bin/iconutil", "-c", "icns", "-o", a.icns, iconset],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit("pe_icon: iconutil: %s" % (r.stderr.strip() or r.returncode))
        if a.png:
            with open(a.png, "wb") as f:
                f.write(cache[1024])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    sizes = ", ".join("%dx%d %d-bit" % (w, w, best[w][0]) for w in sorted(best))
    print("pe_icon: %s (from %s)" % (a.icns, sizes))


if __name__ == "__main__":
    main()
