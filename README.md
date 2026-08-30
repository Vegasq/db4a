# db4a project page

The source of <https://vegasq.github.io/db4a/>, the page advertising db4a:
mouse control, widescreen, and the comforts around them.

## Why this is an orphan branch

It shares no history with `master`. That is deliberate: Pages serving from
`master`'s `/docs` would publish the design notes in there as well, which are
working documents and not written for visitors.

There used to be a second reason -- the site advertised the `remaster` branch,
which was rebased and force-pushed and so made a poor Pages source. That branch
was folded into `master` on 2026-08-30 and deleted; `remaster` survives only as
the name of the product. The remaining reason is enough on its own.

So the site lives on its own branch, and `master` does not have to know about
it.

## Layout

```
index.html     the whole page
style.css      the whole stylesheet -- no framework, no build step
img/           screenshots and art
.nojekyll      serve the files as they are; do not run Jekyll over them
```

## Where the screenshots come from

Every screenshot is **db4a's own output**, captured headlessly from the build
rather than taken from anywhere else. They are reproducible:

```bash
ROM="roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin"

# title screen (unattended boot)
DB4A_SHOTS=3200 DB4A_PPM=build/shots/t ./build/db4a "$ROM" 4000

# gameplay, at three view sizes, from the recorded mission
DB4A_REPLAY=data/recordings/level1atredis.txt DB4A_WIDE=400 \
    DB4A_SHOTS=1200,2200,12000,15000 DB4A_PPM=build/shots/w ./build/db4a "$ROM" 15100
DB4A_REPLAY=data/recordings/level1atredis.txt DB4A_WIDE=320 \
    DB4A_SHOTS=12000 DB4A_PPM=build/shots/n ./build/db4a "$ROM" 12100
DB4A_REPLAY=data/recordings/level1atredis.txt DB4A_WIDE=640 DB4A_TALL=480 \
    DB4A_SHOTS=12000 DB4A_PPM=build/shots/big ./build/db4a "$ROM" 12100

# house selection
make playthrough SCENARIO=houseselect

# the campaign gallery: the same recording carries on past mission one.  It
# wins around frame 15200, plays the briefing, enters mission two by 17400,
# and reaches Defeat once the enemy is left to work.
DB4A_REPLAY=data/recordings/level1atredis.txt DB4A_WIDE=400 \
    DB4A_SHOTS=15400,16200,16800,48000,58000 DB4A_PPM=build/shots/m2 \
    ./build/db4a "$ROM" 90000

# the other two houses, scripted rather than recorded -- the same input
# schedule tests/houses.sh uses, with one extra 'right' per house
DB4A_PRESS="2400:start,2760:right,2820:right,3100:b,3300:b,3500:b,3700:b,3900:b,4100:b,4300:b,4500:b,4700:b,4900:b,5100:b,5300:b" \
    DB4A_HOLD=8 DB4A_WIDE=400 DB4A_SHOTS=2900,3250,4400,6400 \
    DB4A_PPM=build/shots/hs2 ./build/db4a "$ROM" 6600
```

Menu and cutscene captures are taken at 400 wide and cropped to the centre
320 (`-crop 320x224+40+0`), because those screens are 320-wide compositions
that db4a centres rather than stretches.

`img/gameplay-320.png` and `img/gameplay-wide.png` are the **same frame** of the
same replay at two view widths, which is what makes the comparison on the page
an honest one. The page shows them at the same pixel scale.

`img/box.jpg` and `img/cart.jpg` are photographs of the retail box and the
European cartridge, from Sega Retro. They identify the game being rebuilt; they
are not game data.

## Preview it locally

```bash
python3 -m http.server 8123 --directory .    # then http://localhost:8123/
```

Add `--bind 0.0.0.0` to reach it from another device on the same network.

## Publishing

Not published yet. When it should be:

```bash
git push origin gh-pages
```

then in the repository's **Settings -> Pages**, set the source to *Deploy from a
branch*, branch `gh-pages`, folder `/ (root)`.

## Working on it

The branch is checked out as a worktree so it never disturbs the branch you are
developing on:

```bash
git worktree add ../db4a-pages gh-pages     # once
git worktree remove ../db4a-pages           # when finished
```
