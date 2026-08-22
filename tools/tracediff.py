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

    # Collapse spin loops before comparing.
    #
    # A wait loop runs a different number of times on the two sides for reasons
    # that are not bugs -- a status flag observed a few cycles earlier or later.
    # Because the comparison is positional, one extra iteration shifts every
    # record after it and turns a benign timing difference into thousands of
    # reported divergences, drowning any real one.
    #
    # Collapsing consecutive records that share a PC to a single entry removes
    # that entirely: a loop spun 40 times and one spun 41 times both become one
    # record, and the sequences stay aligned. The iteration counts are kept so a
    # genuinely stuck loop -- one side spinning while the other moves on -- can
    # still be reported, since that IS a bug.
    #
    # TRACEDIFF_RAW=1 compares without collapsing.
    def collapse(seq):
        out = []
        for pc, h in seq:
            if out and out[-1][0] == pc:
                out[-1][2] += 1
                out[-1][1] = h          # keep the state on leaving the loop
            else:
                out.append([pc, h, 1])
        return out

    raw = os.environ.get('TRACEDIFF_RAW')
    mine_seq = [(mine[i * 2], mine[i * 2 + 1]) for i in range(mn)]
    if not raw:
        cm_ = collapse(mine_seq)
        cf_ = collapse(filt)
        print("after collapsing spin loops: db4a %d -> %d, reference %d -> %d"
              % (mn, len(cm_), len(filt), len(cf_)))
        mine_seq, filt = [(a, b) for a, b, _ in cm_], [(a, b) for a, b, _ in cf_]
        mine_reps = [c for _, _, c in cm_]
        ref_reps  = [c for _, _, c in cf_]
    else:
        mine_reps = ref_reps = None

    mine = [v for pair in mine_seq for v in pair]
    mn = len(mine_seq)
    lim = min(mn, len(filt))
    limit_reports = int(os.environ.get('TRACEDIFF_N', '10'))
    stuck = []

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

    # A loop one side spins far longer in is a real difference, not the timing
    # noise the collapse is there to hide. Report the worst offenders.
    if mine_reps and ref_reps:
        for i in range(lim):
            a, b = mine_reps[i], ref_reps[i]
            if max(a, b) >= 50 and max(a, b) > 4 * max(1, min(a, b)):
                stuck.append((mine_seq[i][0], a, b))
        if stuck:
            print("\nloops with very different iteration counts (a real difference):")
            for pc, a, b in stuck[:10]:
                print("   %06X : db4a %d, reference %d" % (pc, a, b))

    if first is None:
        print("\nno divergence in the first %d compared records" % lim)
        return 1 if stuck else 0

    total = sum(pc_counts.values())
    print("\n%d divergent records of %d compared (%.3f%%)" % (total, lim, 100.0 * total / lim))
    print("divergences by PC (top 10):")
    for pc, n in sorted(pc_counts.items(), key=lambda kv: -kv[1])[:10]:
        print("   %06X : %d" % (pc, n))
    print("\nfirst divergence at record %d" % first)
    return 1

sys.exit(main(*sys.argv[1:]))
