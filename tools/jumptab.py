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
    return (tbl, kind, targets, bound, slots)
