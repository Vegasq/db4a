#!/usr/bin/env python3
"""Minimal PPM -> PNG converter (stdlib only), for viewing rendered frames."""
import struct, sys, zlib

def convert(src, dst, scale=2):
    d = open(src, 'rb').read()
    if not d.startswith(b'P6'):
        raise SystemExit("not a P6 PPM")
    # header: P6 <w> <h> <maxval>, whitespace separated
    parts, i = [], 2
    while len(parts) < 3:
        while i < len(d) and d[i:i+1].isspace(): i += 1
        if d[i:i+1] == b'#':
            while d[i:i+1] != b'\n': i += 1
            continue
        j = i
        while j < len(d) and not d[j:j+1].isspace(): j += 1
        parts.append(int(d[i:j])); i = j
    i += 1
    w, h, _ = parts
    px = d[i:i + w*h*3]

    W, H = w*scale, h*scale
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        row = (y // scale) * w * 3
        for x in range(W):
            o = row + (x // scale) * 3
            raw += px[o:o+3]

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
           + chunk(b'IEND', b''))
    open(dst, 'wb').write(png)
    print("%s -> %s  (%dx%d)" % (src, dst, W, H))

convert(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 2)
