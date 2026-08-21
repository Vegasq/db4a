#!/usr/bin/env python3
"""Generate build/test_sem.c: run generated code for whole instructions and
check the resulting CPU state.

test_flags.c covers flag helpers and test_ea.c covers addressing modes, but
neither exercises a complete instruction end to end. A sign-extension bug in
LINK survived both and corrupted the stack pointer, so this file tests
instructions through the actual generator output.
"""
import os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import ea, semantics

# (label, capstone mnemonic, operand text, setup C, expected C)
CASES = [
    # LINK with a negative displacement -- capstone prints it UNSIGNED.
    ("link.w a6,#$ffc8 allocates 56 bytes", "link.w", "a6, #$ffc8",
     "CPU.a[7] = 0xFFFFF000u; CPU.a[6] = 0x11223344u;",
     "CPU.a[7] == 0xFFFFF000u - 4 - 56 && CPU.a[6] == 0xFFFFF000u - 4"),
    ("link.w a6,#$0 allocates nothing", "link.w", "a6, #$0",
     "CPU.a[7] = 0xFFFFF000u;",
     "CPU.a[7] == 0xFFFFF000u - 4"),
    ("link.w a5,#$ffa8 allocates 88 bytes", "link.w", "a5, #$ffa8",
     "CPU.a[7] = 0xFFFFF000u;",
     "CPU.a[7] == 0xFFFFF000u - 4 - 88"),
    # UNLK must undo LINK exactly.
    ("unlk a6 restores sp and a6", "unlk", "a6",
     "CPU.a[7] = 0xFFFF0000u; CPU.a[6] = 0xFFFFF100u;"
     " m68k_write32(0xFFFFF100u, 0xFFFFF200u);",
     "CPU.a[7] == 0xFFFFF104u && CPU.a[6] == 0xFFFFF200u"),
    # movea.w sign-extends into the full 32 bits
    ("movea.w #$ffc8 sign-extends", "movea.w", "#$ffc8, a3",
     "CPU.a[3] = 0;",
     "CPU.a[3] == 0xFFFFFFC8u"),
    # moveq sign-extends from 8 bits
    ("moveq #$ff", "moveq", "#$ff, d4",
     "CPU.d[4] = 0;",
     "CPU.d[4] == 0xFFFFFFFFu"),
    # ext.w / ext.l
    # EXT.W sign-extends the low byte into the low word, preserving bits 31-16.
    ("ext.w widens byte to word", "ext.w", "d5",
     "CPU.d[5] = 0x11111180u;",
     "CPU.d[5] == 0x1111FF80u"),
    ("ext.w positive byte", "ext.w", "d5",
     "CPU.d[5] = 0x1111117Fu;",
     "CPU.d[5] == 0x1111007Fu"),
    ("ext.l widens word to long", "ext.l", "d5",
     "CPU.d[5] = 0x00008000u;",
     "CPU.d[5] == 0xFFFF8000u"),
    # addq/subq on an address register are long and do not set flags
    ("addq.w #2,a2 is a full 32-bit add", "addq.w", "#$2, a2",
     "CPU.a[2] = 0xFFFFFFFEu; CPU.z = 1;",
     "CPU.a[2] == 0x00000000u && CPU.z == 1"),
    # swap
    ("swap d6", "swap", "d6",
     "CPU.d[6] = 0x12345678u;",
     "CPU.d[6] == 0x56781234u"),
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
void set_sr(uint16_t v){ set_ccr((uint8_t)(v & 0xFF)); CPU.imask=(v>>8)&7;\n                         CPU.super=(v>>13)&1; CPU.trace=(v>>15)&1; }
void m68k_div_by_zero(void){}
void m68k_illegal(uint32_t pc){(void)pc;}
void m68k_unimplemented(uint32_t pc){(void)pc;}
static int fails=0;
#define CK(w,e) do{ if(!(e)){ printf("FAIL %s\\n", w); fails++; } }while(0)
'''

def main(out_path):
    body = []
    for label, mn, ops, setup, expect in CASES:
        stmts = semantics.emit(mn, ea.parse(ops), semantics.Ctx(0x1000, 4, 0x1004))
        body.append("  { /* %s : %s %s */" % (label, mn, ops))
        body.append("    " + setup)
        for st in stmts:
            body.append("    " + st)
        body.append('    CK("%s", %s);' % (label, expect))
        body.append("  }")

    src = PRELUDE + "int main(void){\n" + "\n".join(body) + '''
  printf(fails ? "\\n%d FAILURES\\n" : "\\nall semantics tests pass\\n", fails);
  return fails != 0;
}
'''
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    open(out_path, 'w').write(src)
    print("generated %s (%d cases)" % (out_path, len(CASES)))

main(sys.argv[1] if len(sys.argv) > 1 else "build/test_sem.c")
