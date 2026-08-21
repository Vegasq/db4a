#!/usr/bin/env python3
"""Recursive-descent opcode survey of a captured Z80 program.

Same approach that made the 68000 work tractable: find out which instructions
are actually reached before implementing any of them. Reports the opcode
histogram and which prefixes matter.
"""
import sys
from collections import Counter

# Unprefixed instruction lengths, 16 rows of 16.
L = (
    [1,3,1,1,1,1,2,1, 1,1,1,1,1,1,2,1] +   # 0x
    [2,3,1,1,1,1,2,1, 2,1,1,1,1,1,2,1] +   # 1x
    [2,3,3,1,1,1,2,1, 2,1,3,1,1,1,2,1] +   # 2x
    [2,3,3,1,1,1,2,1, 2,1,3,1,1,1,2,1] +   # 3x
    [1]*16 + [1]*16 + [1]*16 + [1]*16 +    # 4x-7x  LD r,r' / HALT
    [1]*16 + [1]*16 + [1]*16 + [1]*16 +    # 8x-Bx  ALU
    [1,1,3,3,3,1,2,1, 1,1,3,0,3,3,2,1] +   # Cx  (CB at 0xCB)
    [1,1,3,2,3,1,2,1, 1,1,3,2,3,0,2,1] +   # Dx  (DD at 0xDD)
    [1,1,3,1,3,1,2,1, 1,1,3,1,3,0,2,1] +   # Ex  (ED at 0xED)
    [1,1,3,1,3,1,2,1, 1,1,3,1,3,0,2,1]     # Fx  (FD at 0xFD)
)
ED_LONG = {0x43,0x4B,0x53,0x5B,0x63,0x6B,0x73,0x7B}   # LD (nn),dd / LD dd,(nn)
# DD/FD forms that carry a displacement byte
IDX_D = set(range(0x34,0x37)) | {0x46,0x4E,0x56,0x5E,0x66,0x6E,0x70,0x71,0x72,
            0x73,0x74,0x75,0x77,0x7E,0x86,0x8E,0x96,0x9E,0xA6,0xAE,0xB6,0xBE}

def decode(m, pc):
    """Return (length, kind, target) for the instruction at pc."""
    op = m[pc]
    if op == 0xCB:
        return 2, 'cb', None
    if op == 0xED:
        op2 = m[(pc+1) & 0x1FFF]
        n = 4 if op2 in ED_LONG else 2
        kind = 'ed_ret' if op2 in (0x45,0x4D) else 'ed'
        return n, kind, None
    if op in (0xDD, 0xFD):
        op2 = m[(pc+1) & 0x1FFF]
        if op2 == 0xCB:
            return 4, 'idxcb', None
        n = 1 + L[op2] + (1 if op2 in IDX_D else 0)
        return n, 'idx', None
    n = L[op]
    lo = m[(pc+1) & 0x1FFF]; hi = m[(pc+2) & 0x1FFF]
    nn = lo | (hi << 8)
    if op == 0xC3:                      return n, 'jp',   nn      # JP nn
    if op in (0xC2,0xCA,0xD2,0xDA,0xE2,0xEA,0xF2,0xFA): return n,'jpc',nn
    if op == 0xCD:                      return n, 'call', nn
    if op in (0xC4,0xCC,0xD4,0xDC,0xE4,0xEC,0xF4,0xFC): return n,'callc',nn
    if op == 0x18:                      return n, 'jr',   (pc+2+((lo^0x80)-0x80)) & 0x1FFF
    if op in (0x20,0x28,0x30,0x38):     return n, 'jrc',  (pc+2+((lo^0x80)-0x80)) & 0x1FFF
    if op in (0x10,):                   return n, 'djnz', (pc+2+((lo^0x80)-0x80)) & 0x1FFF
    if op == 0xC9:                      return n, 'ret',  None
    if op in (0xC0,0xC8,0xD0,0xD8,0xE0,0xE8,0xF0,0xF8): return n,'retc',None
    if (op & 0xC7) == 0xC7:             return n, 'rst',  op & 0x38
    if op == 0x76:                      return n, 'halt', None
    return n, 'op', None

def main(path):
    m = bytearray(open(path,'rb').read())
    if len(m) < 0x2000: m += bytes(0x2000 - len(m))
    seen = {}
    hist = Counter(); prefixes = Counter()
    stack = [0x0000]
    while stack:
        pc = stack.pop() & 0x1FFF
        while True:
            if pc in seen: break
            n, kind, tgt = decode(m, pc)
            seen[pc] = n
            op = m[pc]
            hist[op] += 1
            if op in (0xCB,0xDD,0xED,0xFD): prefixes[hex(op)] += 1
            if kind in ('call','callc','jpc','jrc','djnz'):
                if tgt is not None: stack.append(tgt)
            if kind == 'rst': stack.append(tgt)
            if kind in ('jp','jr'):
                if tgt is not None: stack.append(tgt)
                break
            if kind in ('ret','halt','ed_ret'): break
            pc = (pc + n) & 0x1FFF

    covered = sum(seen.values())
    print("reachable instructions : %d" % len(seen))
    print("bytes covered          : %d of 8192 (%.1f%%)" % (covered, 100.0*covered/8192))
    print("distinct opcodes used  : %d of 256" % len(hist))
    print("prefixes used          : %s" % (dict(prefixes) or "none"))
    print("\ntop opcodes:")
    tot = sum(hist.values()); c = 0
    for op, n in hist.most_common(24):
        c += n
        print("   %02X  %5d   (cum %.1f%%)" % (op, n, 100.0*c/tot))

main(sys.argv[1])
