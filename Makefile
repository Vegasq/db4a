# db4a - native rebuild of the Mega Drive "Dune: The Battle for Arrakis"
#
# Analysis targets regenerate everything under build/ from the base ROM.
# Nothing in build/ is tracked in git; it is all reproducible from `make`.

ROM     := roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin
ROM_SHA := 133cc86b43afe133fc9c9142b448340c17fa668e

CFLAGS  := -O1 -Wall -Wextra -Iinclude

.PHONY: all verify-rom analyse test check-operands recomp run play vectors clean
all: test

## verify-rom - confirm the base ROM is the known-good dump
verify-rom:
	@echo "$(ROM_SHA)  $(ROM)" | sha1sum -c -

## analyse - regenerate build/codemap.json from the ROM
analyse: verify-rom
	@mkdir -p build
	python3 tools/trace.py "$(ROM)"

## vectors - run the recompiler against the SingleStepTests m68000 vectors
##           (needs ref/m68k-tests; see docs/verification.md)
VECDIR ?= ref/m68k-tests/v1
VECN   ?= 250
vectors: build/test_vec
	./build/test_vec

build/test_vec.c: tests/gen_vector_test.py tools/m68ktest.py tools/semantics.py tools/ea.py
	@test -d $(VECDIR) || { echo "missing $(VECDIR) -- see docs/verification.md"; exit 1; }
	python3 tests/gen_vector_test.py $@ $(VECDIR) $(VECN)

build/test_vec: build/test_vec.c include/m68k.h
	$(CC) $(CFLAGS) $< -o $@

## check-operands - assert every operand in the corpus parses (needs analyse)
check-operands: build/codemap.json
	python3 tests/check_operands.py

build/codemap.json:
	$(MAKE) analyse

## recomp - regenerate src/gen/blocks.c from the code map and compile it
recomp: build/blocks.o

src/gen/blocks.c: build/codemap.json tools/recomp.py tools/semantics.py tools/ea.py
	python3 tools/recomp.py

build/blocks.o: src/gen/blocks.c include/m68k.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Wno-unused-parameter -c $< -o $@

build/hal_stub.o: src/hal_stub.c include/m68k.h include/vdp.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

## run - build the boot harness and execute the ROM
run: build/db4a
	./build/db4a "$(ROM)" 200000

COMMON_OBJS := build/blocks.o build/hal_stub.o build/hal_vdp.o build/hal_input.o \
               build/hal_z80.o build/z80.o build/render.o build/dispatch.o build/system.o build/invariant.o

build/db4a: $(COMMON_OBJS) build/main.o
	$(CC) $^ -o $@

## play - build and run the interactive SDL build
play: build/db4a-sdl
	./build/db4a-sdl "$(ROM)"

build/db4a-sdl: $(COMMON_OBJS) build/sdl_main.o
	$(CC) $^ -o $@ $(shell pkg-config --libs sdl2)

build/sdl_main.o: src/sdl_main.c include/render.h include/input.h include/hal.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(shell pkg-config --cflags sdl2) -c $< -o $@

build/invariant.o: src/invariant.c include/invariant.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/system.o: src/system.c include/system.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/z80.o: src/z80.c src/z80_exec.h include/z80.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/hal_z80.o: src/hal_z80.c include/z80.h include/m68k.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/hal_input.o: src/hal_input.c include/input.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/hal_vdp.o: src/hal_vdp.c include/vdp.h include/m68k.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/render.o: src/render.c include/render.h include/vdp.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/dispatch.o: src/dispatch.c include/m68k.h include/hal.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/main.o: src/main.c include/m68k.h include/hal.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

## vectors - print the 68000 exception vector table
vectors: verify-rom
	python3 tools/vectors.py "$(ROM)"

## test - build and run all unit tests
test: build/test_flags build/test_ea build/test_sem
	./build/test_flags
	./build/test_ea
	./build/test_sem

build/test_flags: tests/test_flags.c include/m68k.h
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@

build/test_ea.c: tests/gen_ea_test.py tools/ea.py
	@mkdir -p build
	python3 tests/gen_ea_test.py $@

build/test_ea: build/test_ea.c include/m68k.h
	$(CC) $(CFLAGS) $< -o $@

build/test_sem.c: tests/gen_sem_test.py tools/semantics.py tools/ea.py
	@mkdir -p build
	python3 tests/gen_sem_test.py $@

build/test_sem: build/test_sem.c include/m68k.h
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -rf build
