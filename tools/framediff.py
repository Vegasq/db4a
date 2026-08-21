#!/usr/bin/env python3
"""Compare two PPM framebuffers: exact-match rate, per-pixel deltas, diff map.

This is the correctness oracle for the faithful-first fidelity policy. Any
visible difference against the reference emulator is a bug by definition.
"""
import sys

def read_ppm(path):
    d = open(path, 'rb').read()
    if not d.startswith(b'P6'):
        raise SystemExit("%s: not a P6 PPM" % path)
    vals, i = [], 2
    while len(vals) < 3:
        while d[i:i+1].isspace(): i += 1
        if d[i:i+1] == b'#':
            while d[i:i+1] != b'\n': i += 1
            continue
        j = i
        while not d[j:j+1].isspace(): j += 1
        vals.append(int(d[i:j])); i = j
    i += 1
    w, h, _ = vals
    return w, h, d[i:i + w*h*3]

def q565(t):
    """Round-trip a colour through RGB565, matching the oracle's lossy path.

    The reference core emits RGB565, so its 8-bit values have already lost
    precision (5 bits R/B, 6 bits G) while the renderer works from 9-bit CRAM
    directly. Without matching that loss the comparison carries a noise floor
    and cannot distinguish a real defect from format conversion.

    Quantise by TRUNCATION (>>3, >>2) -- what packing into RGB565 actually
    does -- then expand exactly as refhost.c does. Scaling proportionally
    instead leaves the green channel off by 4 and manufactures thousands of
    phantom mismatches.
    """
    r, g, b = t
    r5, g6, b5 = r >> 3, g >> 2, b >> 3
    return (r5 * 255 // 31, g6 * 255 // 63, b5 * 255 // 31)

def main(argv):
    # Flags may appear anywhere; everything else is positional.
    quant = '--quantize' in argv
    pos = [a for a in argv if not a.startswith('--')]
    if len(pos) < 2:
        raise SystemExit("usage: framediff.py A.ppm B.ppm [diff.ppm] [--quantize]")
    a_path, b_path = pos[0], pos[1]
    diff_path = pos[2] if len(pos) > 2 else None
    aw, ah, a = read_ppm(a_path)
    bw, bh, b = read_ppm(b_path)
    if quant: print("(A quantised through RGB565 to match the oracle path)")
    print("A %s  %dx%d" % (a_path, aw, ah))
    print("B %s  %dx%d" % (b_path, bw, bh))
    if (aw, ah) != (bw, bh):
        print("SIZE MISMATCH -- cannot compare")
        return 1

    n = aw * ah
    exact = 0
    close = 0          # within one 3-bit palette step (~36 per channel)
    worst = 0
    diff = bytearray(n * 3)
    for p in range(n):
        o = p * 3
        pa, pb = tuple(a[o:o+3]), tuple(b[o:o+3])
        if quant:
            pa = q565(pa)
        if pa == pb:
            exact += 1
            diff[o:o+3] = b'\x00\x00\x00'
        else:
            d = max(abs(pa[k] - pb[k]) for k in range(3))
            worst = max(worst, d)
            if d <= 36: close += 1
            diff[o:o+3] = bytes((255, 0, 0)) if d > 36 else bytes((80, 80, 0))

    print("exact match : %d / %d  (%.2f%%)" % (exact, n, 100.0*exact/n))
    print("near match  : %d       (<= one palette step)" % close)
    print("mismatched  : %d       (%.2f%%)" % (n-exact, 100.0*(n-exact)/n))
    print("worst channel delta: %d" % worst)

    if diff_path:
        with open(diff_path, 'wb') as f:
            f.write(b"P6\n%d %d\n255\n" % (aw, ah))
            f.write(bytes(diff))
        print("diff map -> %s  (red = mismatch, olive = near)" % diff_path)
    return 0 if exact == n else 1

sys.exit(main(sys.argv[1:]))
