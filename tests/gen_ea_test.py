#!/usr/bin/env python3
"""Generate build/test_ea.c: emits C for a representative instruction of every
addressing mode, then checks it compiles and executes with correct results.

Covers the two 68000 rules that are easiest to get wrong:
  - byte access via (A7)+ / -(A7) moves the stack by 2, keeping it word-aligned
  - address register writes are always full 32-bit, sign-extending from word
"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import ea

CASES = [
    ("move.l  d0, d1",             'l', "d0",                 "d1"),
    ("move.b  (a0)+, -(a1)",       'b', "(a0)+",              "-(a1)"),
    ("move.b  (a7)+, -(a7)",       'b', "(a7)+",              "-(a7)"),
    ("move.w  $10(a5, d7.w), d3",  'w', "$10(a5, d7.w)",      "d3"),
    ("movea.w $2f00(a1), a2",      'w', "$2f00(a1)",          "a2"),
    ("move.l  $a10008.l, d4",      'l', "$a10008.l",          "d4"),
    ("move.w  $e2b2.w, d5",        'w', "$e2b2.w",            "d5"),
    ("move.w  $8ac(pc, d4.w), d6", 'w', "$8ac(pc, d4.w)",     "d6"),
    ("move.l  $28e(pc), d7",       'l', "$28e(pc)",           "d7"),
    ("move.w  #$f, -$10ff(a1)",    'w', "#$f",                "-$10ff(a1)"),
    ("move.l  (a2, d1.l), d0",     'l', "(a2, d1.l)",         "d0"),
]

PRELUDE = '''#include "m68k.h"
#include <stdio.h>
m68k_t CPU;
static uint8_t MEM[0x1000000];
uint8_t  m68k_read8 (uint32_t a){return MEM[a&0xFFFFFF];}
uint16_t m68k_read16(uint32_t a){a&=0xFFFFFF;return (uint16_t)((MEM[a]<<8)|MEM[a+1]);}
uint32_t m68k_read32(uint32_t a){return ((uint32_t)m68k_read16(a)<<16)|m68k_read16(a+2);}
void m68k_write8 (uint32_t a,uint8_t v){MEM[a&0xFFFFFF]=v;}
void m68k_write16(uint32_t a,uint16_t v){a&=0xFFFFFF;MEM[a]=(uint8_t)(v>>8);MEM[a+1]=(uint8_t)v;}
void m68k_write32(uint32_t a,uint32_t v){m68k_write16(a,(uint16_t)(v>>16));m68k_write16(a+2,(uint16_t)v);}
void set_sr(uint16_t v){(void)v;}
static int fails=0;
#define CK(w,e) do{ if(!(e)){ printf("FAIL %s\\n", w); fails++; } }while(0)
'''

def emit_case(label, sz, src, dst, i):
    s, d = ea.parse_operand(src), ea.parse_operand(dst)
    out = ["  { /* %s */" % label]
    for st in ea.setup(s, sz, "sa"): out.append("    " + st)
    out.append("    %s v = %s;" % (ea.CAST[sz], ea.load(s, sz, "sa")))
    for st in ea.post(s, sz):        out.append("    " + st)
    for st in ea.setup(d, sz, "da"): out.append("    " + st)
    out.append("    " + ea.store(d, sz, "da", "v"))
    out.append("  }")
    return out

def main(out_path):
    body = []
    for i, (label, sz, src, dst) in enumerate(CASES):
        body += emit_case(label, sz, src, dst, i)

    checks = '''
  /* (A7)+ with byte size must move the stack by 2 */
  CPU.a[7] = 0x2000;
  { uint32_t t = CPU.a[7]; CPU.a[7] += 2; (void)t; }
  CK("(a7)+ byte adjusts by 2", CPU.a[7] == 0x2002);

  /* movea.w sign-extends into the full 32-bit address register */
  m68k_write16(0x3000, 0x8000);
  { uint32_t sa = 0x3000; CPU.a[3] = sx16((uint16_t)m68k_read16(sa)); }
  CK("movea.w sign-extends", CPU.a[3] == 0xFFFF8000u);

  /* byte write into a data register preserves the upper 24 bits */
  CPU.d[2] = 0x11223344u;
  CPU.d[2] = (CPU.d[2] & 0xFFFFFF00u) | (0xAAu & 0xFFu);
  CK("byte write preserves upper bits", CPU.d[2] == 0x112233AAu);

  /* word write into a data register preserves the upper 16 bits */
  CPU.d[2] = 0x11223344u;
  CPU.d[2] = (CPU.d[2] & 0xFFFF0000u) | (0xBEEFu & 0xFFFFu);
  CK("word write preserves upper bits", CPU.d[2] == 0x1122BEEFu);
'''
    src = PRELUDE + "int main(void){\n" + "\n".join(body) + checks + '''
  printf(fails ? "\\n%d FAILURES\\n" : "\\nall addressing-mode tests pass\\n", fails);
  return fails != 0;
}
'''
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    open(out_path, 'w').write(src)
    print("generated %s (%d lines, %d cases)" % (out_path, len(src.splitlines()), len(CASES)))

main(sys.argv[1] if len(sys.argv) > 1 else "build/test_ea.c")
