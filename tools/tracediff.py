#!/usr/bin/env python3
"""Find the first execution divergence between db4a and the reference core.

Both sides emit 8-byte records of (PC, FNV-1a hash of D0-D7/A0-A7). The
reference logs every instruction; db4a logs block entries. So the reference
trace is filtered down to PCs that db4a treats as block starts, and the two
sequences are compared in order.

The output is an exact PC and the register state that first disagrees -- not a
hypothesis.

usage: tracediff.py <db4a.trace> <ref.trace> [codemap.json]
"""
import json, os, struct, sys

def records(path):
    with open(path, 'rb') as f:
        buf = f.read()
    n = len(buf) // 8
    return struct.unpack('<%dI' % (n * 2), buf[:n * 8]), n

def main(mine_path, ref_path, codemap='build/codemap.json'):
    mine, mn = records(mine_path)
    ref,  rn = records(ref_path)
    print("db4a      : %d block records" % mn)
    print("reference : %d instruction records" % rn)

    starts = set()
    if os.path.exists(codemap):
        cm = json.load(open(codemap))
        starts = {int(k, 16) for k in cm['insns']}   # any decoded instruction

    # Keep only reference records whose PC db4a also treats as a block entry.
    mine_pcs = {mine[i * 2] for i in range(mn)}
    filt = [(ref[i * 2], ref[i * 2 + 1]) for i in range(rn) if ref[i * 2] in mine_pcs]
    print("reference filtered to db4a block PCs: %d" % len(filt))

    lim = min(mn, len(filt))
    limit_reports = int(os.environ.get('TRACEDIFF_N', '10'))

    # Report several divergences, not just the first. A single benign
    # difference -- a scratch register left holding an open-bus value, say --
    # would otherwise mask everything after it, and the useful question is
    # whether divergences share one cause or accumulate.
    shown = 0
    pc_counts = {}
    first = None
    for i in range(lim):
        mpc, mh = mine[i * 2], mine[i * 2 + 1]
        rpc, rh = filt[i]
        if mpc == rpc and mh == rh:
            continue
        if first is None:
            first = i
        pc_counts[mpc] = pc_counts.get(mpc, 0) + 1
        if shown < limit_reports:
            kind = "PC" if mpc != rpc else "registers"
            print("\n#%d record %d: %s differ" % (shown + 1, i, kind))
            print("   db4a      PC=%06X reghash=%08X" % (mpc, mh))
            print("   reference PC=%06X reghash=%08X" % (rpc, rh))
            shown += 1

    if first is None:
        print("\nno divergence in the first %d compared records" % lim)
        return 0

    total = sum(pc_counts.values())
    print("\n%d divergent records of %d compared (%.3f%%)" % (total, lim, 100.0 * total / lim))
    print("divergences by PC (top 10):")
    for pc, n in sorted(pc_counts.items(), key=lambda kv: -kv[1])[:10]:
        print("   %06X : %d" % (pc, n))
    print("\nfirst divergence at record %d" % first)
    return 1

sys.exit(main(*sys.argv[1:]))
