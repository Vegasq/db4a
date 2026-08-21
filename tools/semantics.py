"""Single source of truth for 68000 instruction semantics.

Both backends are emitted from the definitions here:
  - the recompiler, which turns discovered basic blocks into C
  - the fallback interpreter, for PCs no static pass predicted

Never fix a semantics bug in generated output. Fix it here and regenerate.

Flag behaviour lives in include/m68k.h (add8/sub16/flags_logic32/...), which is
unit-tested by tests/test_flags.c. This module decides *which* helper applies to
each instruction; the helper decides what the flags become.

An unimplemented form raises Unsupported rather than emitting something
plausible. A loud gap is recoverable; silently wrong arithmetic is not.
"""
import ea

class Unsupported(Exception):
    pass

SIZE_OF = {'b': 'b', 'w': 'w', 'l': 'l'}

def split_mnemonic(mn):
    """'move.l' -> ('move','l').  Size defaults to word where unsuffixed."""
    if '.' in mn:
        base, _, sz = mn.partition('.')
        return base, (sz if sz in SIZE_OF else 'w')
    return mn, None

# --------------------------------------------------------------- emit ctx

class Ctx:
    """Per-instruction emission context."""
    def __init__(self, addr, size, next_addr):
        self.addr = addr
        self.next = next_addr
        self.n = 0
    def tmp(self, stem="t"):
        self.n += 1
        return "%s%d" % (stem, self.n)

# ------------------------------------------------------------- primitives

def _read(op, sz, ctx, out):
    """Emit setup for `op`, return a C expression for its value."""
    if isinstance(op, (ea.DReg, ea.AReg, ea.Imm, ea.Special)):
        return ea.load(op, sz, None)
    t = ctx.tmp("a")
    out += ea.setup(op, sz, t)
    v = ctx.tmp("v")
    out.append("%s %s = %s;" % (ea.CAST[sz], v, ea.load(op, sz, t)))
    out += ea.post(op, sz)
    return v

def _addr(op, sz, ctx, out):
    """Emit setup for `op` and return its effective address (no load)."""
    if not ea.is_mem(op):
        raise Unsupported("effective address of a register operand")
    t = ctx.tmp("a")
    out += ea.setup(op, sz, t)
    return t

def _write(op, sz, ctx, out, val):
    if isinstance(op, (ea.DReg, ea.AReg, ea.Special)):
        out.append(ea.store(op, sz, None, val))
        return
    t = ctx.tmp("a")
    out += ea.setup(op, sz, t)
    out.append(ea.store(op, sz, t, val))
    out += ea.post(op, sz)

BITS = {'b': 8, 'w': 16, 'l': 32}

# ------------------------------------------------------------ instructions
# Each handler returns a list of C statements.

def i_move(mn, sz, ops, ctx):
    src, dst = ops
    out = []
    # MOVE to/from SR, CCR or USP is a privileged transfer that does NOT set
    # the condition codes from the value moved.
    if isinstance(src, ea.Special) or isinstance(dst, ea.Special):
        w = 'b' if (isinstance(src, ea.Special) and src.name == 'ccr') or \
                   (isinstance(dst, ea.Special) and dst.name == 'ccr') else \
            ('l' if (isinstance(src, ea.Special) and src.name == 'usp') or
                    (isinstance(dst, ea.Special) and dst.name == 'usp') else 'w')
        v = _read(src, w, ctx, out)
        _write(dst, w, ctx, out, v)
        return out
    v = _read(src, sz, ctx, out)
    t = ctx.tmp("m")
    out.append("%s %s = %s;" % (ea.CAST[sz], t, v))
    out.append("flags_logic%d(%s);" % (BITS[sz], t))
    _write(dst, sz, ctx, out, t)
    return out

def i_movea(mn, sz, ops, ctx):
    """MOVEA never affects flags and always writes the full 32 bits."""
    src, dst = ops
    if not isinstance(dst, ea.AReg):
        raise Unsupported("movea to non-address register")
    out = []
    v = _read(src, sz, ctx, out)
    out.append(ea.store(dst, sz, None, v))
    return out

def i_moveq(mn, sz, ops, ctx):
    src, dst = ops
    out = ["CPU.d[%d] = sx8(0x%Xu);" % (dst.n, src.v & 0xFF),
           "flags_logic32(CPU.d[%d]);" % dst.n]
    return out

def i_lea(mn, sz, ops, ctx):
    src, dst = ops
    out = []
    a = _addr(src, 'l', ctx, out)
    out.append("CPU.a[%d] = %s;" % (dst.n, a))
    return out

def i_pea(mn, sz, ops, ctx):
    out = []
    a = _addr(ops[0], 'l', ctx, out)
    out += ["CPU.a[7] -= 4;", "m68k_write32(CPU.a[7], %s);" % a]
    return out

def i_clr(mn, sz, ops, ctx):
    out = []
    _write(ops[0], sz, ctx, out, "0")
    out.append("CPU.n = 0; CPU.z = 1; CPU.v = 0; CPU.c = 0;")
    return out

def i_tst(mn, sz, ops, ctx):
    out = []
    v = _read(ops[0], sz, ctx, out)
    out.append("flags_logic%d(%s);" % (BITS[sz], v))
    return out

def _arith(helper, flags=True, to_areg_no_flags=False):
    """add/sub/cmp family. `dst = dst OP src`."""
    def f(mn, sz, ops, ctx):
        src, dst = ops
        out = []
        s = _read(src, sz, ctx, out)
        # ADDQ/SUBQ and ADDA/SUBA on an address register operate on the full
        # 32 bits and do NOT affect the condition codes.
        if isinstance(dst, ea.AReg) and to_areg_no_flags:
            out.append("CPU.a[%d] = CPU.a[%d] %s %s;"
                       % (dst.n, dst.n, '+' if helper == 'add' else '-',
                          "sx16((uint16_t)%s)" % s if sz == 'w' else s))
            return out
        if isinstance(dst, (ea.DReg, ea.AReg)):
            d = ea.load(dst, sz, None)
            r = ctx.tmp("r")
            out.append("%s %s = %s%d(%s, %s);" % (ea.CAST[sz], r, helper, BITS[sz], d, s))
            if flags:
                out.append(ea.store(dst, sz, None, r))
            return out
        t = _addr(dst, sz, ctx, out)
        d = ea.load(dst, sz, t)
        r = ctx.tmp("r")
        out.append("%s %s = %s%d(%s, %s);" % (ea.CAST[sz], r, helper, BITS[sz], d, s))
        if flags:
            out.append(ea.store(dst, sz, t, r))
        out += ea.post(dst, sz)
        return out
    return f

def i_cmp(mn, sz, ops, ctx):
    src, dst = ops
    out = []
    s = _read(src, sz, ctx, out)
    if isinstance(dst, (ea.DReg, ea.AReg)):
        d = ea.load(dst, sz, None)
    else:
        t = _addr(dst, sz, ctx, out)
        d = ea.load(dst, sz, t)
    out.append("cmp%d(%s, %s);" % (BITS[sz], d, s))
    return out

def i_cmpa(mn, sz, ops, ctx):
    """CMPA compares the full 32 bits, sign-extending a word source."""
    src, dst = ops
    out = []
    s = _read(src, sz, ctx, out)
    s = "sx16((uint16_t)%s)" % s if sz == 'w' else s
    out.append("cmp32(CPU.a[%d], %s);" % (dst.n, s))
    return out

def _logic(cop):
    def f(mn, sz, ops, ctx):
        src, dst = ops
        out = []
        if isinstance(dst, ea.Special):
            w = 'b' if dst.name == 'ccr' else 'w'
            v = _read(src, w, ctx, out)
            out.append(ea.store(dst, w, None,
                                "%s %s %s" % (ea.load(dst, w, None), cop, v)))
            return out
        s = _read(src, sz, ctx, out)
        if isinstance(dst, ea.DReg):
            d = ea.load(dst, sz, None)
            r = ctx.tmp("r")
            out.append("%s %s = %s %s %s;" % (ea.CAST[sz], r, d, cop, s))
            out.append("flags_logic%d(%s);" % (BITS[sz], r))
            out.append(ea.store(dst, sz, None, r))
            return out
        t = _addr(dst, sz, ctx, out)
        d = ea.load(dst, sz, t)
        r = ctx.tmp("r")
        out.append("%s %s = %s %s %s;" % (ea.CAST[sz], r, d, cop, s))
        out.append("flags_logic%d(%s);" % (BITS[sz], r))
        out.append(ea.store(dst, sz, t, r))
        out += ea.post(dst, sz)
        return out
    return f

def i_not(mn, sz, ops, ctx):
    out = []
    op = ops[0]
    if isinstance(op, ea.DReg):
        r = ctx.tmp("r")
        out.append("%s %s = (%s)~%s;" % (ea.CAST[sz], r, ea.CAST[sz], ea.load(op, sz, None)))
        out.append("flags_logic%d(%s);" % (BITS[sz], r))
        out.append(ea.store(op, sz, None, r))
        return out
    t = _addr(op, sz, ctx, out)
    r = ctx.tmp("r")
    out.append("%s %s = (%s)~%s;" % (ea.CAST[sz], r, ea.CAST[sz], ea.load(op, sz, t)))
    out.append("flags_logic%d(%s);" % (BITS[sz], r))
    out.append(ea.store(op, sz, t, r))
    return out

def i_neg(mn, sz, ops, ctx):
    out = []
    op = ops[0]
    if isinstance(op, ea.DReg):
        r = ctx.tmp("r")
        out.append("%s %s = sub%d(0, %s);" % (ea.CAST[sz], r, BITS[sz], ea.load(op, sz, None)))
        out.append(ea.store(op, sz, None, r))
        return out
    t = _addr(op, sz, ctx, out)
    r = ctx.tmp("r")
    out.append("%s %s = sub%d(0, %s);" % (ea.CAST[sz], r, BITS[sz], ea.load(op, sz, t)))
    out.append(ea.store(op, sz, t, r))
    return out

def i_ext(mn, sz, ops, ctx):
    """EXT.W sign-extends byte->word; EXT.L sign-extends word->long."""
    d = ops[0]
    if sz == 'w':
        return ["CPU.d[%d] = (CPU.d[%d] & 0xFFFF0000u) | (sx8((uint8_t)CPU.d[%d]) & 0xFFFFu);"
                % (d.n, d.n, d.n),
                "flags_logic16((uint16_t)CPU.d[%d]);" % d.n]
    return ["CPU.d[%d] = sx16((uint16_t)CPU.d[%d]);" % (d.n, d.n),
            "flags_logic32(CPU.d[%d]);" % d.n]

def i_swap(mn, sz, ops, ctx):
    d = ops[0]
    return ["CPU.d[%d] = (CPU.d[%d] >> 16) | (CPU.d[%d] << 16);" % (d.n, d.n, d.n),
            "flags_logic32(CPU.d[%d]);" % d.n]

def i_exg(mn, sz, ops, ctx):
    a, b = ops
    ra = "CPU.d[%d]" % a.n if isinstance(a, ea.DReg) else "CPU.a[%d]" % a.n
    rb = "CPU.d[%d]" % b.n if isinstance(b, ea.DReg) else "CPU.a[%d]" % b.n
    t = ctx.tmp("x")
    return ["uint32_t %s = %s;" % (t, ra), "%s = %s;" % (ra, rb), "%s = %s;" % (rb, t)]

def i_nop(mn, sz, ops, ctx):
    return ["/* nop */"]

def i_link(mn, sz, ops, ctx):
    an, disp = ops
    return ["CPU.a[7] -= 4;",
            "m68k_write32(CPU.a[7], CPU.a[%d]);" % an.n,
            "CPU.a[%d] = CPU.a[7];" % an.n,
            "CPU.a[7] += %d;" % disp.v]

def i_unlk(mn, sz, ops, ctx):
    an = ops[0]
    return ["CPU.a[7] = CPU.a[%d];" % an.n,
            "CPU.a[%d] = m68k_read32(CPU.a[7]);" % an.n,
            "CPU.a[7] += 4;"]

# ------------------------------------------------ shifts, bits, movem, muldiv

SHIFT_OPS = ('lsl','lsr','asl','asr','rol','ror','roxl','roxr')

def _shift(name):
    def f(mn, sz, ops, ctx):
        out = []
        # Memory form shifts by exactly one, and is word-sized only.
        if len(ops) == 1:
            dst, cnt = ops[0], "1"
        else:
            src, dst = ops
            cnt = ("0x%Xu" % (src.v if src.v else 8)) if isinstance(src, ea.Imm) \
                  else "(CPU.d[%d] & 63)" % src.n
        if isinstance(dst, ea.DReg):
            r = ctx.tmp("r")
            out.append("%s %s = %s%d(%s, %s);"
                       % (ea.CAST[sz], r, name, BITS[sz], ea.load(dst, sz, None), cnt))
            out.append(ea.store(dst, sz, None, r))
            return out
        t = _addr(dst, sz, ctx, out)
        r = ctx.tmp("r")
        out.append("%s %s = %s%d(%s, %s);"
                   % (ea.CAST[sz], r, name, BITS[sz], ea.load(dst, sz, t), cnt))
        out.append(ea.store(dst, sz, t, r))
        return out
    return f

def _bitop(kind):
    """btst/bset/bclr/bchg. Bit number is mod 32 on a data register and mod 8
    on memory -- a real 68000 rule, since memory bit ops address one byte."""
    def f(mn, sz, ops, ctx):
        src, dst = ops
        mem = ea.is_mem(dst)
        width = 8 if mem else 32
        osz = 'b' if mem else 'l'
        out = []
        if isinstance(src, ea.Imm):
            bit = "0x%Xu" % (src.v % width)
        else:
            bit = "(CPU.d[%d] %% %d)" % (src.n, width)
        if mem:
            t = _addr(dst, osz, ctx, out)
            cur, store = ea.load(dst, osz, t), lambda v: ea.store(dst, osz, t, v)
        else:
            cur, store = ea.load(dst, osz, None), lambda v: ea.store(dst, osz, None, v)
        v = ctx.tmp("b")
        out.append("%s %s = %s;" % (ea.CAST[osz], v, cur))
        out.append("CPU.z = ((%s >> %s) & 1) ^ 1;" % (v, bit))
        if kind == 'set':
            out.append(store("(%s | (%s)1u << %s)" % (v, ea.CAST[osz], bit)))
        elif kind == 'clr':
            out.append(store("(%s & (%s)~((%s)1u << %s))" % (v, ea.CAST[osz], ea.CAST[osz], bit)))
        elif kind == 'chg':
            out.append(store("(%s ^ (%s)1u << %s)" % (v, ea.CAST[osz], bit)))
        return out
    return f

def i_movem(mn, sz, ops, ctx):
    """MOVEM. In predecrement mode registers are stored in REVERSE order
    (a7 first, down to d0); every other mode uses ascending order. Loading a
    word into a register sign-extends it to the full 32 bits."""
    a, b = ops
    step = 2 if sz == 'w' else 4
    out = []
    # A single-register MOVEM prints as a bare register rather than a list.
    def as_list(x):
        if isinstance(x, ea.RegList):
            return x
        if isinstance(x, ea.DReg): return ea.RegList(("d%d" % x.n,))
        if isinstance(x, ea.AReg): return ea.RegList(("a%d" % x.n,))
        return x
    if not isinstance(a, ea.RegList) and not isinstance(b, ea.RegList):
        # exactly one side is the register operand; the other is the address
        if ea.is_mem(b): a = as_list(a)
        else:            b = as_list(b)
    else:
        a, b = as_list(a), as_list(b)
    if isinstance(a, ea.RegList):                     # registers -> memory
        regs, dst = a.regs, b
        if isinstance(dst, ea.PreDec):
            an = dst.an
            for r in reversed(_movem_order(regs)):
                out.append("CPU.a[%d] -= %d;" % (an, step))
                out.append("m68k_write%d(CPU.a[%d], (%s)%s);"
                           % (BITS[sz], an, ea.CAST[sz], _reg_c(r)))
            return out
        t = _addr(dst, sz, ctx, out)
        for i, r in enumerate(_movem_order(regs)):
            out.append("m68k_write%d(%s + %d, (%s)%s);"
                       % (BITS[sz], t, i*step, ea.CAST[sz], _reg_c(r)))
        return out
    regs, src = b.regs, a                             # memory -> registers
    if isinstance(src, ea.PostInc):
        an = src.an
        for r in _movem_order(regs):
            out.append("%s = %s;" % (_reg_c(r), _movem_load(sz, "CPU.a[%d]" % an)))
            out.append("CPU.a[%d] += %d;" % (an, step))
        return out
    t = _addr(src, sz, ctx, out)
    for i, r in enumerate(_movem_order(regs)):
        out.append("%s = %s;" % (_reg_c(r), _movem_load(sz, "%s + %d" % (t, i*step))))
    return out

def _movem_order(regs):
    """Canonical MOVEM order: d0..d7 then a0..a7."""
    return sorted(regs, key=lambda r: (r[0] == 'a', int(r[1])))

def _reg_c(r):
    return "CPU.%s[%s]" % ('a' if r[0] == 'a' else 'd', r[1])

def _movem_load(sz, addr):
    return ("sx16(m68k_read16(%s))" % addr) if sz == 'w' else ("m68k_read32(%s)" % addr)

def _muldiv(name):
    def f(mn, sz, ops, ctx):
        src, dst = ops
        out = []
        v = _read(src, 'w', ctx, out)
        if name in ('mulu', 'muls'):
            cast = "(uint32_t)(uint16_t)" if name == 'mulu' else "(uint32_t)(int32_t)(int16_t)"
            out.append("CPU.d[%d] = %s%s * %s(uint16_t)CPU.d[%d];"
                       % (dst.n, cast, v, cast, dst.n))
            out.append("flags_logic32(CPU.d[%d]);" % dst.n)
            return out
        # Division by zero traps on real hardware; the ROM guards its divides,
        # so leave the register untouched and flag it rather than invent a result.
        out.append("if ((uint16_t)%s == 0) { m68k_div_by_zero(); } else {" % v)
        if name == 'divu':
            out += ["  uint32_t q = CPU.d[%d] / (uint16_t)%s;" % (dst.n, v),
                    "  uint32_t rem = CPU.d[%d] %% (uint16_t)%s;" % (dst.n, v)]
        else:
            out += ["  int32_t q = (int32_t)CPU.d[%d] / (int16_t)%s;" % (dst.n, v),
                    "  int32_t rem = (int32_t)CPU.d[%d] %% (int16_t)%s;" % (dst.n, v)]
        out += ["  CPU.d[%d] = ((uint32_t)(rem) << 16) | ((uint32_t)(q) & 0xFFFFu);" % dst.n,
                "  flags_logic16((uint16_t)q);",
                "}"]
        return out
    return f

def _extarith(name):
    """ADDX/SUBX: register-to-register, or -(Ay) to -(Ax) for multi-precision."""
    def f(mn, sz, ops, ctx):
        src, dst = ops
        out = []
        if isinstance(src, ea.DReg) and isinstance(dst, ea.DReg):
            r = ctx.tmp("r")
            out.append("%s %s = %s%d(%s, %s);" % (
                ea.CAST[sz], r, name, BITS[sz],
                ea.load(dst, sz, None), ea.load(src, sz, None)))
            out.append(ea.store(dst, sz, None, r))
            return out
        sa = _addr(src, sz, ctx, out); sv = ctx.tmp("s")
        out.append("%s %s = %s;" % (ea.CAST[sz], sv, ea.load(src, sz, sa)))
        da = _addr(dst, sz, ctx, out)
        r = ctx.tmp("r")
        out.append("%s %s = %s%d(%s, %s);" % (
            ea.CAST[sz], r, name, BITS[sz], ea.load(dst, sz, da), sv))
        out.append(ea.store(dst, sz, da, r))
        return out
    return f

def i_cmpm(mn, sz, ops, ctx):
    """CMPM compares (Ay)+ with (Ax)+; both operands postincrement."""
    src, dst = ops
    out = []
    sa = _addr(src, sz, ctx, out); sv = ctx.tmp("s")
    out.append("%s %s = %s;" % (ea.CAST[sz], sv, ea.load(src, sz, sa)))
    out += ea.post(src, sz)
    da = _addr(dst, sz, ctx, out); dv = ctx.tmp("d")
    out.append("%s %s = %s;" % (ea.CAST[sz], dv, ea.load(dst, sz, da)))
    out += ea.post(dst, sz)
    out.append("cmp%d(%s, %s);" % (BITS[sz], dv, sv))
    return out

def i_illegal(mn, sz, ops, ctx):
    return ["m68k_illegal(0x%Xu);" % ctx.addr, "return 0x%Xu;" % ctx.next]

def _scc(cond):
    def f(mn, sz, ops, ctx):
        out = []
        val = "(cond_%s() ? 0xFFu : 0x00u)" % cond
        _write(ops[0], 'b', ctx, out, val)
        return out
    return f

# ------------------------------------------------------------ control flow
#
# Block model: every basic block compiles to a function returning the next PC.
#
#     uint32_t blk_000200(void) { ...; return 0x20E; }
#
# A dispatch loop drives it (`pc = call_block(pc)`), which means no unbounded
# C stack growth, and an indirect transfer is just a computed return value --
# the $FFFFE002 RAM dispatch needs no special case.
#
# These handlers are TERMINAL: they end a block and must be emitted last.

CONDS = {
    'bra':'t','bhi':'hi','bls':'ls','bcc':'cc','bcs':'cs','bne':'ne','beq':'eq',
    'bvc':'vc','bvs':'vs','bpl':'pl','bmi':'mi','bge':'ge','blt':'lt',
    'bgt':'gt','ble':'le',
}

def _target(op, ctx, out):
    """Resolve a control-transfer destination to a C expression."""
    if isinstance(op, (ea.Branch, ea.PCDisp)):
        return "0x%Xu" % (op.target if isinstance(op, ea.Branch) else op.target)
    if isinstance(op, ea.Abs):
        return "0x%Xu" % (op.addr & 0xFFFFFFFF)
    if isinstance(op, ea.Ind):
        return "CPU.a[%d]" % op.an          # jmp (An) / jsr (An)
    if ea.is_mem(op):
        return _addr(op, 'l', ctx, out)     # jmp d(An,Xn) etc: EA *is* the target
    raise Unsupported("control transfer to %r" % (op,))

def i_bcc(mn, sz, ops, ctx):
    base, _ = split_mnemonic(mn)
    cond = CONDS[base]
    out = []
    tgt = _target(ops[0], ctx, out)
    if cond == 't':
        out.append("return %s;" % tgt)
    else:
        out.append("if (cond_%s()) return %s;" % (cond, tgt))
        out.append("return 0x%Xu;" % ctx.next)
    return out

def i_bsr(mn, sz, ops, ctx):
    out = []
    tgt = _target(ops[0], ctx, out)
    out += ["CPU.a[7] -= 4;",
            "m68k_write32(CPU.a[7], 0x%Xu);" % ctx.next,
            "return %s;" % tgt]
    return out

def i_jsr(mn, sz, ops, ctx):
    return i_bsr(mn, sz, ops, ctx)

def i_jmp(mn, sz, ops, ctx):
    out = []
    tgt = _target(ops[0], ctx, out)
    out.append("return %s;" % tgt)
    return out

def i_rts(mn, sz, ops, ctx):
    t = ctx.tmp("r")
    return ["uint32_t %s = m68k_read32(CPU.a[7]);" % t,
            "CPU.a[7] += 4;",
            "return %s;" % t]

def i_rte(mn, sz, ops, ctx):
    t = ctx.tmp("r")
    return ["set_sr(m68k_read16(CPU.a[7]));",
            "CPU.a[7] += 2;",
            "uint32_t %s = m68k_read32(CPU.a[7]);" % t,
            "CPU.a[7] += 4;",
            "return %s;" % t]

def i_dbcc(mn, sz, ops, ctx):
    """DBcc: if the condition is FALSE, decrement Dn as a word and branch
    unless it wrapped to -1. dbra/dbf never take the condition exit."""
    base, _ = split_mnemonic(mn)
    reg, dest = ops
    cond = {'dbra':'f', 'dbf':'f', 'dbt':'t'}.get(base)
    if cond is None:
        cond = CONDS.get('b' + base[2:], None)
        if cond is None:
            raise Unsupported(base)
    out = []
    tgt = _target(dest, ctx, out)
    if cond != 'f':
        out.append("if (cond_%s()) return 0x%Xu;" % (cond, ctx.next))
    c = ctx.tmp("c")
    out += ["uint16_t %s = (uint16_t)(CPU.d[%d] - 1);" % (c, reg.n),
            "CPU.d[%d] = (CPU.d[%d] & 0xFFFF0000u) | %s;" % (reg.n, reg.n, c),
            "if (%s != 0xFFFFu) return %s;" % (c, tgt),
            "return 0x%Xu;" % ctx.next]
    return out

TERMINAL = set(CONDS) | {'bsr','jsr','jmp','rts','rte','rtr','dbra','dbf','dbt','illegal'}

HANDLERS = {
    'move': i_move, 'movea': i_movea, 'moveq': i_moveq,
    'lea': i_lea, 'pea': i_pea, 'clr': i_clr, 'tst': i_tst,
    'add': _arith('add'), 'addi': _arith('add'), 'addq': _arith('add', to_areg_no_flags=True),
    'adda': _arith('add', to_areg_no_flags=True),
    'sub': _arith('sub'), 'subi': _arith('sub'), 'subq': _arith('sub', to_areg_no_flags=True),
    'suba': _arith('sub', to_areg_no_flags=True),
    'cmp': i_cmp, 'cmpi': i_cmp, 'cmpa': i_cmpa,
    'and': _logic('&'), 'andi': _logic('&'),
    'or': _logic('|'),  'ori':  _logic('|'),
    'eor': _logic('^'), 'eori': _logic('^'),
    'not': i_not, 'neg': i_neg, 'ext': i_ext, 'swap': i_swap, 'exg': i_exg,
    'nop': i_nop, 'link': i_link, 'unlk': i_unlk,
    'bsr': i_bsr, 'jsr': i_jsr, 'jmp': i_jmp, 'rts': i_rts, 'rte': i_rte,
    'movem': i_movem,
    'btst': _bitop('tst'), 'bset': _bitop('set'),
    'bclr': _bitop('clr'), 'bchg': _bitop('chg'),
    'mulu': _muldiv('mulu'), 'muls': _muldiv('muls'),
    'divu': _muldiv('divu'), 'divs': _muldiv('divs'),
}
HANDLERS.update({o: _shift(o) for o in SHIFT_OPS})
HANDLERS.update({'addx': _extarith('addx'), 'subx': _extarith('subx'),
                 'cmpm': i_cmpm, 'illegal': i_illegal})
HANDLERS.update({'st': _scc('t'), 'sf': _scc('f')})
HANDLERS.update({'s'+c: _scc(c) for c in
                 ('hi','ls','cc','cs','ne','eq','vc','vs','pl','mi','ge','lt','gt','le')})
HANDLERS.update({b: i_bcc for b in CONDS})
HANDLERS.update({d: i_dbcc for d in ('dbra','dbf','dbt','dbcc','dbeq','dbne',
                                     'dblt','dbge','dbgt','dble','dbmi','dbpl')})

def is_terminal(mn):
    base, _ = split_mnemonic(mn)
    return base in TERMINAL or base.startswith('db')

def emit(mn, ops, ctx):
    base, sz = split_mnemonic(mn)
    if base not in HANDLERS:
        raise Unsupported(base)
    if sz is None:
        sz = 'l' if base in ('lea', 'pea', 'moveq') else 'w'
    return HANDLERS[base](mn, sz, ops, ctx)

def supported():
    return set(HANDLERS)
