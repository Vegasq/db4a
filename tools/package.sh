#!/usr/bin/env bash
#
# Package a build somebody else can run: the game, the ROM it needs, and a
# settings file, in one self-contained folder.
#
# The folder is deliberately not the build tree. A tester gets a launcher that
# does not care where the folder ends up, a db4a.conf they can edit, and a
# report/ directory that collects the two things worth sending back -- a
# recorded session, and any unknown PC the dispatcher hit.
#
#   ./tools/package.sh              -> build/dist/db4a-<branch>-<sha>/
#   ./tools/package.sh -t           -> ... and the same as a .tar.gz
#
# The package contains the cartridge. That is the point of it, and it is also
# why the default output lives under build/, which is gitignored: ground rule 1
# is that a ROM never reaches the repository.
set -euo pipefail

die() { printf '%s\n' "$*" >&2; exit 1; }

root=$(git rev-parse --show-toplevel 2>/dev/null) || die "not in a git checkout"
cd "$root"

# A Windows package differs from a Unix one in four ways, all of them following
# from the same fact: the thing being shipped is a native Windows program.
# It carries .exe, it has to bring SDL2.dll with it because there is no system
# package manager on the far side to install one, its launcher is a .bat, and
# .zip is the archive a Windows user can open without installing anything.
case $(uname -s) in
    MINGW*|MSYS*|CYGWIN*) host=windows; exe=.exe ;;
    *)                    host=unix;    exe= ;;
esac

ROM_DEFAULT='roms/Dune-The-Battle-for-Arrakis_Genesis_EN/Dune - The Battle for Arrakis (E).bin'
ROM_SHA=133cc86b43afe133fc9c9142b448340c17fa668e

out=build/dist
rom=$ROM_DEFAULT
name=
tarball=0
force=0
dobuild=1

usage() {
    cat <<'USAGE'
usage: tools/package.sh [options]

  -o DIR      where to put the package     (default build/dist)
  -r ROM      the cartridge to ship        (default the Makefile's)
  -n NAME     package folder name          (default db4a-<branch>-<sha>)
  -t          also write an archive beside the folder
              (NAME.tar.gz, or NAME.zip when packaging on Windows)
  -B          do not build; package whatever is already in build/
  -f          package anyway when the ROM is not the known-good dump,
              or when the output would land in a tracked part of the repo
  -h          this message
USAGE
}

while getopts ':o:r:n:tBfh' opt; do
    case $opt in
        o) out=$OPTARG ;;
        r) rom=$OPTARG ;;
        n) name=$OPTARG ;;
        t) tarball=1 ;;
        B) dobuild=0 ;;
        f) force=1 ;;
        h) usage; exit 0 ;;
        :) die "-$OPTARG needs an argument" ;;
        \?) usage >&2; die "unknown option -$OPTARG" ;;
    esac
done

[ -f "$rom" ] || die "no ROM at: $rom
  pass one with -r, or put the dump where the Makefile expects it."

# Ground rule 1, enforced rather than trusted: refuse to write a ROM anywhere
# git might pick it up. build/ and roms/ are gitignored; outside the checkout
# is the tester's business.
case $out in
    /*) abs_out=$out ;;
    *)  abs_out=$root/$out ;;
esac
case "$abs_out/" in
    "$root"/*)
        rel=${abs_out#"$root"/}
        if ! git check-ignore -q "$rel" 2>/dev/null; then
            [ "$force" = 1 ] || die "refusing to write a ROM to $rel: git does not ignore it.
  Use build/dist, somewhere outside the checkout, or -f if you are sure."
            echo "warning: $rel is not gitignored and will contain the ROM" >&2
        fi ;;
esac

mkdir -p "$out"

sha=$(sha1sum "$rom" | cut -d' ' -f1)
if [ "$sha" != "$ROM_SHA" ]; then
    [ "$force" = 1 ] || die "that is not the known-good dump.
  want $ROM_SHA
  got  $sha
  Every measurement in this project is against the (E) cartridge. -f overrides."
    echo "warning: packaging an unverified ROM ($sha)" >&2
fi

branch=$(git symbolic-ref --quiet --short HEAD 2>/dev/null || echo detached)
commit=$(git rev-parse --short HEAD)
dirty=
git diff --quiet HEAD 2>/dev/null || dirty=-dirty
[ -n "$name" ] || name="db4a-${branch//\//-}-${commit}${dirty}"

if [ "$dobuild" = 1 ]; then
    echo "== building"
    make all
fi
[ -x "build/db4a-sdl$exe" ] || die "build/db4a-sdl$exe is missing -- run without -B"

dest=$out/$name
echo "== packaging $dest"
rm -rf "$dest"
mkdir -p "$dest/report"

cp "build/db4a-sdl$exe" "$dest/db4a$exe"
cp "$rom" "$dest/dune.bin"

if [ "$host" = windows ]; then
    # SDL2 is the only DLL the binary needs that Windows does not already have,
    # and SDL2.dll itself pulls in nothing further from mingw64 -- so this one
    # file makes the package self-contained. Found through ldd rather than
    # hardcoded, so a different MSYS2 prefix still works.
    dll=$(ldd "build/db4a-sdl$exe" | awk '/[Ss][Dd][Ll]2\.dll/ {print $3; exit}')
    [ -n "${dll:-}" ] && [ -f "$dll" ] || die "cannot find SDL2.dll for build/db4a-sdl$exe
  ldd said: $(ldd "build/db4a-sdl$exe" | grep -i sdl2 || echo '(nothing)')"
    cp "$dll" "$dest/SDL2.dll"
fi

# The settings file is the tracked template, which is entirely comments, plus
# one active line. `state` has to be set because the built-in default is
# build/state.db4a and a package has no build/ -- and it is set in the FILE
# rather than the launcher's environment so that a tester editing it wins,
# which is the way round db4a's settings are meant to work.
{
    cat db4a.conf.example
    echo
    echo "# Set when this package was made: a package has no build/ directory,"
    echo "# which is where the built-in default would put save states."
    echo "state = saves/state.db4a"
} > "$dest/db4a.conf"
mkdir -p "$dest/saves"

if [ "$host" = windows ]; then
# cmd.exe is picky about line endings in a .bat -- a `goto` label with a bare
# LF before it is not always found -- so this one is written with CRLF.
sed 's/$/\r/' > "$dest/play.bat" <<'LAUNCHER'
@echo off
rem Start db4a. Double-click this file, or run it from a console.
rem
rem   play.bat                 play
rem   play.bat 4               a bigger window (the number is a pixel scale)
rem   play.bat --record        play, and save your inputs to report\
rem   play.bat --replay FILE   play a recording back
rem
rem db4a.exe needs the cartridge named on its command line, which is the whole
rem reason this file exists -- double-clicking the .exe would show its usage
rem and quit.
setlocal EnableExtensions
rem %~dp0 is captured before any shift, because shift moves %0 too, and it is
rem used to launch the exe by full path: a console with
rem NoDefaultCurrentDirectoryInExePath set -- which is what PowerShell hands
rem to a child cmd -- will not find db4a.exe by name in the current directory.
set "HERE=%~dp0"
cd /d "%HERE%"
if not exist saves  md saves
if not exist report md report

rem Diagnostics are environment-only by design: they make one run behave
rem unusually, so they do not belong in a file that persists across runs.
if "%DB4A_SEEDS%"=="" set "DB4A_SEEDS=report\seeds.txt"

if /i "%~1"=="--record" goto record
if /i "%~1"=="--replay" goto replay
"%HERE%db4a.exe" dune.bin %*
goto :eof

:record
shift
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "stamp=%%t"
set "DB4A_RECORD=report\session-%stamp%.txt"
echo recording to %DB4A_RECORD% -- send it back with report\ if you hit a bug
"%HERE%db4a.exe" dune.bin %1 %2 %3
goto :eof

:replay
shift
if "%~1"=="" (
    echo usage: play.bat --replay FILE
    exit /b 2
)
set "DB4A_REPLAY=%~1"
shift
"%HERE%db4a.exe" dune.bin %1 %2 %3
LAUNCHER
else
cat > "$dest/play.sh" <<'LAUNCHER'
#!/usr/bin/env bash
# Start db4a. Run it from anywhere -- it works out where it lives.
#
#   ./play.sh                 play
#   ./play.sh --record        play, and save your inputs to report/
#   ./play.sh --replay FILE   play a recording back
#
# Anything else is passed to the game (a second argument is the window scale).
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$here"
mkdir -p saves report

if command -v ldd >/dev/null 2>&1 && ldd ./db4a 2>/dev/null | grep -q 'not found'; then
    echo "Missing libraries -- db4a needs SDL2:" >&2
    ldd ./db4a | grep 'not found' >&2
    echo >&2
    echo "  Debian/Ubuntu:  sudo apt install libsdl2-2.0-0" >&2
    echo "  Fedora:         sudo dnf install SDL2" >&2
    echo "  Arch:           sudo pacman -S sdl2" >&2
    exit 1
fi

# Diagnostics are environment-only by design: they make one run behave
# unusually, so they do not belong in a file that persists across runs.
export DB4A_SEEDS="${DB4A_SEEDS:-report/seeds.txt}"

case "${1:-}" in
    --record)
        shift
        rec="report/session-$(date +%Y%m%d-%H%M%S).txt"
        echo "recording to $rec -- send it back with report/ if you hit a bug"
        DB4A_RECORD="$rec" exec ./db4a dune.bin "$@" ;;
    --replay)
        shift
        [ $# -ge 1 ] || { echo "usage: ./play.sh --replay FILE" >&2; exit 2; }
        rep=$1; shift
        DB4A_REPLAY="$rep" exec ./db4a dune.bin "$@" ;;
    *)
        exec ./db4a dune.bin "$@" ;;
esac
LAUNCHER
chmod +x "$dest/play.sh"
fi

cat > "$dest/BUILD.txt" <<EOF
db4a build identification -- quote this in any bug report.

package   $name
branch    $branch
commit    $(git rev-parse HEAD)${dirty:+   (WITH UNCOMMITTED CHANGES)}
built     $(date -u '+%Y-%m-%d %H:%M:%S UTC')
host      $(uname -srm)
compiler  $(${CC:-cc} --version 2>/dev/null | head -1)

binary    sha1 $(sha1sum "build/db4a-sdl$exe" | cut -d' ' -f1)
cartridge sha1 $sha
          from $(basename "$rom")

Links against SDL2 at run time:
$(if [ "$host" = windows ]; then
      echo "  SDL2.dll, shipped in this folder -- sha1 $(sha1sum "$dest/SDL2.dll" | cut -d' ' -f1)"
  else
      ldd build/db4a-sdl 2>/dev/null | grep -i sdl | sed 's/^[[:space:]]*/  /' || echo '  (ldd unavailable)'
  fi)
EOF

cat > "$dest/README.txt" <<'EOF'
db4a -- Dune: The Battle for Arrakis, rebuilt as a native program
================================================================

This is not an emulator running a ROM. The cartridge's 68000 code has been
translated to C ahead of time and compiled into the program next to this file;
`dune.bin` is here because that code still reads its own graphics, maps and
sound data out of the cartridge.


@RUNNING@


Controls
--------

    the mouse       point at the map and the cursor goes there
    left / right /
      middle click  A / B / C
    arrows          D-pad
    Q / W / E       A / B / C          (Z/X/C and Space/Alt/Shift also work)
    Enter           Start              (Tab also works)
    F5 / F9         save / load a state -- the cartridge has no battery save
    P               pause
    ` or F          fast-forward while held
    F11             fullscreen
    Esc             quit

Pointing also works in the menus: the Construction Yard's build console, the
Starport, house selection and the mentat's yes/no. Gamepads are picked up
automatically.


What is different from the Mega Drive
------------------------------------

Two things are on by default here that the cartridge never did, because they
are the point of this build:

  * the view is 400 pixels wide instead of 320, opening more map on the left
    during play
  * the mouse drives the cursor, which on the cartridge crawls at three pixels
    a frame

Either can be turned off in db4a.conf -- `wide = 320` and `mouse = 0` -- and
with both off you are looking at the cartridge's own picture, pixel for pixel.


Settings
--------

`db4a.conf`, in this folder, is read at start-up. Every setting in it is
explained in place and commented out at its default, so you can uncomment a
line, restart, and see what changes. The start-up banner prints what the file
actually resolved to, so you can tell whether your edit was picked up.

Worth knowing about: `wide` and `mouse` (above), `gain` (volume), `mute`,
`fullscreen` and `integer`, and `splash` (how long the start-up notice is held;
`splash = 0` skips it).


Reporting a bug
---------------

Send back:

  * BUILD.txt         -- says exactly which build you were running
  * report/           -- anything the game wrote there
  * what you did, and what you expected instead

`@PLAY@ --record` writes your inputs to report/. A recording replays
deterministically, which means a bug captured in one can be reproduced exactly
rather than hunted for -- it is by far the most useful thing you can send.

If the game stops with a message about an "unknown PC", that is the one fault
that is genuinely useful raw: it appends a line to report/seeds.txt, and that
line is enough to fix it.


What to expect
--------------

Audio is incomplete: the music plays, in-game sound is not yet right.

Save states are a convenience, not part of the original -- the cartridge had
no save at all, so a mission was one sitting.
EOF

if [ "$host" = windows ]; then
    play='play.bat'
    running='Running it
----------

Double-click play.bat, or run it from a console. Everything it needs is in
this folder -- SDL2.dll is here, and there is nothing to install.

    play.bat 4               a bigger window (the number is a pixel scale)
    play.bat --record        play, and write your inputs to report/
    play.bat --replay FILE   play a recorded session back

db4a.exe is the game itself, but it wants the cartridge named on its command
line, so double-clicking it shows a usage message and quits. play.bat is the
way in.'
else
    play='./play.sh'
    running='Running it
----------

    ./play.sh

It needs SDL2 installed, and nothing else. If it is missing, play.sh says so
and names the package to install.

    ./play.sh 4              a bigger window (the number is a pixel scale)
    ./play.sh --record       play, and write your inputs to report/
    ./play.sh --replay FILE  play a recorded session back'
fi
# awk rather than sed: the replacement is multi-line, and this way neither the
# launcher name nor the block needs escaping.
awk -v r="$running" -v pl="$play" '{ sub(/@RUNNING@/, r); gsub(/@PLAY@/, pl); print }' "$dest/README.txt" > "$dest/README.tmp"
mv "$dest/README.tmp" "$dest/README.txt"

# Anything the tester's own run writes lands here, so the folder they send
# back is the folder they were given.
cat > "$dest/report/README.txt" <<EOF
Anything worth sending back is written into this directory: recorded sessions
from $play --record, and seeds.txt if the game ever hits an unknown PC.
EOF

echo
du -sh "$dest" | sed 's/^/   /'
find "$dest" -type f | sed "s|^$dest/|   |" | sort

if [ "$tarball" = 1 ]; then
    echo
    echo "== archiving"
    if [ "$host" = windows ]; then
        # .zip, because it is the one archive a Windows user can open with
        # nothing installed. -X drops the Unix uid/gid attribute fields, which
        # mean nothing on the far side.
        archive=$out/$name.zip
        command -v zip >/dev/null 2>&1 || die "zip is not installed (pacman -S zip)"
        rm -f "$archive"
        ( cd "$out" && zip -qrX "$name.zip" "$name" )
    else
        archive=$out/$name.tar.gz
        tar -C "$out" -czf "$archive" "$name"
    fi
    ls -lh "$archive" | awk '{print "   " $9 "  " $5}'
fi

echo
echo "done: $dest"
echo "      hand over the whole folder; $play is the entry point"
