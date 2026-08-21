# db4a - native rebuild of the Mega Drive "Dune: The Battle for Arrakis"
#
# Analysis targets regenerate everything under build/ from the base ROM.
# Nothing in build/ is tracked in git; it is all reproducible from `make`.

ROM     := roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin
ROM_SHA := 133cc86b43afe133fc9c9142b448340c17fa668e

CFLAGS  := -O1 -Wall -Wextra -Iinclude

.PHONY: all verify-rom analyse test check-operands recomp run vectors clean
all: test

## verify-rom - confirm the base ROM is the known-good dump
verify-rom:
	@echo "$(ROM_SHA)  $(ROM)" | sha1sum -c -

## analyse - regenerate build/codemap.json from the ROM
analyse: verify-rom
	@mkdir -p build
	python3 tools/trace.py "$(ROM)"

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

build/db4a: build/blocks.o build/hal_stub.o build/hal_vdp.o build/render.o build/dispatch.o build/main.o
	$(CC) $^ -o $@

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
test: build/test_flags build/test_ea
	./build/test_flags
	./build/test_ea

build/test_flags: tests/test_flags.c include/m68k.h
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@

build/test_ea.c: tests/gen_ea_test.py tools/ea.py
	@mkdir -p build
	python3 tests/gen_ea_test.py $@

build/test_ea: build/test_ea.c include/m68k.h
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -rf build
