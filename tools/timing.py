"""68000 instruction timing, used to pace frames.

Cycle counts come from the Motorola 68000 user manual: a base cost per
operation plus an effective-address calculation cost per memory operand.

This is an APPROXIMATION and is documented as one. It does not model
instruction prefetch overlap, and it does not model Mega Drive bus contention,
where the 68000 stalls while the VDP has the bus. It is accurate enough to pace
frames at 49.70 Hz, which is what it exists for; it is NOT accurate enough for
raster-timed effects. This ROM has no HBlank handler, so that is acceptable.

Where a value depends on data (shift counts, multiply, divide) the worst or
typical case is used, noted per entry.
"""
import ea

# Effective-address calculation cost: (byte/word, long)
EA_COST = {
    'DReg':    (0, 0),
    'AReg':    (0, 0),
    'Imm':     (4, 8),
    'Ind':     (4, 8),
    'PostInc': (4, 8),
    'PreDec':  (6, 10),
    'Disp':    (8, 12),
    'Idx':     (10, 14),
    'PCDisp':  (8, 12),
    'PCIdx':   (10, 14),
    'Abs':     (8, 12),      # abs.w; abs.l corrected below
    'Special': (0, 0),
    'Branch':  (0, 0),
    'RegList': (0, 0),
}

# Base cost per mnemonic: (byte/word, long). Memory destinations add more,
# handled by the caller adding EA cost for each operand.
BASE = {
    'move': (4, 4),   'movea': (4, 4),  'moveq': (4, 4),
    'lea':  (4, 4),   'pea':   (12, 12),
    'clr':  (4, 6),   'tst':   (4, 4),
    'add':  (4, 8),   'addi':  (8, 16),  'addq': (4, 8),  'adda': (8, 8),
    'sub':  (4, 8),   'subi':  (8, 16),  'subq': (4, 8),  'suba': (8, 8),
    'cmp':  (4, 6),   'cmpi':  (8, 14),  'cmpa': (6, 6),  'cmpm': (12, 20),
    'and':  (4, 8),   'andi':  (8, 16),
    'or':   (4, 8),   'ori':   (8, 16),
    'eor':  (4, 8),   'eori':  (8, 16),
    'not':  (4, 6),   'neg':   (4, 6),
    'ext':  (4, 4),   'swap':  (4, 4),   'exg': (6, 6),
    'nop':  (4, 4),
    'link': (16, 16), 'unlk':  (12, 12),
    'addx': (4, 8),   'subx':  (4, 8),
    'btst': (4, 6),   'bset':  (8, 8),   'bclr': (10, 10), 'bchg': (8, 8),
    'mulu': (70, 70), 'muls':  (70, 70),   # data dependent; typical case
    'divu': (140, 140), 'divs': (158, 158),  # data dependent; worst case
    'jmp':  (8, 8),   'jsr':   (16, 16),
    'rts':  (16, 16), 'rte':   (20, 20),  'rtr': (20, 20),
    'illegal': (34, 34),
    'movem': (12, 12),   # plus per-register cost, added below
}

SHIFT = ('lsl','lsr','asl','asr','rol','ror','roxl','roxr')
BRANCH_TAKEN, BRANCH_NOT_TAKEN = 10, 8
DBCC_TAKEN, DBCC_EXPIRED = 10, 14

def _kind(op):
    return type(op).__name__

def _ea(op, is_long):
    k = _kind(op)
    c = EA_COST.get(k, (0, 0))[1 if is_long else 0]
    if k == 'Abs' and getattr(op, 'size', 'w') == 'l':
        c = 16 if is_long else 12
    return c

def cycles(mn, ops, size):
    """Estimated cycles for one instruction."""
    base, _ = mn.split('.', 1) if '.' in mn else (mn, None)
    is_long = (size == 'l')

    if base in SHIFT:
        # 6 cycles plus 2 per bit shifted; assume a typical count of 4.
        n = 8 if is_long else 6
        return n + 2 * 4

    if base.startswith('db'):
        return DBCC_TAKEN            # taken is the common case in a loop

    if base in ('bra', 'bsr'):
        return 10 if base == 'bra' else 18
    if base.startswith('b') and base not in ('bset','bclr','bchg','btst'):
        return BRANCH_TAKEN          # assume taken; the difference is 2 cycles

    if base == 'movem':
        nregs = sum(len(o.regs) for o in ops if _kind(o) == 'RegList')
        per = 8 if is_long else 4
        return 12 + per * max(nregs, 1)

    b = BASE.get(base)
    if b is None:
        return 8                      # unknown: a plausible middling cost
    total = b[1 if is_long else 0]
    for o in ops:
        total += _ea(o, is_long)
    return total

def block_cycles(instrs):
    """Sum cycles for a list of (mnemonic, operands, size)."""
    return sum(cycles(mn, ops, sz) for mn, ops, sz in instrs)

# PAL Mega Drive: the 68000 runs at 53203424/7 Hz and the display is 49.70 Hz
# (313 lines * 3420 master clocks per frame), NOT 50 -- see include/hal.h.
PAL_CPU_HZ       = 53203424 // 7      # 7,600,489
PAL_FRAME_CYCLES = PAL_CPU_HZ // 50   # 152,009
NTSC_CPU_HZ       = 53693175 // 7     # 7,670,453
NTSC_FRAME_CYCLES = NTSC_CPU_HZ // 60
