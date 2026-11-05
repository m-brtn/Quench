#!/bin/sh
# Builds every game against this engine checkout (FETCHCONTENT_SOURCE_DIR_QUENCH) and checks only that each one
# starts, renders a frame (its capture file is written) and quits with exit code 0. The games' own tests and checks
# are their full gates, run once at the end of the work (AGENTS.md, "Build and test").
#
# Usage: tools/check_games.sh [--preset NAME] [game ...]
#   games: trailforge, ashfall (default: both); --preset: the games' configure and build preset (default: release)
#   checkouts: $TRAILFORGE_DIR (default ../trailforge-public), $ASHFALL_DIR (default ../strategy)
# Each game builds in its own build/quench-check-<preset>, so its usual build directories keep their settings.
set -eu
cd "$(dirname "$0")/.."
engine=$(pwd)

preset=release
games=""
while [ $# -gt 0 ]; do
    case "$1" in
    --preset)
        [ $# -ge 2 ] || { echo "check_games: --preset needs a name" >&2; exit 2; }
        preset=$2
        shift 2
        ;;
    trailforge | ashfall)
        games="$games $1"
        shift
        ;;
    *)
        echo "usage: tools/check_games.sh [--preset NAME] [trailforge] [ashfall]" >&2
        exit 2
        ;;
    esac
done
[ -n "$games" ] || games="trailforge ashfall"

tmp=${TMPDIR:-/tmp}
out=$(mktemp -d "${tmp%/}/quench_check_games.XXXXXX")
failed=""

# check NAME SOURCE_DIR EXECUTABLE ARGS...: the capture goes to $out/NAME.png; logs to $out/NAME.*.log.
check() {
    name=$1
    dir=$2
    exe=$3
    shift 3
    if [ ! -d "$dir" ]; then
        echo "== $name"
        echo "   FAIL: no checkout at $dir"
        failed="$failed $name"
        return
    fi
    dir=$(cd "$dir" && pwd)
    build="$dir/build/quench-check-$preset"
    echo "== $name ($dir, preset $preset)"
    if ! (cd "$dir" && cmake --preset "$preset" -B "$build" --no-warn-unused-cli \
        -DFETCHCONTENT_SOURCE_DIR_QUENCH="$engine") >"$out/$name.configure.log" 2>&1; then
        echo "   FAIL: configure (log: $out/$name.configure.log)"
        failed="$failed $name"
        return
    fi
    if ! cmake --build "$build" --target "$exe" >"$out/$name.build.log" 2>&1; then
        echo "   FAIL: build (log: $out/$name.build.log)"
        failed="$failed $name"
        return
    fi
    capture="$out/$name.png"
    status=0
    # Two minutes at most: a game that hangs is a failure, not a stuck check.
    (cd "$dir" && perl -e 'alarm shift; exec @ARGV' 120 "$build/$exe" --capture "$capture" "$@") \
        >"$out/$name.run.log" 2>&1 || status=$?
    if [ "$status" -ne 0 ]; then
        echo "   FAIL: exit code $status (log: $out/$name.run.log)"
        failed="$failed $name"
    elif [ ! -s "$capture" ]; then
        echo "   FAIL: no capture written (log: $out/$name.run.log)"
        failed="$failed $name"
    else
        echo "   ok: started, rendered $capture, exit code 0"
    fi
}

for game in $games; do
    case "$game" in
    trailforge)
        check trailforge "${TRAILFORGE_DIR:-$engine/../trailforge-public}" trailforge \
            --no-intro --capture-at 2 --quit-after 3
        ;;
    ashfall)
        check ashfall "${ASHFALL_DIR:-$engine/../strategy}" ashfall \
            --demo --no-ui --capture-at 3 --quit-after 4
        ;;
    esac
done

if [ -n "$failed" ]; then
    echo "check_games: failed:$failed (logs and captures in $out)"
    exit 1
fi
echo "check_games: all passed (logs and captures in $out)"
