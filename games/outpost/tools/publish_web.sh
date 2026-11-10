#!/usr/bin/env bash
# Assembles the web build of Outpost (the web preset) as a static page in dist/web and, with --push, publishes it
# under games/outpost/ of the engine's GitHub Pages site (the gh-pages branch of the engine's repository):
# https://m-brtn.github.io/Quench/games/outpost/
#
#   tools/publish_web.sh            # build the web preset, assemble dist/web, check it in headless Chrome
#   tools/publish_web.sh --no-build # take build/web as it is
#   tools/publish_web.sh --push     # also commit the page to gh-pages and push it (a normal push)
#
# The page: index.html (outpost.html), outpost.js, .wasm and .data, version.txt. The licences of the engine and of
# what it ships are at the site's root (LICENSE, THIRD_PARTY_LICENSES, licenses/), put there by the engine's
# tools/publish_web.sh, which keeps games/ when it replaces the site.
#
# The check runs scenarios/smoke.txt on the page in headless Chrome (no window), then a few seconds on the path a
# person's page takes (--onscreen): exit code 0 and no WebGPU error each.
#
# QUENCH_DIR names the engine checkout (default: two directories up, where this experiment lives).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="$(cd "${QUENCH_DIR:-$ROOT/../..}" && pwd)"
SITE="$ROOT/dist/web"
BUILD=1
PUSH=0
while [[ $# -gt 0 ]]; do
    case "$1" in
    --no-build) BUILD=0 ;;
    --push) PUSH=1 ;;
    -h | --help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n=== %s\n' "$*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

cd "$ROOT"
[[ -f "$ENGINE/tools/web_check.mjs" ]] || fail "no engine checkout at $ENGINE (QUENCH_DIR)"
ENGINE_HASH="$(git -C "$ENGINE" rev-parse --short HEAD)"
ENGINE_VERSION="$(sed -n 's/^project(Quench VERSION \([0-9.]*\).*/\1/p' "$ENGINE/CMakeLists.txt")"
VERSION="$(sed -n 's/^project([A-Za-z_]* VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"

if [[ $BUILD -eq 1 ]]; then
    step "web build (the web preset, the engine from $ENGINE)"
    OVERRIDE="-DFETCHCONTENT_SOURCE_DIR_QUENCH=$ENGINE"
    [[ -f build/host-tools/CMakeCache.txt ]] || cmake --preset host-tools "$OVERRIDE" >/dev/null
    cmake --build --preset host-tools
    [[ -f build/web/CMakeCache.txt ]] || emcmake cmake --preset web "$OVERRIDE" >/dev/null
    cmake --build --preset web
fi

step "page: dist/web"
for f in build/web/outpost.{html,js,wasm,data}; do
    [[ -f "$f" ]] || fail "$f is missing (build the web preset)"
done
rm -rf "$SITE"
mkdir -p "$SITE"
cp build/web/outpost.html "$SITE/index.html"
cp build/web/outpost.{js,wasm,data} "$SITE/"
printf 'Outpost %s, an experiment on Quench %s (engine commit %s), built %s\n%s\n%s\n' \
    "${VERSION:-0}" "$ENGINE_VERSION" "$ENGINE_HASH" "$(date -u +%Y-%m-%dT%H:%MZ)" \
    "https://github.com/m-brtn/Quench" "Licences: ../../LICENSE, ../../THIRD_PARTY_LICENSES, ../../licenses/" \
    >"$SITE/version.txt"
du -sh "$SITE" | awk '{print "dist/web: " $1}'

step "check: the page in headless Chrome (no window)"
check() { # name, then web_check.mjs's arguments
    local name="$1"
    shift
    local log="$ROOT/dist/web_check_$name.log"
    node "$ENGINE/tools/web_check.mjs" --build "$SITE" --repo "$ROOT" --page index.html \
        --out "$ROOT/dist/web_check_$name" --timeout 300 "$@" >"$log" 2>&1 || {
        tail -40 "$log"
        fail "the page's check '$name' (log: $log)"
    }
    grep -E "scenario: [^ ]*smoke.txt: |web_check: (passed|[0-9]+ failed)" "$log"
}
check smoke -- --window 1280x720 --script /repo/scenarios/smoke.txt
check onscreen --onscreen -- --autopilot --quit-after 6

if [[ $PUSH -eq 0 ]]; then
    echo "dist/web is ready (not published: --push)"
    exit 0
fi

step "publish: games/outpost/ on the engine's gh-pages branch"
WORK="$(mktemp -d)"
TEMP_BRANCH="outpost-publish-web-$$" # a local name for the commit; the remote branch is gh-pages
cleanup() {
    git -C "$ENGINE" worktree remove --force "$WORK/site" >/dev/null 2>&1 || true
    git -C "$ENGINE" branch -D "$TEMP_BRANCH" >/dev/null 2>&1 || true
    rm -rf "$WORK"
}
trap cleanup EXIT
git -C "$ENGINE" fetch origin >/dev/null 2>&1 || fail "git fetch origin"
git -C "$ENGINE" rev-parse --verify --quiet origin/gh-pages >/dev/null || fail "no gh-pages branch (the engine's site)"
git -C "$ENGINE" worktree add -b "$TEMP_BRANCH" "$WORK/site" origin/gh-pages >/dev/null 2>&1
rm -rf "$WORK/site/games/outpost"
mkdir -p "$WORK/site/games/outpost"
cp -R "$SITE"/. "$WORK/site/games/outpost/"
git -C "$WORK/site" add -A games/outpost
if git -C "$WORK/site" diff --cached --quiet; then
    echo "gh-pages already holds this page"
else
    git -C "$WORK/site" commit --quiet -m "Publish the Outpost experiment ${VERSION:-0} (engine $ENGINE_HASH)" \
        -m "Built by the game's tools/publish_web.sh (the web preset) and checked in headless Chrome on the Mac."
    git -C "$WORK/site" push origin "$TEMP_BRANCH:gh-pages"
    echo "published: branch gh-pages, games/outpost/"
fi
