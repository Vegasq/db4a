#!/usr/bin/env python3
"""Recursive-descent code discovery for a Mega Drive (68000) ROM.

Capstone's m68k detail API reports disp=0 for absolute addressing, so branch
targets are parsed from op_str, which is correct for every addressing mode:
    $17a4.l    absolute long
    $1664.w    absolute short (sign-extended 16-bit)
    $82e(pc)   PC-relative, already resolved to the target by capstone
    $2fa       branch displacement, already resolved
    (a0)       register indirect  -> unresolved, recorded for manual work
"""
import json, os, re, sys, struct
from capstone import *
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jumptab

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT  = os.path.join(ROOT, "build", "codemap.json")

ROM_END  = 0x100000
RAM_BASE = 0xFF0000

RE_ABS_L = re.compile(r'^\$([0-9a-f]+)\.l$')
RE_ABS_W = re.compile(r'^\$([0-9a-f]+)\.w$')
RE_PCREL = re.compile(r'^\$([0-9a-f]+)\(pc\)$')
RE_BRA   = re.compile(r'^\$([0-9a-f]+)$')

TERMINAL = ("rts", "rte", "rtr", "bra", "jmp", "illegal", "stop", "reset")

def resolve(op_str):
    """Absolute ROM target for a direct control transfer, else None."""
    s = op_str.strip()
    m = RE_ABS_L.match(s) or RE_PCREL.match(s) or RE_BRA.match(s)
    if m:
        v = int(m.group(1), 16)
        return v if 0 <= v < ROM_END else None
    m = RE_ABS_W.match(s)
    if m:
        v = int(m.group(1), 16)
        if v & 0x8000:              # sign-extend: this is a RAM address
            return None
        return v if v < ROM_END else None
    return None

class Tracer:
    def __init__(self, data):
        self.d  = data
        self.md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
        self.md.detail = False
        self.insns    = {}   # addr -> (size, mnemonic, op_str)
        self.starts   = set()
        self.xrefs    = {}
        self.indirect = []
        self.bad      = []
        self.pending  = []

    def ref(self, tgt, src):
        self.xrefs.setdefault(tgt, set()).add(src)

    def add(self, tgt, src, is_call):
        if tgt is None or not (0 <= tgt < ROM_END) or tgt & 1:
            return
        self.ref(tgt, src)
        if is_call:
            self.starts.add(tgt)
        if tgt not in self.insns:
            self.pending.append(tgt)

    def trace(self, entry):
        self.starts.add(entry)
        self.pending.append(entry)
        while self.pending:
            pc = self.pending.pop()
            while True:
                if pc in self.insns or not (0 <= pc < ROM_END) or (pc & 1):
                    break
                try:
                    ins = next(self.md.disasm(self.d[pc:pc+16], pc, 1))
                except StopIteration:
                    self.bad.append(pc)
                    break
                m, op = ins.mnemonic, ins.op_str
                self.insns[pc] = (ins.size, m, op)

                is_branch = (m in ("bsr","jsr","jmp","bra")
                             or (m.startswith("b") and m not in ("bset","bclr","bchg","btst")))
                if is_branch:
                    tgt = resolve(op)
                    if tgt is None:
                        if m in ("jmp","jsr"):
                            self.indirect.append((pc, "%s %s" % (m, op)))
                    else:
                        self.add(tgt, pc, m in ("bsr","jsr"))
                if m.split('.')[0] in TERMINAL:
                    break
                pc += ins.size

def main(path):
    d = open(path, "rb").read()
    t = Tracer(d)
    for i in range(1, 64):
        v = struct.unpack(">I", d[i*4:i*4+4])[0]
        if v < ROM_END:
            t.trace(v)

    tables = {}
    for rnd in range(1, 21):
        order = sorted(t.insns)
        new_sites = [(a, s) for a, s in t.indirect
                     if a not in tables and jumptab.RE_DISPATCH.match(s.split(None,1)[1].strip())]
        added = 0
        for site, s in new_sites:
            r = jumptab.resolve(d, t.insns, order, site, s.split(None,1)[1])
            if not r:
                continue
            tbl, kind, targets, bound = r
            tables[site] = (tbl, kind, targets, bound)
            for tg in targets:
                if tg not in t.insns:
                    added += 1
                t.add(tg, site, True)
        before = len(t.insns)
        if t.pending:
            t.trace(t.pending.pop())
        while t.pending:
            t.trace(t.pending.pop())
        grown = len(t.insns) - before
        print("round %2d: +%d tables, +%d instructions" % (rnd, len(new_sites), grown))
        if not new_sites and grown == 0:
            break

    covered = sum(sz for sz, _, _ in t.insns.values())
    unres = [(a, s) for a, s in t.indirect if a not in tables]
    print()
    print("instructions decoded : %d" % len(t.insns))
    print("bytes covered as code: %d (%.1f%% of ROM)" % (covered, 100.0*covered/len(d)))
    print("function entry points: %d" % len(t.starts))
    print("jump tables resolved : %d" % len(tables))
    print("still unresolved     : %d" % len(unres))
    print("failed decodes       : %d" % len(t.bad))
    if t.bad:
        blame = {}
        for a,(tbl,kind,tgts,bound) in tables.items():
            for tg in tgts:
                if tg in t.bad: blame.setdefault(a,[]).append(tg)
        print("  bad decode sites blamed on tables:")
        for a,v in sorted(blame.items()):
            tbl,kind,tgts,bound = tables[a]
            print("    site %06X tbl %06X %-6s bound=%s entries=%d -> %d bad"
                  % (a,tbl,kind,bound,len(tgts),len(v)))

    json.dump({
        "insns":    {("%06X"%a): list(v) for a, v in sorted(t.insns.items())},
        "starts":   sorted("%06X"%a for a in t.starts),
        "indirect": [["%06X"%a, s] for a, s in sorted(unres)],
        "tables":   {("%06X"%a): {"table":"%06X"%v[0], "kind":v[1], "bound":v[3],
                                  "targets":["%06X"%x for x in v[2]]}
                     for a, v in sorted(tables.items())},
        "xrefs":    {("%06X"%a): sorted("%06X"%s for s in v) for a, v in sorted(t.xrefs.items())},
    }, open(OUT,"w"), indent=1)
    print("\nwrote " + OUT)

main(sys.argv[1])
