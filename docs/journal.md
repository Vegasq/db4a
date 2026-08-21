# Work journal

Newest entries at the bottom. Each entry records what was done, what it
produced, and anything that turned out to be wrong.

---

## 2026-08-21 — Session 1: analysis foundation

### ROM triage

Nine dumps in `roms/`. Hashed all of them, parsed the Mega Drive header of the
unmarked one, and computed the header checksum over `0x200..EOF`: `0x5E34`,
matching the stored value. That establishes
`Dune - The Battle for Arrakis (E).bin` (sha1 `133cc86b…`) as the clean base.
The `[b*]` files are bad dumps, `[h*]` are header hacks, `[T+Rus]` is a 2 MiB
fan translation. Recorded in `docs/rom.md`.

### Exception vectors

Wrote `tools/vectors.py`. Only two vectors are live: reset at `0x200` and
VBlank (level 6) at `0x17F4`; all others point at `0x200`. **No HBlank
handler**, which removes a class of raster-timing work from the VDP.

### Coarse ROM map — partly a dead end

Wrote `tools/rommap.py` to band the ROM by entropy and classify 1 KiB blocks.
**The classification was wrong.** The opcode heuristic accepted almost every
16-bit high nibble, so it labelled 671 KiB of a 1 MiB cart as code — implausible
on its face. Only the entropy column survives as useful signal. Kept the file
for entropy banding, marked untrustworthy in `CLAUDE.md`. Superseded by
reachability analysis, which is ground truth rather than guesswork.

### Code discovery

Wrote `tools/trace.py`: recursive descent from the vectors, following direct
branches and calls. Three bugs, in order:

1. Reading `ins.groups` raised `CsError` under capstone's SKIPDATA mode. The
   value was never used — deleted.
2. First real run reached only **156 instructions**. Cause: capstone's m68k
   `detail` API reports `disp=0` for absolute addressing, so `jmp $17a4.l` was
   silently unresolvable, and PC-relative targets came back negative and were
   discarded. Cross-checked `4eba f01e` at `0x180E` against
   `m68k-linux-gnu-objdump`, which resolves it to `0x82E`, confirming capstone's
   `op_str` is correct even where `detail` is not. Rewrote target resolution to
   parse `op_str`. Result: **25,280 instructions**, 465 functions, 0 failed
   decodes.
3. Output path was relative, so running from `tools/` crashed on write. Anchored
   to the repo root.

### Jump tables

The 48 unresolved indirect transfers were dominated by 35 instances of
`jmp tbl(pc,dN.w)` — the standard 68k dispatch idiom. Inspected four sites and
found three table formats (`BRA_B`, `BRA_W`, `OFFSET`), with entry counts
bounded by the `cmpi #N,dN / bgt` guard preceding each dispatch. Note: the first
inspection was misread because `od -t x2` byte-swaps on x86; re-read with
`-t x1`.

Wrote `tools/jumptab.py` and iterated the tracer to a fixpoint:

```
round 1: +36 tables, +6295 instructions
round 2: +10 tables,  +414 instructions
round 3:  +2 tables,   +36 instructions
round 4:  +0 tables,    +0 instructions
```

Final: **32,025 instructions, 108,014 bytes (10.3% of ROM), 772 functions,
48 tables resolved, 13 indirect sites unresolved, 76 bad decodes.**

The 76 bad decodes are *not* bogus table entries — attribution showed no table
produced them. They are flow running off function ends into inline data. 0.24%
of instructions; deferred.

### Why coverage stops at 10%

The 13 remaining indirect sites include `jsr (a0)` at `0x181A`, where the target
is a **function pointer read from `$FFFFE002`**. The game's main state machine
dispatches through RAM. Static analysis structurally cannot follow that, so
coverage plateaus near 10% regardless of effort.

Surfaced this to the user as a fork in the road. **Decision: static
recompilation**, plus emulator-based coverage tracing. Indirect transfers will
resolve at runtime, so the plateau is tolerable rather than fatal.

### Instruction mix

Histogrammed the discovered code. 79 base mnemonics but sharply top-heavy:
15 cover 80%, 35 cover 95%. No `TRAP`, `CHK`, BCD, or 68020+ addressing.
~40 instruction forms gets the translator to 99%.

**Finding: the ROM was compiled from C.** 33 matched `link`/`unlk` pairs,
stack-pushed arguments, `addq.l #4,a7` cleanup after calls. Regular calling
conventions and clean function boundaries help every later step.

### Runtime

Wrote `include/m68k.h`: CPU state, memory interface, and flag helpers. Flags
are discrete bytes rather than a packed CCR, because 68000 flag behaviour
varies per instruction and explicit helpers make each case auditable; SR is
only packed for the ~200 sites that touch it directly.

Wrote `tests/test_flags.c` covering signed overflow on `ADD`/`SUB`, carry and
borrow, `CMP` preserving X, signed conditions after an overflowing subtract,
sign extension, and SR packing. All pass. These are the foundation of every
generated instruction, so they were tested before generating anything.

### State at end of session

Analysis tooling and the runtime are done and reproducible via `make`. The
translator itself is **not started**. Remaining large pieces: translator,
dispatch, VDP, audio, input, differential testing.

Raised but undecided: dropping in Musashi as the interpreter fallback would
produce a running build far sooner and give a correctness oracle, avoiding
simultaneous translator and VDP debugging.
