#!/usr/bin/env python3
"""Dump the 68000 exception vector table from a Mega Drive ROM."""
import struct, sys

NAMES = ["Initial SSP","Reset PC","Bus Error","Address Error","Illegal Instruction",
    "Divide by Zero","CHK Exception","TRAPV","Privilege Violation","Trace",
    "Line 1010 Emu","Line 1111 Emu","Reserved 12","Reserved 13","Reserved 14",
    "Uninitialised IRQ"]
NAMES += ["Reserved %d" % i for i in range(16,24)]
NAMES += ["Spurious IRQ"] + ["IRQ Level %d" % i for i in range(1,8)]
NAMES += ["TRAP #%d" % i for i in range(16)]
NAMES += ["Reserved %d" % i for i in range(48,64)]

def main(path):
    d = open(path,"rb").read()
    seen = {}
    for i in range(64):
        v = struct.unpack(">I", d[i*4:i*4+4])[0]
        seen.setdefault(v, []).append(i)
        print("%02d  %06X  %-18s %s" % (i, i*4, NAMES[i], "%08X" % v))
    print("\nDistinct handler addresses (targets for code discovery):")
    for v in sorted(seen):
        if v < 0x100000 and len(seen[v]) or v == 0:
            print("  %08X  <- vectors %s" % (v, ",".join(str(x) for x in seen[v])))

main(sys.argv[1])
