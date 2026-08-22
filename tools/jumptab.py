"""Static recovery of 68k jump tables behind `jmp table(pc,dN.w)` dispatches.

Three table formats appear in this ROM:

  BRA_B   table of `bra.b`  entries, 2 bytes each   (word & 0xFF00)==0x6000, low byte != 0
  BRA_W   table of `bra.w`  entries, 4 bytes each   word == 0x6000
  OFFSET  table of int16 self-relative offsets from the table base, 2 bytes each,
          used with the `move.w tbl(pc,dN.l),dN ; jmp tbl(pc,dN.w)` idiom

The entry count comes from the `cmpi #N,dN / bgt` range guard that precedes the
dispatch; without one we stop at the first implausible entry.
"""
import re, struct

ROM_END = 0x100000
MAX_ENTRIES = 512

# Addressing forms capstone emits that CANNOT exist on a 68000. Their presence
# proves the bytes being decoded are data, not code:
#   ([$N, Rn])        memory indirect        68020+
#   ([$N, Rn], $N)    memory indirect + od   68020+
#   $N(Rn, Rn.l * 8)  scale factor           68020+
#   $N(Rn, invalid.w) capstone gave up
RE_IMPOSSIBLE = re.compile(r'\(\[|\* [248]\b|invalid')

def is_impossible(op_str):
    """True if this operand form cannot occur in real 68000 code."""
    return bool(RE_IMPOSSIBLE.search(op_str))

RE_DISPATCH = re.compile(r'^\$([0-9a-f]+)\(pc, ([ad][0-7])\.[wl]\)$')
RE_CMPI     = re.compile(r'^#\$([0-9a-f]+), ([ad][0-7])$')

def s16(v):
    return v - 0x10000 if v & 0x8000 else v

def table_format(d, tbl):
    w = struct.unpack(">H", d[tbl:tbl+2])[0]
    if (w & 0xFF00) == 0x6000:
        return ("BRA_B", 2) if (w & 0xFF) not in (0x00, 0xFF) else ("BRA_W", 4)
    return ("OFFSET", 2)

def entry_target(d, tbl, kind, i, stride):
    p = tbl + i*stride
    if p + stride > ROM_END:
        return None
    if kind == "BRA_B":
        w = struct.unpack(">H", d[p:p+2])[0]
        if (w & 0xFF00) != 0x6000: return None
        disp = w & 0xFF
        if disp & 0x80: disp -= 0x100
        return p + 2 + disp
    if kind == "BRA_W":
        w, disp = struct.unpack(">HH", d[p:p+4])
        if w != 0x6000: return None
        return p + 2 + s16(disp)
    off = struct.unpack(">H", d[p:p+2])[0]
    return tbl + s16(off)

def find_bound(insns, order, site):
    """Largest index allowed by a `cmpi #N,dN` guard shortly before `site`."""
    import bisect
    i = bisect.bisect_left(order, site)
    for a in reversed(order[max(0, i-12):i]):
        sz, mn, op = insns[a]
        if mn.startswith("cmpi"):
            m = RE_CMPI.match(op)
            if m:
                return int(m.group(1), 16)
    return None

def probe_valid(md, d, addr, depth=8):
    """Decode a few instructions at addr; reject if any form is impossible on
    a 68000, or if decoding fails immediately. Used to vet jump-table targets
    before accepting them as code."""
    if not (0 <= addr < ROM_END) or (addr & 1):
        return False
    pc = addr
    for _ in range(depth):
        try:
            ins = next(md.disasm(d[pc:pc+16], pc, 1))
        except StopIteration:
            return pc != addr          # ok if we decoded at least one
        if ins.mnemonic.split('.')[0] == 'dc' or is_impossible(ins.op_str):
            return False
        m = ins.mnemonic.split('.')[0]
        if m in ("rts", "rte", "rtr", "bra", "jmp", "illegal"):
            return True
        pc += ins.size
    return True

def locality_ok(targets, cand, slack=0x2000):
    """Guard-less tables have no explicit entry count, so reject an entry that
    sits far outside the span of the entries accepted so far. Real dispatch
    arms live close together; a sudden jump of many KiB means we have read
    past the end of the table into whatever follows it."""
    if len(targets) < 3:
        return True
    lo, hi = min(targets), max(targets)
    return (lo - slack) <= cand <= (hi + slack)

RE_ANDI = re.compile(r'^#\$([0-9a-f]+), ([ad][0-7])$')

def enumerate_offsets(insns, order, site, reg):
    """Work out exactly which offsets a `jmp tbl(pc,dN.w)` can produce.

    Some dispatches bound the index with a mask rather than a compare, and
    scale it with a rotate rather than a shift:

        andi.w #$c000, d0      2 bits set -> 4 possible values
        rol.w  #$3, d0         -> offsets 0, 2, 4, 6
        jmp    $2e30e(pc, d0.w)

    Guessing a stride from the first word at the table base fails here: the
    first arm is `rts`, so the table is not recognisable as a branch table at
    all. Instead enumerate every value the mask allows and push it through the
    scaling, which yields the reachable offsets directly and needs no guess.

    Returns a sorted list of offsets, or None if the pattern is not present.
    """
    import bisect
    i = bisect.bisect_left(order, site)
    mask = None
    ops = []
    for a in reversed(order[max(0, i - 10):i]):
        sz, mn, op = insns[a]
        base = mn.split('.')[0]
        parts = [x.strip() for x in op.split(',')]
        if base == 'andi' and len(parts) == 2 and parts[1] == reg:
            m = RE_ANDI.match(op.strip())
            if m:
                mask = int(m.group(1), 16)
                break
        if base in ('rol', 'ror', 'lsl', 'lsr', 'asl', 'asr') and parts[-1] == reg:
            m = re.match(r'^#\$?([0-9a-f]+)$', parts[0])
            if m:
                ops.append((base, int(m.group(1), 16)))
        elif base == 'add' and len(parts) == 2 and parts[0] == reg and parts[1] == reg:
            ops.append(('lsl', 1))
    if mask is None or not (0 < mask <= 0xFFFF):
        return None

    bits = [b for b in range(16) if mask & (1 << b)]
    if len(bits) > 8:                      # too many combinations to be a table
        return None

    offsets = set()
    for combo in range(1 << len(bits)):
        v = 0
        for k, b in enumerate(bits):
            if combo & (1 << k):
                v |= 1 << b
        for base, cnt in reversed(ops):    # applied in program order
            cnt &= 63
            if base == 'lsl':   v = (v << cnt) & 0xFFFF
            elif base == 'lsr': v = (v & 0xFFFF) >> cnt
            elif base == 'rol': v = ((v << cnt) | (v >> (16 - cnt))) & 0xFFFF if cnt % 16 else v
            elif base == 'ror': v = ((v >> cnt) | (v << (16 - cnt))) & 0xFFFF if cnt % 16 else v
            elif base == 'asl': v = (v << cnt) & 0xFFFF
            elif base == 'asr': v = v >> cnt
        offsets.add(v)
    return sorted(offsets)

def resolve(d, insns, order, site, op_str, md=None):
    """Return (table_addr, kind, [targets]) for one dispatch site, or None."""
    m = RE_DISPATCH.match(op_str.strip())
    if not m:
        return None
    tbl = int(m.group(1), 16)
    if not (0 <= tbl < ROM_END):
        return None
    kind, stride = table_format(d, tbl)
    bound = find_bound(insns, order, site)
    limit = (bound + 1) if bound is not None else MAX_ENTRIES

    targets = []
    for i in range(min(limit, MAX_ENTRIES)):
        t = entry_target(d, tbl, kind, i, stride)
        if t is None or not (0 <= t < ROM_END) or (t & 1):
            break
        # a target inside the table itself means we ran past the end
        if bound is None and tbl <= t < tbl + (i+1)*stride:
            break
        # Without a cmpi guard we have no entry count, so vet each entry.
        if bound is None:
            if not locality_ok(targets, t):
                break
            if md is not None and not probe_valid(md, d, t):
                break
        targets.append(t)

    # For a table of branch instructions the CPU jumps INTO the table: the
    # dispatch lands on the slot, which then branches onward. So each slot
    # address is itself an entry point, not just the address it branches to.
    # Seeding only the branch targets leaves the slots undecoded, and
    # execution dies on the second and later arms -- the first one survives
    # only because it happens to be reached by fallthrough.
    slots = []
    if kind in ("BRA_B", "BRA_W"):
        slots = [tbl + i * stride for i in range(len(targets))]
    else:
        # Not a recognisable branch table. If the index is mask-bounded, the
        # reachable offsets can be enumerated exactly, and each is a slot the
        # dispatch jumps directly into.
        m = RE_DISPATCH.match(op_str.strip())
        if m:
            offs = enumerate_offsets(insns, order, site, m.group(2))
            if offs is not None and len(offs) <= 64:
                cand = [tbl + o for o in offs]
                if md is not None:
                    cand = [c for c in cand if probe_valid(md, d, c)]
                slots = cand
    return (tbl, kind, targets, bound, slots)
