"""68000 effective-address model: parsing and C code emission.

This is the layer shared by both backends of the semantics generator. It is
deliberately independent of *how* an operand was obtained -- the recompiler
parses capstone's op_str at build time and folds constants, while the
interpreter will decode mode/register bits at runtime, but both end up with
the same Operand objects and use the same emitters.

Only forms that can actually occur on a 68000 are accepted. Anything else
raises, because on this project an unparseable operand means we are decoding
data as code (see jumptab.is_impossible).
"""
import re
from dataclasses import dataclass, field
from typing import List

# ---------------------------------------------------------------- operands

@dataclass(frozen=True)
class DReg:     n: int
@dataclass(frozen=True)
class AReg:     n: int
@dataclass(frozen=True)
class Imm:      v: int
@dataclass(frozen=True)
class Abs:      addr: int; size: str          # 'w' (sign-extended) or 'l'
@dataclass(frozen=True)
class Ind:      an: int                       # (An)
@dataclass(frozen=True)
class PostInc:  an: int                       # (An)+
@dataclass(frozen=True)
class PreDec:   an: int                       # -(An)
@dataclass(frozen=True)
class Disp:     an: int; d: int               # d16(An)
@dataclass(frozen=True)
class Idx:      an: int; xn: int; xa: bool; xl: bool; d: int   # d8(An,Xn.sz)
@dataclass(frozen=True)
class PCDisp:   target: int                   # d16(PC), already resolved
@dataclass(frozen=True)
class PCIdx:    base: int; xn: int; xa: bool; xl: bool
@dataclass(frozen=True)
class Special:  name: str                     # sr / ccr / usp
@dataclass(frozen=True)
class Branch:   target: int
@dataclass(frozen=True)
class RegList:
    regs: tuple                               # ('d0','d1',...,'a6')

# ---------------------------------------------------------------- parsing

def split_operands(s):
    """Split on commas not inside parentheses. capstone uses ', ' inside
    indexed forms too, so a naive split corrupts them."""
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out

_RE = {
 'dreg':    re.compile(r'^d([0-7])$'),
 'areg':    re.compile(r'^a([0-7])$'),
 'imm':     re.compile(r'^#\$?(-?[0-9a-f]+)$'),
 'absw':    re.compile(r'^\$([0-9a-f]+)\.w$'),
 'absl':    re.compile(r'^\$([0-9a-f]+)\.l$'),
 'ind':     re.compile(r'^\(a([0-7])\)$'),
 'post':    re.compile(r'^\(a([0-7])\)\+$'),
 'pre':     re.compile(r'^-\(a([0-7])\)$'),
 'disp':    re.compile(r'^(-?)\$([0-9a-f]+)\(a([0-7])\)$'),
 'idx':     re.compile(r'^(-?)\$([0-9a-f]+)\(a([0-7]), ([ad])([0-7])\.([wl])\)$'),
 'idx0':    re.compile(r'^\(a([0-7]), ([ad])([0-7])\.([wl])\)$'),
 'pcdisp':  re.compile(r'^\$([0-9a-f]+)\(pc\)$'),
 'pcidx':   re.compile(r'^\$([0-9a-f]+)\(pc, ([ad])([0-7])\.([wl])\)$'),
 'branch':  re.compile(r'^\$([0-9a-f]+)$'),
 'special': re.compile(r'^(sr|ccr|usp)$'),
 'reglist': re.compile(r'^[ad][0-7](-[ad][0-7])?(/[ad][0-7](-[ad][0-7])?)*$'),
}

def _expand_reglist(s):
    regs = []
    for part in s.split('/'):
        if '-' in part:
            a, b = part.split('-')
            # ranges never cross the D/A boundary in movem syntax
            if a[0] != b[0]:
                raise ValueError("reglist range crosses register file: %r" % s)
            for i in range(int(a[1]), int(b[1]) + 1):
                regs.append("%s%d" % (a[0], i))
        else:
            regs.append(part)
    return tuple(regs)

def parse_operand(s):
    s = s.strip()
    m = _RE['dreg'].match(s)
    if m: return DReg(int(m.group(1)))
    m = _RE['areg'].match(s)
    if m: return AReg(int(m.group(1)))
    m = _RE['imm'].match(s)
    if m:
        t = m.group(1)
        return Imm(-int(t[1:], 16) if t.startswith('-') else int(t, 16))
    m = _RE['absw'].match(s)
    if m:
        v = int(m.group(1), 16)
        return Abs(v - 0x10000 if v & 0x8000 else v, 'w')
    m = _RE['absl'].match(s)
    if m: return Abs(int(m.group(1), 16), 'l')
    m = _RE['ind'].match(s)
    if m: return Ind(int(m.group(1)))
    m = _RE['post'].match(s)
    if m: return PostInc(int(m.group(1)))
    m = _RE['pre'].match(s)
    if m: return PreDec(int(m.group(1)))
    m = _RE['disp'].match(s)
    if m:
        d = int(m.group(2), 16)
        return Disp(int(m.group(3)), -d if m.group(1) else d)
    m = _RE['idx'].match(s)
    if m:
        d = int(m.group(2), 16)
        return Idx(int(m.group(3)), int(m.group(5)),
                   m.group(4) == 'a', m.group(6) == 'l', -d if m.group(1) else d)
    m = _RE['idx0'].match(s)
    if m:
        return Idx(int(m.group(1)), int(m.group(3)),
                   m.group(2) == 'a', m.group(4) == 'l', 0)
    m = _RE['pcdisp'].match(s)
    if m: return PCDisp(int(m.group(1), 16))
    m = _RE['pcidx'].match(s)
    if m:
        return PCIdx(int(m.group(1), 16), int(m.group(3)),
                     m.group(2) == 'a', m.group(4) == 'l')
    m = _RE['special'].match(s)
    if m: return Special(m.group(1))
    m = _RE['branch'].match(s)
    if m: return Branch(int(m.group(1), 16))
    if _RE['reglist'].match(s) and ('/' in s or '-' in s):
        return RegList(_expand_reglist(s))
    raise ValueError("unparseable operand: %r" % s)

def parse(op_str):
    return [parse_operand(o) for o in split_operands(op_str)] if op_str.strip() else []

# ------------------------------------------------------- C code emission
#
# Emission is split into four steps so that side effects land in the right
# order and each address is computed exactly once:
#
#   setup(op,sz,t)  statements computing the effective address into `t`
#                   (this is where -(An) predecrement happens)
#   load(op,sz,t)   expression reading the operand
#   store(op,sz,t,v) statement writing `v` to the operand
#   post(op,sz)     statements for (An)+ postincrement, emitted last
#
# Register operands ignore `t` entirely.

SZB = {'b': 1, 'w': 2, 'l': 4}
CAST = {'b': 'uint8_t', 'w': 'uint16_t', 'l': 'uint32_t'}
RD   = {'b': 'm68k_read8', 'w': 'm68k_read16', 'l': 'm68k_read32'}
WR   = {'b': 'm68k_write8', 'w': 'm68k_write16', 'l': 'm68k_write32'}

MEM = (Ind, PostInc, PreDec, Disp, Idx, PCDisp, PCIdx, Abs)

def _xindex(xn, xa, xl):
    """Index register contribution: .w is sign-extended, .l is used whole."""
    reg = ("CPU.a[%d]" % xn) if xa else ("CPU.d[%d]" % xn)
    return reg if xl else "sx16((uint16_t)%s)" % reg

def _stack_adjust(an, sz):
    """A7 must stay word-aligned, so byte access to (A7)+/-(A7) moves by 2."""
    return 2 if (an == 7 and sz == 'b') else SZB[sz]

def setup(op, sz, t):
    if isinstance(op, PreDec):
        n = _stack_adjust(op.an, sz)
        return ["CPU.a[%d] -= %d;" % (op.an, n), "uint32_t %s = CPU.a[%d];" % (t, op.an)]
    if isinstance(op, (Ind, PostInc)):
        return ["uint32_t %s = CPU.a[%d];" % (t, op.an)]
    if isinstance(op, Disp):
        return ["uint32_t %s = CPU.a[%d] + %d;" % (t, op.an, op.d)]
    if isinstance(op, Idx):
        return ["uint32_t %s = CPU.a[%d] + %s + %d;"
                % (t, op.an, _xindex(op.xn, op.xa, op.xl), op.d)]
    if isinstance(op, Abs):
        return ["uint32_t %s = 0x%Xu;" % (t, op.addr & 0xFFFFFFFF)]
    if isinstance(op, PCDisp):
        return ["uint32_t %s = 0x%Xu;" % (t, op.target)]
    if isinstance(op, PCIdx):
        return ["uint32_t %s = 0x%Xu + %s;"
                % (t, op.base, _xindex(op.xn, op.xa, op.xl))]
    return []

def load(op, sz, t):
    if isinstance(op, DReg):
        return "(%s)CPU.d[%d]" % (CAST[sz], op.n)
    if isinstance(op, AReg):
        return "(%s)CPU.a[%d]" % (CAST[sz], op.n)
    if isinstance(op, Imm):
        mask = (1 << (SZB[sz] * 8)) - 1
        return "0x%Xu" % (op.v & mask)
    if isinstance(op, MEM):
        return "%s(%s)" % (RD[sz], t)
    if isinstance(op, Special):
        if op.name == 'sr':  return "get_sr()"
        if op.name == 'ccr': return "(uint8_t)(get_sr() & 0xFF)"
        if op.name == 'usp': return "CPU.usp"
    raise TypeError("cannot load from %r" % (op,))

def store(op, sz, t, val):
    if isinstance(op, DReg):
        if sz == 'l':
            return "CPU.d[%d] = %s;" % (op.n, val)
        keep = 0xFFFFFF00 if sz == 'b' else 0xFFFF0000
        mask = 0xFF if sz == 'b' else 0xFFFF
        return "CPU.d[%d] = (CPU.d[%d] & 0x%Xu) | ((%s) & 0x%Xu);" % (
            op.n, op.n, keep, val, mask)
    if isinstance(op, AReg):
        # Address register writes are ALWAYS full 32-bit and sign-extend from
        # word. This is a real 68000 rule, not a simplification: movea.w
        # sign-extends into the whole register.
        if sz == 'w':
            return "CPU.a[%d] = sx16((uint16_t)(%s));" % (op.n, val)
        return "CPU.a[%d] = %s;" % (op.n, val)
    if isinstance(op, MEM):
        return "%s(%s, %s);" % (WR[sz], t, val)
    if isinstance(op, Special):
        if op.name == 'sr':  return "set_sr((uint16_t)(%s));" % val
        if op.name == 'ccr': return "set_ccr((uint8_t)((%s) & 0xFF));" % val
        if op.name == 'usp': return "CPU.usp = %s;" % val
    raise TypeError("cannot store to %r" % (op,))

def post(op, sz):
    if isinstance(op, PostInc):
        return ["CPU.a[%d] += %d;" % (op.an, _stack_adjust(op.an, sz))]
    return []

def is_mem(op):
    return isinstance(op, MEM)


def fix_brief(ops, raw):
    """Correct indexed operands that capstone decoded as 68020 full-format.

    Bit 8 of an extension word selects the full format on the 68020, but on the
    68000 it is reserved and ignored: the word is ALWAYS brief format with a
    signed 8-bit displacement in bits 7-0. Capstone honours bit 8 even in
    M68K_000 mode, swallowing following words and reporting a displacement far
    outside the 8-bit range.

    A real assembler never sets the bit, so no instruction in the Dune ROM is
    affected (all 816 indexed operands there decode correctly). Randomly
    generated test vectors do set it, and so may other games' code.

    Only operands whose extension word really has bit 8 set are touched.
    """
    out = []
    for op in ops:
        # Capstone's displacement is unreliable whenever bit 8 is set, not just
        # when it lands outside the 8-bit range: it often renders 0 for what is
        # really a nonzero brief displacement. Always re-derive from the raw
        # word. _brief_disp only substitutes when it actually finds a matching
        # extension word with bit 8 set, so correct decodes are left alone.
        if isinstance(op, Idx):
            op = Idx(an=op.an, xn=op.xn, xa=op.xa, xl=op.xl,
                     d=_brief_disp(raw, op.xn, op.xa, op.xl, op.d))
        out.append(op)
    return out


def _brief_disp(raw, xn, xa, xl, fallback):
    """Recover the 8-bit displacement from the real extension word."""
    want = (int(bool(xa)) << 15) | ((xn & 7) << 12) | (int(bool(xl)) << 11)
    for j in range(2, len(raw) - 1, 2):
        ext = (raw[j] << 8) | raw[j + 1]
        if ext & 0x0100 and (ext & 0xF800) == want:
            d = ext & 0xFF
            return d - 0x100 if d & 0x80 else d
    return fallback


_SHIFT_SZ = {0: 'b', 1: 'w', 2: 'l'}
_SHIFT_TY = {0: 'as', 1: 'ls', 2: 'rox', 3: 'ro'}


def fix_shift(mn, ops, raw):
    """Decode the register-destination shift/rotate family from raw bytes.

    Capstone mis-decodes the register-count ROXR.b form, reporting it as
    'roxr.l #<n>' -- wrong size AND wrong count source, so it rotates the full
    longword by an immediate instead of the low byte by a register. That is
    roughly half of the ROXR.b vectors.

    The encoding here is entirely regular, so rather than diff against capstone
    and patch up disagreements, decode it outright:

        1110 ccc d ss i tt rrr

    ccc = count (register number when i=1, else immediate with 0 meaning 8),
    d = direction (1 left), ss = size, i = count source, tt = type,
    rrr = destination register. Returns (mnemonic, operands) unchanged for
    anything outside this family, including the ss=3 memory form, which is a
    different encoding entirely.
    """
    if len(raw) < 2:
        return mn, ops
    w = (raw[0] << 8) | raw[1]
    if (w & 0xF000) != 0xE000 or ((w >> 6) & 3) == 3:
        return mn, ops
    ccc = (w >> 9) & 7
    name = "%s%s.%s" % (_SHIFT_TY[(w >> 3) & 3],
                        'l' if (w >> 8) & 1 else 'r',
                        _SHIFT_SZ[(w >> 6) & 3])
    count = DReg(n=ccc) if (w >> 5) & 1 else Imm(v=ccc or 8)
    return name, [count, DReg(n=w & 7)]


def fix_btst_imm(mn, ops, raw):
    """BTST with an immediate operand is byte-sized.

    BTST is the only bit instruction that accepts an immediate as its operand,
    and an immediate operand is a byte: one extension word carrying the data in
    its low half, with the bit number taken modulo 8. Capstone reports the form
    as 'btst.l' and swallows a full longword, so it tests the wrong bit of the
    wrong value.

    Only the destination position is considered. In the static form the FIRST
    operand is also an Imm -- that one is the bit number and must be left alone.
    """
    if mn.split('.')[0] != 'btst' or not ops or not isinstance(ops[-1], Imm):
        return mn, ops
    w = (raw[0] << 8) | raw[1] if len(raw) >= 2 else 0
    if (w & 0x3F) != 0x3C:            # destination is not immediate
        return mn, ops
    off = 3 if (w & 0x0100) else 5    # dynamic: one word in; static: two
    if len(raw) <= off:
        return mn, ops
    return 'btst.b', ops[:-1] + [Imm(v=raw[off])]
