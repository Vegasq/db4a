# db4a - native rebuild of the Mega Drive "Dune: The Battle for Arrakis"
#
# Analysis targets regenerate everything under build/ from the base ROM.
# Nothing in build/ is tracked in git; it is all reproducible from `make`.

ROM     := roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin
ROM_SHA := 133cc86b43afe133fc9c9142b448340c17fa668e

CFLAGS  := -O1 -Wall -Wextra -Iinclude

.PHONY: check-margins all verify-rom analyse test check-operands check-cpu check-z80 recomp run play record replay playthrough compare-screen vectors clean
## all - build the emulator and run the unit tests (the default target)
##       `make clean && make` must leave a playable build behind, which is
##       v1 acceptance criterion 8; running only the tests did not.
all: build/db4a build/db4a-sdl test

## verify-rom - confirm the base ROM is the known-good dump
verify-rom:
	@echo "$(ROM_SHA)  $(ROM)" | sha1sum -c -

## analyse - regenerate build/codemap.json from the ROM
analyse: verify-rom
	@mkdir -p build
	python3 tools/trace.py "$(ROM)"

## check-cpu - run the recompiler against the SingleStepTests m68000 vectors
##             (needs ref/m68k-tests; see docs/verification.md)
VECDIR ?= ref/m68k-tests/v1
VECN   ?= 250
check-cpu: build/test_vec
	./build/test_vec

build/test_vec.c: tests/gen_vector_test.py tools/m68ktest.py tools/semantics.py tools/ea.py
	@test -d $(VECDIR) || { echo "missing $(VECDIR) -- see docs/verification.md"; exit 1; }
	python3 tests/gen_vector_test.py $@ $(VECDIR) $(VECN)

build/test_vec: build/test_vec.c include/m68k.h
	$(CC) $(CFLAGS) $< -o $@

## check-z80 - run the Z80 core against the CP/M exerciser suite
##             (needs ref/z80-tests; see docs/verification.md)
Z80ROMS ?= ref/z80-tests/roms
check-z80: build/z80_zex
	./build/z80_zex $(Z80ROMS)/prelim.com 200

build/z80_zex: tests/z80_zex.c build/z80.o include/z80.h
	@mkdir -p build
	$(CC) $(CFLAGS) tests/z80_zex.c build/z80.o -o $@

## record - play the game and save your inputs to a file
##          usage: make record REC=data/recordings/slab.txt
##                 make record REC=... WIDE=400   (record in widescreen)
REC ?= data/recordings/session.txt
record: build/db4a-sdl
	@mkdir -p $(dir $(REC))
	DB4A_RECORD=$(REC) $(if $(WIDE),DB4A_WIDE=$(WIDE),) ./build/db4a-sdl "$(ROM)"

## replay - replay a recording headlessly, capturing screenshots
##          usage: make replay REC=data/recordings/slab.txt SHOTS=6000,9000
SHOTS ?=
replay: build/db4a
	DB4A_REPLAY=$(REC) DB4A_SHOTS=$(SHOTS) DB4A_PPM=build/replay \
	    $(if $(WIDE),DB4A_WIDE=$(WIDE),) ./build/db4a "$(ROM)"

## playthrough - drive the game through a scripted route, capturing each screen
##               usage: make playthrough SCENARIO=house
SCENARIO ?= house
playthrough: build/db4a
	python3 tests/playthrough.py $(SCENARIO)

## compare-screen - capture one scripted moment from db4a and the reference,
##                  then diff them.  usage: make compare-screen SCENARIO=houseselect FRAME=2800
SCENARIO ?= house
FRAME    ?= 2800
compare-screen: build/db4a build/refhost
	./tools/compare_screen.sh $(SCENARIO) $(FRAME)

build/refhost: tools/refhost.c
	@test -d ref/gpgx || { echo "missing ref/gpgx -- see docs/verification.md"; exit 1; }
	$(CC) -O2 -Wall -I ref/gpgx/libretro/libretro-common/include $< -o $@ -ldl

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

COMMON_OBJS := build/blocks.o build/hal_stub.o build/hal_vdp.o build/hal_input.o build/cursor.o build/config.o build/splash.o build/psg.o build/ym2612.o build/savestate.o build/mouse.o build/buildmenu.o build/menus.o build/probe.o \
               build/hal_z80.o build/z80.o build/render.o build/widescreen.o build/mapview.o build/dispatch.o build/system.o build/invariant.o build/inputlog.o

build/db4a: $(COMMON_OBJS) build/main.o
	$(CC) $^ -o $@ -lm

## explore - resume mid-mission and play live, optionally in widescreen
##           usage: make explore              (320, from frame 6000)
##                  make explore WIDE=400     (widescreen spike)
##                  make explore STATE=<file>
STATE ?= data/states/mission1-f6000.state
WIDE  ?=
explore: build/db4a-sdl
	@test -f "$(STATE)" || $(MAKE) --no-print-directory $(STATE)
	DB4A_LOAD="$(STATE)" $(if $(WIDE),DB4A_WIDE=$(WIDE),) ./build/db4a-sdl "$(ROM)"

## the state itself is reproducible from the recorded playthrough
data/states/mission1-f6000.state: build/db4a data/recordings/level1atredis.txt
	@mkdir -p data/states
	DB4A_REPLAY=data/recordings/level1atredis.txt \
	    DB4A_SAVE_AT="6000:$@" ./build/db4a "$(ROM)" 6010 >/dev/null

## play - build and run the interactive SDL build
##        usage: make play          (320, faithful)
##               make play WIDE=400 (experimental widescreen)
play: build/db4a-sdl
	$(if $(WIDE),DB4A_WIDE=$(WIDE),) ./build/db4a-sdl "$(ROM)"

build/db4a-sdl: $(COMMON_OBJS) build/sdl_main.o
	$(CC) $^ -o $@ $(shell pkg-config --libs sdl2) -lm

build/sdl_main.o: src/sdl_main.c include/render.h include/input.h include/hal.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(shell pkg-config --cflags sdl2) -c $< -o $@

build/inputlog.o: src/inputlog.c include/inputlog.h include/input.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

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

build/test_z80_timing: tests/test_z80_timing.c src/z80.c include/z80.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Iinclude -Isrc tests/test_z80_timing.c src/z80.c -o $@

build/test_ym: tests/test_ym.c src/ym2612.c include/ym2612.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Iinclude $^ -o $@ -lm

build/test_psg: tests/test_psg.c src/psg.c include/psg.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Iinclude $^ -o $@ -lm

build/ym2612.o: src/ym2612.c include/ym2612.h include/psg.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/savestate.o: src/savestate.c include/savestate.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/splash.o: src/splash.c include/splash.h include/render.h include/config.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/config.o: src/config.c include/config.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/cursor.o: src/cursor.c include/native.h include/m68k.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/mouse.o: src/mouse.c include/mouse.h include/input.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/buildmenu.o: src/buildmenu.c include/buildmenu.h include/input.h include/hal.h include/probe.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/menus.o: src/menus.c include/menus.h include/buildmenu.h include/probe.h include/input.h include/hal.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/probe.o: src/probe.c include/probe.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/psg.o: src/psg.c include/psg.h include/hal.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/hal_input.o: src/hal_input.c include/input.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/hal_vdp.o: src/hal_vdp.c include/vdp.h include/m68k.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/mapview.o: src/mapview.c include/mapview.h include/render.h include/vdp.h
	$(CC) $(CFLAGS) -c $< -o $@

build/widescreen.o: src/widescreen.c include/widescreen.h include/render.h include/vdp.h
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

## check-mouse - mouse steering must not hold the d-pad outside gameplay
check-mouse: build/db4a
	./tests/mouse.sh

## check-state - save a state mid-run, resume from it, and require the same frame
check-state: build/db4a
	./tests/savestate.sh

## check-native - native C overrides must match the cartridge code they replace
check-native: build/db4a
	./tests/native.sh

## check-menu - pointing at a build-console cell selects it
check-menu: build/db4a
	./tests/buildmenu.sh

## check-map - the map renderer agrees with the tilemap the cartridge drew
check-map: build/db4a
	./tests/mapfidelity.sh

## check-res - the cartridge's own picture survives every view size
check-res: build/db4a
	./tests/resolutions.sh

## check-mission - the recorded mission-1 playthrough is unchanged at any size
check-mission: build/db4a
	./tests/mission_wide.sh

## check-cursor - the cursor can reach the map drawn in the widened strip
check-cursor: build/db4a
	./tests/cursorfield.sh

## check-margins - the widened view's margin fills from the map, in every direction
check-margins: build/db4a
	./tests/margins.sh

## check-menus - pointing at a house shield or a mentat answer selects it
check-menus: build/db4a
	./tests/menus.sh

## check-jump - the picture must not move when a menu or console opens
check-jump: build/db4a
	./tests/nojump.sh

## check-houses - all three houses must select and load their mission
check-houses: build/db4a
	./tests/houses.sh

## test - build and run all unit tests
test: build/test_flags build/test_ea build/test_sem build/test_psg build/test_ym build/test_z80_timing
	./build/test_flags
	./build/test_ea
	./build/test_sem
	./build/test_psg
	./build/test_ym
	./build/test_z80_timing

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

## keytest - standalone SDL keyboard probe: what does SDL see for a given key?
keytest: build/keytest
	./build/keytest

build/keytest: tools/keytest.c
	cc -O1 -Wall -Wextra -o $@ $< $(shell sdl2-config --cflags --libs)
