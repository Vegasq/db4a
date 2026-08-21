#include "m68k.h"
#include <stdio.h>
m68k_t CPU;
void set_sr(uint16_t v){(void)v;}
static int fails=0;
#define CK(what,expr) do{ if(!(expr)){ printf("FAIL %-28s %s\n",what,#expr); fails++; } }while(0)

int main(void){
    /* ADD.B 0x7F+0x01 -> 0x80 : signed overflow, no carry */
    uint8_t r=add8(0x7F,0x01);
    CK("add8 7F+01", r==0x80 && CPU.v==1 && CPU.c==0 && CPU.n==1 && CPU.z==0);
    /* ADD.B 0xFF+0x01 -> 0x00 : carry, no overflow, zero */
    r=add8(0xFF,0x01);
    CK("add8 FF+01", r==0x00 && CPU.v==0 && CPU.c==1 && CPU.z==1 && CPU.x==1);
    /* ADD.L 0x7FFFFFFF+1 : signed overflow */
    uint32_t R=add32(0x7FFFFFFFu,1);
    CK("add32 7FFFFFFF+1", R==0x80000000u && CPU.v==1 && CPU.c==0 && CPU.n==1);
    /* ADD.L 0xFFFFFFFF+1 : carry out */
    R=add32(0xFFFFFFFFu,1);
    CK("add32 FFFFFFFF+1", R==0 && CPU.c==1 && CPU.z==1 && CPU.v==0);
    /* SUB.B 0x80-0x01 -> 0x7F : signed overflow (neg - pos = pos) */
    r=sub8(0x80,0x01);
    CK("sub8 80-01", r==0x7F && CPU.v==1 && CPU.c==0 && CPU.n==0);
    /* SUB.B 0x00-0x01 -> 0xFF : borrow */
    r=sub8(0x00,0x01);
    CK("sub8 00-01", r==0xFF && CPU.c==1 && CPU.n==1 && CPU.v==0 && CPU.x==1);
    /* SUB.L borrow */
    R=sub32(0,1);
    CK("sub32 0-1", R==0xFFFFFFFFu && CPU.c==1 && CPU.n==1 && CPU.v==0);
    /* CMP must NOT disturb X */
    CPU.x=1; cmp16(0,1); CK("cmp16 preserves X=1", CPU.x==1 && CPU.c==1);
    CPU.x=0; cmp16(0,1); CK("cmp16 preserves X=0", CPU.x==0 && CPU.c==1);
    /* logic clears V,C and preserves X */
    CPU.x=1; flags_logic32(0x80000000u);
    CK("logic32 neg", CPU.n==1 && CPU.z==0 && CPU.v==0 && CPU.c==0 && CPU.x==1);
    /* signed conditions: 0x00 - 0x01 leaves N=1,V=0 -> LT true, GE false */
    sub8(0x00,0x01);
    CK("lt after 0-1", cond_lt() && !cond_ge() && !cond_gt() && cond_le());
    /* 0x80 - 0x01 leaves N=0,V=1 -> LT true (overflow makes it still 'less') */
    sub8(0x80,0x01);
    CK("lt after 80-01", cond_lt() && !cond_ge());
    /* sign extension */
    CK("sx8",  sx8(0xFF)==0xFFFFFFFFu);
    CK("sx16", sx16(0x8000)==0xFFFF8000u);
    /* SR packing round-trip of CCR bits */
    CPU.x=1;CPU.n=0;CPU.z=1;CPU.v=0;CPU.c=1;CPU.imask=7;CPU.super=1;
    CK("get_sr", get_sr()==0x2715);


    /* ---- shifts and rotates ---- */
    /* LSL.B 0x81 << 1 = 0x02, last bit out = 1 */
    CPU.x=0; r=lsl8(0x81,1);
    CK("lsl8 0x81<<1", r==0x02 && CPU.c==1 && CPU.x==1 && CPU.v==0);
    /* shift count 0 clears C, leaves X alone */
    CPU.x=1; CPU.c=1; r=lsl8(0x55,0);
    CK("lsl8 count 0", r==0x55 && CPU.c==0 && CPU.x==1 && CPU.v==0);
    /* LSR.B 0x01 >> 1 = 0, bit out = 1, Z set */
    r=lsr8(0x01,1); CK("lsr8 0x01>>1", r==0 && CPU.c==1 && CPU.z==1);
    /* ASR preserves sign */
    r=asr8(0x80,1); CK("asr8 sign preserved", r==0xC0 && CPU.n==1 && CPU.v==0);
    /* ASR.L of a negative value keeps filling with ones */
    R=asr32(0x80000000u,4); CK("asr32 sign fill", R==0xF8000000u && CPU.n==1);
    /* ASL sets V when the sign bit changes during the shift */
    r=asl8(0x40,1); CK("asl8 V on sign change", r==0x80 && CPU.v==1 && CPU.n==1);
    r=asl8(0x01,1); CK("asl8 no V", r==0x02 && CPU.v==0);
    /* ROL.B 0x81 rotates the top bit into bit 0 */
    r=rol8(0x81,1); CK("rol8 0x81", r==0x03 && CPU.c==1);
    /* ROR.B 0x01 rotates bit 0 into the top */
    r=ror8(0x01,1); CK("ror8 0x01", r==0x80 && CPU.c==1 && CPU.n==1);
    /* ROXL rotates through X: X=1, 0x00 -> 0x01, new X = 0 */
    CPU.x=1; r=roxl8(0x00,1); CK("roxl8 through X", r==0x01 && CPU.x==0 && CPU.c==0);
    /* ROXR with count 0 copies X into C and changes nothing */
    CPU.x=1; r=roxr8(0x5A,0); CK("roxr8 count 0", r==0x5A && CPU.c==1);
    /* rotate by a full width is identity */
    r=rol8(0xA5,8); CK("rol8 by 8 identity", r==0xA5);

    printf(fails? "\n%d FAILURES\n" : "\nall flag tests pass\n", fails);
    return fails!=0;
}
