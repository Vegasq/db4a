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
                if m.split('.')[0] == 'dc' or jumptab.is_impossible(op):
                    # 'dc' is capstone's marker for data it could not decode,
                    # and 68020+ addressing cannot occur on this CPU. Either way
                    # these bytes are not code, so stop following this path --
                    # 'dc' in particular "decodes successfully", so without an
                    # explicit stop it walks straight through a data table.
                    self.bad.append(pc)
                    break
                self.insns[pc] = (ins.size, m, op)

                is_branch = (m in ("bsr","jsr","jmp","bra")
                             or m.startswith("db")
                             or (m.startswith("b") and m not in ("bset","bclr","bchg","btst")))
                if is_branch:
                    # DBcc is `dbra dN, target` -- the destination is the LAST
                    # operand, not the whole string.
                    tgt = resolve(op.rsplit(',', 1)[-1] if m.startswith("db") else op)
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

    # Hand-written routines in this ROM use an A0-return convention:
    #     lea.l $49b1e(pc), a0     ; load the return address
    #     bra.b $49b4c             ; enter the routine
    #     049b1e: ...              ; which returns via jmp (a0)
    # Nothing branches to the return point, so recursive descent never reaches
    # it and execution dies there at runtime. Every lea target is therefore a
    # candidate entry point, vetted with the same validity probe used for
    # guard-less jump tables. Over-discovery is cheap -- a block that is really
    # data is simply never entered -- whereas under-discovery is a crash.
    lea_seeds = 0
    order = sorted(t.insns)
    for idx, a in enumerate(order):
        sz, mn, op = t.insns[a]
        base = mn.split('.')[0]

        # Two shapes of the same idiom: take the address of a return point,
        # then branch into a routine that comes back to it.
        #
        #   lea.l $49b1e(pc), a0     pea.l $49c14(pc)
        #   bra.b $49b4c             ... ; bra into the routine
        #   049b1e: <returns via jmp (a0)>   049c14: <returns via rts>
        #
        # Nothing branches to the return point, so recursive descent never
        # reaches it and execution dies there at runtime.
        if base == 'lea':
            # Require the next instruction to be an unconditional transfer:
            # seeding every lea target drags in data tables.
            nxt = a + sz
            if nxt not in t.insns:
                continue
            if t.insns[nxt][1].split('.')[0] not in ('bra', 'jmp', 'bsr', 'jsr'):
                continue
            m = re.match(r'^\$([0-9a-f]+)\(pc\), a[0-7]$', op.strip())
        elif base == 'pea':
            # pea pushes a return address. The PC-relative form is a code
            # pointer by construction; the frame-relative form (-$12(a6)) is
            # not, and is excluded by the pattern.
            m = re.match(r'^\$([0-9a-f]+)\(pc\)$', op.strip())
        else:
            continue

        if not m:
            continue
        tgt = int(m.group(1), 16)
        if not (0 < tgt < ROM_END) or (tgt & 1) or tgt in t.insns:
            continue
        if jumptab.probe_valid(t.md, d, tgt):
            t.trace(tgt)
            lea_seeds += 1
    if lea_seeds:
        print("lea/pea targets seeded: %d" % lea_seeds)

    # The game's state machine dispatches through a function pointer in RAM
    # ($FFFFE002), and several handlers are installed with a literal address:
    #     move.l #$24724, $e002.w
    # Those immediates are code entry points, but nothing branches to them, so
    # recursive descent alone never sees them -- they surface only when the
    # game reaches that state at runtime and the dispatcher jumps into a block
    # that does not exist. Seeding them statically closes that gap.
    dispatch_seeds = 0
    for a in sorted(t.insns):
        sz, mn, op = t.insns[a]
        if not mn.startswith('move'):
            continue
        parts = [x.strip() for x in op.split(',')]
        if len(parts) != 2 or not parts[1].endswith('.w'):
            continue
        # destination must be the state pointer (or its immediate neighbours,
        # which some handlers write as a pair)
        m = re.match(r'^\$([0-9a-f]+)\.w$', parts[1])
        if not m:
            continue
        dest = int(m.group(1), 16)
        if dest & 0x8000:
            dest = 0xFF0000 | (dest & 0xFFFF)
        if dest != 0xFFE002:
            continue
        mi = re.match(r'^#\$?([0-9a-f]+)$', parts[0])
        if not mi:
            continue
        tgt = int(mi.group(1), 16)
        if 0 < tgt < ROM_END and not (tgt & 1) and tgt not in t.insns:
            t.trace(tgt)
            dispatch_seeds += 1
    if dispatch_seeds:
        print("state-pointer handlers seeded: %d" % dispatch_seeds)

    # Entry points found by actually running the recompiled build. The game
    # dispatches through RAM function pointers, so these are PCs no static
    # pass could predict; feeding them back is how coverage grows past the
    # static plateau.
    # Tracked under data/ because these are discovered, not derived: see the
    # header of that file. build/seeds.txt is the scratch file the running
    # build appends to during a bootstrap run.
    seed_files = [os.path.join(ROOT, "data", "seeds.txt"),
                  os.path.join(ROOT, "build", "seeds.txt")]
    nseeds = 0
    for seeds_path in seed_files:
        if not os.path.exists(seeds_path):
            continue
        for line in open(seeds_path):
            line = line.split('#')[0].strip()
            if not line:
                continue
            v = int(line, 16)
            if v < ROM_END and v not in t.insns:
                t.trace(v)
                nseeds += 1
    if nseeds:
        print("runtime seeds applied: %d" % nseeds)

    tables = {}
    for rnd in range(1, 21):
        order = sorted(t.insns)
        new_sites = [(a, s) for a, s in t.indirect
                     if a not in tables and jumptab.RE_DISPATCH.match(s.split(None,1)[1].strip())]
        added = 0
        for site, s in new_sites:
            r = jumptab.resolve(d, t.insns, order, site, s.split(None,1)[1], md=t.md)
            if not r:
                continue
            tbl, kind, targets, bound, slots = r
            tables[site] = (tbl, kind, targets, bound, slots)
            for tg in slots:
                if tg not in t.insns:
                    added += 1
                t.add(tg, site, True)
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
        badset = set(t.bad)
        blame = {}
        for a,(tbl,kind,tgts,bound,slots) in tables.items():
            for tg in tgts:
                # a target is suspect if it failed to decode OR decoded into
                # something that is not real 68000 code
                if tg in badset or not jumptab.probe_valid(t.md, d, tg):
                    blame.setdefault(a,[]).append(tg)
        if blame:
            print("  tables producing suspect targets:")
            for a,v in sorted(blame.items()):
                tbl,kind,tgts,bound,slots = tables[a]
                print("    site %06X tbl %06X %-6s bound=%s entries=%d -> %d suspect"
                      % (a,tbl,kind,bound,len(tgts),len(v)))
        else:
            print("  no table produced a suspect target")

    json.dump({
        "insns":    {("%06X"%a): list(v) for a, v in sorted(t.insns.items())},
        "starts":   sorted("%06X"%a for a in t.starts),
        "indirect": [["%06X"%a, s] for a, s in sorted(unres)],
        "tables":   {("%06X"%a): {"table":"%06X"%v[0], "kind":v[1], "bound":v[3],
                                  "targets":["%06X"%x for x in v[2]],
                                  "slots":["%06X"%x for x in v[4]]}
                     for a, v in sorted(tables.items())},
        "xrefs":    {("%06X"%a): sorted("%06X"%s for s in v) for a, v in sorted(t.xrefs.items())},
    }, open(OUT,"w"), indent=1)
    print("\nwrote " + OUT)

main(sys.argv[1])
