#!/usr/bin/env python3
"""Scripted playthroughs: drive the game, capture each screen, report progress.

Interactive play kept reaching states the scripted tests never did, because
those only ever pressed Start. This turns a manual route into a reproducible
run that captures a screenshot per screen and says exactly where it stopped.

A scenario is a list of steps:
    ('wait',  n)          advance n frames
    ('press', 'b')        press a button (held briefly, then released)
    ('shot',  'name')     capture the frame here
    ('mash',  'b', n, k)  press a button k times, n frames apart

usage: playthrough.py [scenario] [--frames N] [--keep-ppm]
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM  = os.path.join(ROOT, "roms", "Dune-The-Battle-for-Arrakis_Genesis_EN",
                    "Dune - The Battle for Arrakis (E).bin")
OUT  = os.path.join(ROOT, "build", "play")

def mash(button, gap, count):
    return [('mash', button, gap, count)]

ALL_BUTTONS = ['up', 'down', 'left', 'right', 'a', 'b', 'c', 'start']

def sweep(buttons, gap, rounds=1):
    """Press each button in turn, `rounds` times over.

    Exercising every input is how dispatch tables get covered: an arm is only
    reached if something selects it, and a scenario that only presses Start
    selects arm 0 forever. Most crashes found by hand have been later arms of
    a table whose first arm worked fine.
    """
    steps = []
    for _ in range(rounds):
        for b in buttons:
            steps.append(('mash', b, gap, 1))
    return steps

SCENARIOS = {
    # The route reported from interactive play: title -> house select ->
    # advisor briefing -> (previously) a crash in the 0x49xxx subsystem.
    "house": (
        [('wait', 2400), ('shot', '01-title'),
         ('press', 'start'), ('wait', 250), ('shot', '02-after-start'),
         ('press', 'b'),     ('wait', 250), ('shot', '03-house-select')]
        # Advance through the advisor briefing. The count matters: 8 presses
        # completes cleanly, 14 reaches the dispatch at $49C14. Reproducing a
        # reported crash is the point of this scenario, so it mashes properly.
        + mash('b', 90, 14)
        + [('shot', '04-mentat'), ('wait', 400), ('shot', '05-later')]
    ),
    # Into the mission, then exercise every input. This is the scenario that
    # matters for coverage: menus select one dispatch arm, gameplay selects
    # many, and only pressing everything reaches them all.
    "gameplay": (
        [('wait', 2400),
         ('press', 'start'), ('wait', 250),
         ('press', 'b'),     ('wait', 250)]
        + mash('b', 90, 16)                      # through the advisor briefing
        + [('wait', 300), ('shot', '01-in-game')]
        + sweep(ALL_BUTTONS, 45, rounds=3)       # every button, three passes
        + [('shot', '02-after-sweep')]
        + sweep(['up', 'down', 'left', 'right'], 25, rounds=6)   # directions
        + [('shot', '03-after-dpad')]
        + sweep(['a', 'b', 'c', 'start'], 40, rounds=4)          # actions
        + [('shot', '04-after-actions'), ('wait', 300), ('shot', '05-final')]
    ),

    # Stop precisely on the house-select screen. Kept separate from "house"
    # so the screen can be captured and diffed without anything after it.
    "houseselect": [
        ('wait', 2400), ('shot', '01-title'),
        ('press', 'start'), ('wait', 200), ('shot', '02-transition'),
        ('wait', 150),  ('shot', '03-house-select'),
        ('wait', 150),  ('shot', '04-house-select-later'),
        ('wait', 300),  ('shot', '05-house-select-settled'),
    ],

    # Reach the mission and stop, so the in-game screen can be inspected and
    # compared against the reference before any gameplay inputs are scripted.
    "mission": (
        [('wait', 2400),
         ('press', 'start'), ('wait', 250),
         ('press', 'b'),     ('wait', 250)]
        + mash('b', 90, 18)                    # through the advisor briefing
        + [('wait', 200), ('shot', '01-arrive'),
           ('wait', 200), ('shot', '02-settled'),
           ('wait', 400), ('shot', '03-idle')]
    ),

    # Exploration aid: reach the mission, then step the cursor one press at a
    # time with a capture after each, so the mapping from input to on-screen
    # movement can be read off rather than guessed.
    "cursor": (
        [('wait', 2400),
         ('press', 'start'), ('wait', 250),
         ('press', 'b'),     ('wait', 250)]
        + mash('b', 90, 18)
        + [('wait', 300), ('shot', '00-arrive')]
        + [('press', 'right'), ('wait', 50), ('shot', '01-right')]
        + [('press', 'right'), ('wait', 50), ('shot', '02-right')]
        + [('press', 'down'),  ('wait', 50), ('shot', '03-down')]
        + [('press', 'left'),  ('wait', 50), ('shot', '04-left')]
        + [('press', 'up'),    ('wait', 50), ('shot', '05-up')]
        + [('press', 'a'),     ('wait', 80), ('shot', '06-a')]
        + [('press', 'b'),     ('wait', 80), ('shot', '07-b')]
        + [('press', 'c'),     ('wait', 80), ('shot', '08-c')]
    ),

    # Attempt to build a concrete slab. The interface is explored empirically:
    # select the Construction Yard, then step through the build panel one input
    # at a time with a capture after each, so the menu's behaviour can be read
    # off the screenshots rather than assumed.
    # Build a concrete slab, per the sequence described from real play:
    # select the Construction Yard with A, then down+A to start the slab,
    # wait for it to build, then down+A to place it.
    "slab": (
        [('wait', 2400),
         ('press', 'start'), ('wait', 250),
         ('press', 'b'),     ('wait', 250)]
        + mash('b', 90, 18)
        + [('wait', 300),      ('shot', '00-arrive')]
        + [('press', 'a'),     ('wait', 90),  ('shot', '01-yard-selected')]
        + [('press', 'down'),  ('wait', 50)]
        + [('press', 'a'),     ('wait', 120), ('shot', '02-slab-ordered')]
        + [('wait', 900),      ('shot', '03-building')]
        + [('press', 'down'),  ('wait', 50)]
        + [('press', 'a'),     ('wait', 200), ('shot', '04-placement-mode')]
        + [('press', 'b'),     ('wait', 200), ('shot', '05-after-confirm')]
        + [('wait', 400),      ('shot', '06-settled')]
    ),

    # Just the opening, as a fast regression check.
    "intro": [
        ('wait', 300),  ('shot', '01-logo'),
        ('wait', 300),  ('shot', '02-publisher'),
        ('wait', 900),  ('shot', '03-presents'),
        ('wait', 1100), ('shot', '04-title'),
    ],
}

def compile_scenario(steps, hold):
    """Turn a scenario into (press-script, shot-frames, names, last-frame)."""
    frame = 0
    presses, shots, names = [], [], {}
    for step in steps:
        kind = step[0]
        if kind == 'wait':
            frame += step[1]
        elif kind == 'press':
            presses.append("%d:%s" % (frame, step[1]))
            frame += hold + 2
        elif kind == 'shot':
            shots.append(frame)
            names[frame] = step[1]
        elif kind == 'mash':
            _, button, gap, count = step
            for _ in range(count):
                presses.append("%d:%s" % (frame, button))
                frame += gap
        else:
            raise SystemExit("unknown step %r" % (kind,))
    return presses, shots, names, frame

def main(argv):
    name = next((a for a in argv if not a.startswith('--')), 'house')
    # --emit-press prints the compiled input script and frame count, so the
    # bootstrap loop can replay a realistic route instead of a single press.
    if '--emit-press' in argv:
        if name not in SCENARIOS:
            raise SystemExit("unknown scenario %s" % name)
        presses, shots, names, last = compile_scenario(SCENARIOS[name], 6)
        print("%s\n%d" % (",".join(presses), last + 400))
        return 0
    keep = '--keep-ppm' in argv
    frames = None
    for i, a in enumerate(argv):
        if a == '--frames' and i + 1 < len(argv):
            frames = int(argv[i + 1])
    if name not in SCENARIOS:
        raise SystemExit("scenarios: %s" % ", ".join(sorted(SCENARIOS)))

    hold = 6
    presses, shots, names, last = compile_scenario(SCENARIOS[name], hold)
    total = frames or (last + 200)
    os.makedirs(OUT, exist_ok=True)

    env = dict(os.environ)
    env['DB4A_PRESS'] = ",".join(presses)
    env['DB4A_SHOTS'] = ",".join(str(s) for s in shots)
    env['DB4A_PPM']   = os.path.join(OUT, name)
    env['DB4A_HOLD']  = str(hold)

    print("scenario '%s': %d inputs, %d screenshots, %d frames (%.1fs)"
          % (name, len(presses), len(shots), total, total / 50.0))
    print("  inputs: %s" % env['DB4A_PRESS'])

    r = subprocess.run([os.path.join(ROOT, "build", "db4a"), ROM, str(total)],
                       env=env, capture_output=True, text=True)
    out = r.stdout + r.stderr

    # Convert whatever was captured, naming each by its scenario step.
    made = []
    for f in shots:
        ppm = os.path.join(OUT, "%s.%u.ppm" % (name, f))
        if not os.path.exists(ppm):
            continue
        png = os.path.join(OUT, "%s-%s.png" % (name, names[f]))
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "ppm2png.py"),
                        ppm, png, "2"], capture_output=True)
        if not keep:
            os.remove(ppm)
        made.append((f, names[f], png))

    print("\ncaptured %d screens:" % len(made))
    for f, nm, png in made:
        print("   frame %5u  %-16s %s" % (f, nm, os.path.relpath(png, ROOT)))

    missing = [names[f] for f in shots if not any(m[0] == f for m in made)]
    if missing:
        print("\nNOT reached: %s" % ", ".join(missing))

    bad = [l for l in out.splitlines() if 'BAD PC' in l or 'no block for PC' in l]
    inv = [l for l in out.splitlines() if 'INVARIANT' in l]
    for l in out.splitlines():
        if l.startswith(('distinct blocks', 'reason', 'invariants', 'pad reads')):
            print("   %s" % l)
    if bad:
        print("\nCRASHED:")
        for l in bad:
            print("   %s" % l)
        return 1
    if inv:
        print("\nINVARIANT VIOLATIONS:")
        for l in inv:
            print("   %s" % l)
        return 1
    print("\ncompleted without crashing")
    return 0

sys.exit(main(sys.argv[1:]))
