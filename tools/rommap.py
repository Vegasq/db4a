#!/usr/bin/env python3
"""Coarse structure map of a Mega Drive ROM: per-block entropy + byte stats.

Classifies 1 KiB blocks as:
  ZERO  - padding / unused
  CODE  - low-ish entropy, high density of 68k-plausible opcode words
  DATA  - structured data (tables, tilemaps, uncompressed gfx)
  COMP  - high entropy: compressed data, packed graphics, or samples
"""
import sys, math, struct
from collections import Counter

BLK = 1024

def entropy(b):
    if not b: return 0.0
    c = Counter(b); n = len(b)
    return -sum((v/n)*math.log2(v/n) for v in c.values())

# Opcode-word high nibbles that dominate real 68k code
def code_score(b):
    hits = 0; total = 0
    for i in range(0, len(b)-1, 2):
        w = (b[i] << 8) | b[i+1]
        total += 1
        hi = w >> 12
        # common: move(1,2,3) bcc(6) moveq(7) sub(9) cmp(B) and(C) add(D) misc(4) addq/subq(5)
        if hi in (0x1,0x2,0x3,0x4,0x5,0x6,0x7,0x9,0xB,0xC,0xD,0x0,0x8,0xE):
            hits += 1
    return hits/total if total else 0

def classify(b):
    if not any(b): return "ZERO"
    if all(x == b[0] for x in b): return "FILL"
    e = entropy(b)
    if e > 7.3: return "COMP"
    if e < 4.2: return "DATA"
    return "CODE" if code_score(b) > 0.90 else "DATA"

def main(path):
    d = open(path,"rb").read()
    rows = []
    for off in range(0, len(d), BLK):
        b = d[off:off+BLK]
        rows.append((off, classify(b), entropy(b)))
    # coalesce runs
    print("%-20s %-6s %-9s %s" % ("RANGE","KIND","SIZE","avg entropy"))
    print("-"*58)
    i = 0
    while i < len(rows):
        j = i
        while j+1 < len(rows) and rows[j+1][1] == rows[i][1]:
            j += 1
        start = rows[i][0]; end = rows[j][0] + BLK
        avg = sum(r[2] for r in rows[i:j+1]) / (j-i+1)
        print("%06X-%06X  %-6s %-9s %.2f" % (start, end-1, rows[i][1],
              "%d KiB" % ((end-start)//1024), avg))
        i = j+1
    tally = Counter(r[1] for r in rows)
    print("\nTotals (KiB):", dict(tally))

main(sys.argv[1])
