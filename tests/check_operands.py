#!/usr/bin/env python3
"""Assert every operand in the discovered corpus parses.

This is a strong invariant, not a nicety. An unparseable operand means either
the EA model has a gap, or -- as happened with the 0x1D000-0x1F000 text region
-- the tracer is decoding data as code. Either way it must fail loudly.
"""
import collections, json, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import ea, jumptab

m = json.load(open(os.path.join(ROOT, 'build', 'codemap.json')))
insns = m['insns']

ok = 0
fail = collections.Counter()
kinds = collections.Counter()
impossible = []

for k, (sz, mn, op) in insns.items():
    if jumptab.is_impossible(op):
        impossible.append((k, mn, op))
    try:
        for o in ea.parse(op):
            kinds[type(o).__name__] += 1
        ok += 1
    except Exception as e:
        fail["%s  (e.g. %s %s)" % (str(e)[:60], mn, op)] += 1

print("instructions parsed : %d / %d" % (ok, len(insns)))
print("operand kinds       : %d distinct" % len(kinds))
for k, v in kinds.most_common():
    print("    %-9s %6d" % (k, v))

bad = sum(fail.values())
if impossible:
    print("\nFAIL: %d instructions use forms impossible on a 68000:" % len(impossible))
    for k, mn, op in impossible[:5]:
        print("    %s  %s %s" % (k, mn, op))
if bad:
    print("\nFAIL: %d instructions had unparseable operands:" % bad)
    for k, v in fail.most_common(10):
        print("    %4d  %s" % (v, k))

if bad or impossible:
    sys.exit(1)
print("\nall %d instructions parse cleanly" % ok)
