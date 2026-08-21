"""Reader for the SingleStepTests m68000 vector files (v1 .json.bin).

Each vector gives a full initial CPU + memory state, the expected final state,
and a cycle-level transaction log. We use the states and ignore the
transactions: this validates semantics, not bus timing.

Format (little-endian), per the suite's documentation:
  file   : u32 magic 0x1A3F5D71, u32 test_count, then test_count tests
  test   : u32 size, u32 magic 0xABC12367, name, initial state, final state,
           transaction block
  name   : u32 size, u32 magic 0x89ABCDEF, u32 len, len bytes utf-8
  state  : u32 size, u32 magic 0x01234567,
           19 u32 registers in REG_ORDER, 2 u32 prefetch,
           u32 ram_count, then ram_count entries of (u32 addr, u16 word)
  trans  : u32 size, u32 magic 0x456789AB, u32 cycles, u32 count, entries

Note: `pc` in these vectors is MAME's m_au, the next-prefetch address, which is
4 bytes past the instruction being executed.
"""
from struct import unpack_from

REGS = ['d0','d1','d2','d3','d4','d5','d6','d7',
        'a0','a1','a2','a3','a4','a5','a6','usp','ssp','sr','pc']

MAGIC_FILE, MAGIC_TEST = 0x1A3F5D71, 0xABC12367
MAGIC_NAME, MAGIC_STATE, MAGIC_TRANS = 0x89ABCDEF, 0x01234567, 0x456789AB

def _name(buf, p):
    _, magic = unpack_from('<II', buf, p); p += 8
    assert magic == MAGIC_NAME, "bad name magic"
    n = unpack_from('<I', buf, p)[0]; p += 4
    s = buf[p:p+n].decode('utf-8'); p += n
    return p, s

def _state(buf, p):
    _, magic = unpack_from('<II', buf, p); p += 8
    assert magic == MAGIC_STATE, "bad state magic"
    st = {}
    for r in REGS:
        st[r] = unpack_from('<I', buf, p)[0]; p += 4
    st['prefetch'] = list(unpack_from('<II', buf, p)); p += 8
    nram = unpack_from('<I', buf, p)[0]; p += 4
    ram = {}
    for _ in range(nram):
        addr, word = unpack_from('<IH', buf, p); p += 6
        ram[addr]     = word >> 8        # the suite stores 16-bit words
        ram[addr | 1] = word & 0xFF
    st['ram'] = ram
    return p, st

def _skip_trans(buf, p):
    """Skip the transaction log, reporting whether it contains an address error.

    Kinds: 0 idle, 1 write, 2 read, 3 TAS, 4 read address error,
    5 write address error. Kinds 4 and 5 mean the instruction trapped, which
    the recompiler does not model -- detecting them here is more reliable than
    inferring a trap from the final register state, because a trap taken while
    already in supervisor mode leaves the S bit unchanged.
    """
    _, magic = unpack_from('<II', buf, p); p += 8
    assert magic == MAGIC_TRANS, "bad transaction magic"
    _, count = unpack_from('<II', buf, p); p += 8
    addr_error = False
    for _ in range(count):
        tw = unpack_from('<B', buf, p)[0]
        p += 5                            # kind + cycle count
        if tw != 0:
            p += 20                       # fc, addr, data, UDS, LDS
        if tw in (4, 5):
            addr_error = True
    return p, addr_error

def load(path, limit=None):
    buf = open(path, 'rb').read()
    magic, count = unpack_from('<II', buf, 0)
    assert magic == MAGIC_FILE, "not a m68000 test file"
    p = 8
    out = []
    for i in range(count):
        _, magic = unpack_from('<II', buf, p); p += 8
        assert magic == MAGIC_TEST, "bad test magic"
        t = {}
        p, t['name']    = _name(buf, p)
        p, t['initial'] = _state(buf, p)
        p, t['final']   = _state(buf, p)
        p, t['addr_error'] = _skip_trans(buf, p)
        out.append(t)
        if limit and len(out) >= limit:
            break
    return out

def instr_bytes(test, n=10):
    """Instruction bytes at the executing address (pc - 4)."""
    ram = test['initial']['ram']
    addr = (test['initial']['pc'] - 4) & 0xFFFFFF
    return addr, bytes(ram.get(addr + i, 0) for i in range(n))
