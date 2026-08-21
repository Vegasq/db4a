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

def resolve(d, insns, order, site, op_str):
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
        targets.append(t)
    return (tbl, kind, targets, bound)
