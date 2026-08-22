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


RE_AREG_DISPATCH = re.compile(r'^\((a[0-7]), ([ad][0-7])\.[wl]\)$')
RE_LEA_PC        = re.compile(r'^\$([0-9a-f]+)\(pc\), (a[0-7])$')


def areg_offset_tables(d, insns, md):
    """Recover handlers from `jsr/jmp (aN, dM.w)` where aN came from a lea.

    A script interpreter in this ROM dispatches like this:

        lea.l  $4192e(pc), a0      ; table base
        move.w (a0, d0.w), d0      ; d0 = a 16-bit offset out of the table
        jsr    (a0, d0.w)          ; call base + offset

    The transfer goes through an address register, so it does not match the
    `jmp tbl(pc,dN.w)` form jumptab.py handles, and recursive descent reaches
    none of the handlers. That is what left PC 041970 undiscovered.

    Entry count comes from the table itself and needs no range guard: the
    entries are offsets from the base, so the table cannot extend past its own
    first handler. Reading until the cursor reaches base + (smallest positive
    offset) stops exactly at the end -- in the table above entry 1 is 0x0036,
    and 27 entries is 0x36 bytes.

    Returns {target: table base}.
    """
    found = {}
    order = sorted(insns)
    for i, a in enumerate(order):
        _, mn, op = insns[a]
        if mn.split('.')[0] not in ('jsr', 'jmp'):
            continue
        m = RE_AREG_DISPATCH.match(op.strip())
        if not m:
            continue
        areg = m.group(1)
        base = None
        for j in range(i - 1, max(-1, i - 14), -1):
            _, pmn, pop = insns[order[j]]
            if pmn.split('.')[0] != 'lea':
                continue
            lm = RE_LEA_PC.match(pop.strip())
            if lm and lm.group(2) == areg:
                base = int(lm.group(1), 16)
                break
        if base is None or base + 2 > len(d):
            continue
        limit = None
        for k in range(0, 512):
            ea_ = base + k * 2
            if (limit is not None and ea_ >= limit) or ea_ + 2 > len(d):
                break
            off = struct.unpack_from('>H', d, ea_)[0]
            if off == 0:                      # opcode 0 terminates the stream
                continue
            if off & 1:
                break
            tgt = base + off
            if tgt >= len(d):
                break
            limit = tgt if limit is None else min(limit, tgt)
            found.setdefault(tgt, base)
    return found



RE_IMM_LONG = re.compile(r'^#\$([0-9a-f]+),')


def immediate_code_pointers(d, insns, md):
    """Seed handlers whose address is loaded as a 32-bit immediate.

    The main state machine dispatches through a function pointer at $FFFFE002,
    and an existing pass seeds immediates written STRAIGHT to that address. But
    a handler address is just as often passed as an argument and stored by the
    callee:

        026A28  move.l #$b540, (a7)     ; 00B540 is a VBlank state handler
        026A2E  jsr    $4792.l          ; which installs it

    Nothing branches to 00B540, no table contains it, and the only reference in
    the whole ROM is that immediate -- so recursive descent never reaches it and
    the game dies in the VBlank dispatch the moment that state is entered.

    Any long immediate that lands on a plausible instruction run is therefore a
    candidate entry point, vetted the same way guard-less jump table entries
    are. Targets that fall INSIDE an already-decoded instruction are rejected:
    those are not entry points but misaligned readings of bytes already spoken
    for, and recompiling from one corrupts the real block.
    """
    n = len(d)

    def plausible(a, depth=8):
        if a < 0x200 or a >= n or a & 1:
            return False
        off = a
        for _ in range(depth):
            ins = next(md.disasm(d[off:off + 12], off, 1), None)
            if ins is None:
                return False
            base = ins.mnemonic.split('.')[0]
            if base == 'dc':
                return False
            if base in ('rts', 'rte', 'rtr', 'jmp', 'bra', 'bsr', 'jsr'):
                return True
            off += ins.size
        return True

    interior = set()
    for a, rec in insns.items():
        for off in range(2, rec[0], 2):
            interior.add(a + off)

    found = {}
    for a, (_sz, mn, op) in insns.items():
        if mn not in ('move.l', 'movea.l'):
            continue
        m = RE_IMM_LONG.match(op.strip())
        if not m:
            continue
        v = int(m.group(1), 16)
        if v in insns or v in interior:
            continue
        if plausible(v):
            found.setdefault(v, a)
    return found


_PTR_RUNS = {}


def pointer_tables(d, insns, md, min_entries=4):
    """Recover entry points from tables of 32-bit ROM pointers.

    Genesis code routinely dispatches through a table of absolute addresses
    that nothing branches to directly, so recursive descent never sees the
    targets and execution dies there at runtime.

    A run of >=4 consecutive aligned ROM-range values that each decode as a
    plausible instruction run is the signal. On its own that still matches
    plenty of data -- 16 of the candidate tables in this ROM contain no known
    code at all. So a table only counts when at least one of its entries is
    ALREADY discovered code: that anchor is what separates a real dispatch
    table from four integers that happen to look like addresses.

    Returns {target: table offset} for the undiscovered targets of anchored
    tables, so each seed carries a real xref back to the table that names it.

    The scan itself is expensive -- a decode probe at every aligned ROM offset
    -- so it is cached: only the anchoring check re-runs as discovery grows.
    """
    n = len(d)

    def plausible(a, depth=8):
        if a < 0x200 or a >= n or a & 1:
            return False
        off = a
        for _ in range(depth):
            ins = next(md.disasm(d[off:off + 12], off, 1), None)
            if ins is None:
                return False
            base = ins.mnemonic.split('.')[0]
            if base == 'dc':
                return False
            if base in ('rts', 'rte', 'rtr', 'jmp', 'bra', 'bsr', 'jsr'):
                return True
            off += ins.size
        return True

    runs = _PTR_RUNS.get(min_entries)
    if runs is None:
        ptrs = {}
        for off in range(0x200, n - 4, 2):
            v = struct.unpack_from('>I', d, off)[0]
            if 0x200 <= v < n and not (v & 1) and plausible(v):
                ptrs[off] = v
        runs = []
        srcs = sorted(ptrs)
        i = 0
        while i < len(srcs):
            j = i
            while j + 1 < len(srcs) and srcs[j + 1] - srcs[j] == 4:
                j += 1
            if j - i + 1 >= min_entries:
                runs.append((srcs[i], [ptrs[o] for o in srcs[i:j + 1]]))
            i = j + 1
        _PTR_RUNS[min_entries] = runs

    # A seed that lands strictly INSIDE an already-decoded instruction is not a
    # new entry point, it is a misaligned reading of bytes that are already
    # spoken for. Recompiling from there splits the real block at a boundary
    # that is not an instruction boundary and corrupts it -- which is how an
    # earlier version of this pass broke a working playthrough by seeding
    # 000400 in the middle of the 4-byte instruction at 0003FE.
    interior = set()
    for a, rec in insns.items():
        for off in range(2, rec[0], 2):
            interior.add(a + off)

    found = {}
    for base, tg in runs:
        if any(t in insns for t in tg):              # anchored in known code
            for t in tg:
                if t not in insns and t not in interior:
                    found.setdefault(t, base)
    return found


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
        for tg, src in immediate_code_pointers(d, t.insns, t.md).items():
            if tg not in t.insns:
                added += 1
                t.add(tg, src, True)
        for tg, tbl in areg_offset_tables(d, t.insns, t.md).items():
            if tg not in t.insns:
                added += 1
                t.add(tg, tbl, True)
        for tg, tbl in ({} if os.environ.get('NO_PTR') else pointer_tables(d, t.insns, t.md)).items():
            if tg not in t.insns:
                added += 1
                t.add(tg, tbl, True)
        before = len(t.insns)
        if t.pending:
            t.trace(t.pending.pop())
        while t.pending:
            t.trace(t.pending.pop())
        grown = len(t.insns) - before
        print("round %2d: +%d tables, +%d seeds, +%d instructions"
              % (rnd, len(new_sites), added, grown))
        if not new_sites and grown == 0:
            break

    # Overlapping decodes: an instruction address that lands strictly inside
    # another means two instruction streams claim the same bytes. A few are
    # expected (data reached by more than one path), but a jump in this number
    # means a seeding pass is inventing misaligned entry points -- recompiling
    # from one splits a real block at a non-instruction boundary and corrupts
    # it. This is reported because a pointer-table pass once pushed it from 25
    # to 58 and broke a working playthrough with no other visible signal.
    _addrs = sorted(t.insns)
    _ov = 0
    for _i, _a in enumerate(_addrs):
        _end = _a + t.insns[_a][0]
        _j = _i + 1
        while _j < len(_addrs) and _addrs[_j] < _end:
            _ov += 1
            _j += 1
    print("overlapping decodes  : %d" % _ov)

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
