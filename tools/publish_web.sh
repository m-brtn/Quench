#!/usr/bin/env bash
# Assembles the web build of the engine demo and the sample (the web preset) as a static site in dist/web and, with
# --push, publishes it on GitHub Pages through the gh-pages branch (docs/platforms.md, "Web (WebAssembly)").
#
#   tools/publish_web.sh             # build the web preset, assemble dist/web, check it in headless Chrome
#   tools/publish_web.sh --no-build  # take build/web as it is (tools/check.sh step web built and checked it)
#   tools/publish_web.sh --push      # also commit the site to the gh-pages branch and push it (a normal push)
#
# The site: index.html (the demo's page; its arguments come from the query string, index.html?--section&sky),
# hello.html (the sample), their .js, .wasm and .data files, LICENSE, THIRD_PARTY_LICENSES, Emscripten's licence and
# the licences of its system libraries in licenses/emscripten/, version.txt (the version and the commit) and
# .nojekyll (the files are served as they are). The check starts the site's index.html in headless Chrome (no window)
# with the demo's smoke scenario, on the path a person's page takes (--onscreen): exit code 0 and no WebGPU error.
#
# --push needs a clean working tree (the site is then that commit's build) and refuses otherwise. It checks the
# gh-pages branch out into a temporary worktree (an orphan branch the first time), replaces its content with the
# site (all but games/, where games publish their own pages: games/outpost/), commits and pushes: no history is
# rewritten, so every publication adds its files (about 22 MB before git's compression) to that branch. Pages is turned on once per repository: Settings, Pages, "Deploy from a branch",
# gh-pages, / (root); or
#   gh api -X POST repos/<owner>/<repo>/pages -f 'source[branch]=gh-pages' -f 'source[path]=/'
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SITE="$ROOT/dist/web"
BUILD=1
PUSH=0
while [[ $# -gt 0 ]]; do
    case "$1" in
    --no-build) BUILD=0 ;;
    --push) PUSH=1 ;;
    -h | --help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n=== %s\n' "$*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

cd "$ROOT"
VERSION="$(sed -n 's/^project(Quench VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
[[ -n "$VERSION" ]] || fail "no version in CMakeLists.txt"
HASH="$(git rev-parse --short HEAD)"
DIRTY=""
[[ -z "$(git status --porcelain)" ]] || DIRTY="-dirty"
if [[ $PUSH -eq 1 && -n "$DIRTY" ]]; then fail "--push needs a clean working tree (commit first)"; fi

if [[ $BUILD -eq 1 ]]; then
    step "web build (the web preset)"
    [[ -f build/host-tools/CMakeCache.txt ]] || cmake --preset host-tools >/dev/null
    cmake --build --preset host-tools
    [[ -f build/web/CMakeCache.txt ]] || emcmake cmake --preset web >/dev/null
    cmake --build --preset web
fi

step "site: dist/web"
DEMO=build/web/examples/demo
HELLO=build/web/examples/hello
for f in "$DEMO"/quench-demo.{html,js,wasm,data} "$HELLO"/hello.{html,js,wasm}; do
    [[ -f "$f" ]] || fail "$f is missing (build the web preset)"
done
rm -rf "$SITE"
mkdir -p "$SITE/licenses/emscripten"
cp "$DEMO/quench-demo.html" "$SITE/index.html"
cp "$DEMO"/quench-demo.{js,wasm,data} "$HELLO"/hello.{html,js,wasm} "$SITE/"
[[ ! -f "$HELLO/hello.data" ]] || cp "$HELLO/hello.data" "$SITE/"
cp LICENSE THIRD_PARTY_LICENSES "$SITE/"
# Emscripten's own licence and those of the system libraries it links (musl, libc++, libc++abi, compiler-rt, ...).
EM_ROOT="$(em-config EMSCRIPTEN_ROOT)"
EM_LICENSE="$EM_ROOT/LICENSE"
[[ -f "$EM_LICENSE" ]] || EM_LICENSE="$EM_ROOT/../LICENSE" # Homebrew keeps it beside libexec
[[ -f "$EM_LICENSE" ]] || fail "Emscripten's LICENSE was not found near $EM_ROOT"
cp "$EM_LICENSE" "$SITE/licenses/emscripten/LICENSE"
(cd "$EM_ROOT/system/lib" && find -L . -maxdepth 3 -type f \
    \( -iname 'LICENSE*' -o -iname 'COPYRIGHT*' -o -iname 'CREDITS*' -o -iname 'NOTICE*' \) | sort) |
    while read -r f; do
        mkdir -p "$SITE/licenses/emscripten/$(dirname "$f")"
        cp "$EM_ROOT/system/lib/$f" "$SITE/licenses/emscripten/$f"
    done
[[ -f "$SITE/licenses/emscripten/libc/musl/COPYRIGHT" ]] || fail "musl's COPYRIGHT was not found in $EM_ROOT"
printf 'Quench %s, commit %s%s\nhttps://github.com/m-brtn/Quench\n' "$VERSION" "$HASH" "$DIRTY" >"$SITE/version.txt"
touch "$SITE/.nojekyll"
du -sh "$SITE" | awk '{print "dist/web: " $1}'

step "check: the site's index.html in headless Chrome (no window)"
LOG="$ROOT/dist/web_check.log"
node tools/web_check.mjs --build "$SITE" --page index.html --out "$ROOT/dist/web_check" --timeout 300 --onscreen -- \
    --script /repo/examples/demo/scenarios/smoke.txt >"$LOG" 2>&1 || {
    tail -40 "$LOG"
    fail "the site's check (log: $LOG)"
}
grep -E "scenario: [^ ]*smoke.txt: |web_check: (passed|[0-9]+ failed)" "$LOG"

if [[ $PUSH -eq 0 ]]; then
    echo "dist/web is ready (not published: --push)"
    exit 0
fi

step "publish: the gh-pages branch"
WORK="$(mktemp -d)"
TEMP_BRANCH="quench-publish-web-$$" # a local name for the commit; the remote branch is gh-pages
cleanup() {
    git worktree remove --force "$WORK/site" >/dev/null 2>&1 || true
    git branch -D "$TEMP_BRANCH" >/dev/null 2>&1 || true
    rm -rf "$WORK"
}
trap cleanup EXIT
git fetch origin >/dev/null 2>&1 || fail "git fetch origin"
if git rev-parse --verify --quiet origin/gh-pages >/dev/null; then
    git worktree add -b "$TEMP_BRANCH" "$WORK/site" origin/gh-pages >/dev/null 2>&1
else
    git worktree add --detach "$WORK/site" >/dev/null 2>&1
    git -C "$WORK/site" checkout --orphan "$TEMP_BRANCH" >/dev/null 2>&1
fi
git -C "$WORK/site" rm -rf --quiet . >/dev/null 2>&1 || true
git -C "$WORK/site" clean -fdxq
# The pages of games published beside the demo (games/<name>/, by their own scripts) stay as they are.
if git cat-file -e origin/gh-pages:games 2>/dev/null; then
    git -C "$WORK/site" checkout --quiet origin/gh-pages -- games
fi
cp -R "$SITE"/. "$WORK/site/"
git -C "$WORK/site" add -A
if git -C "$WORK/site" rev-parse --verify --quiet HEAD >/dev/null && git -C "$WORK/site" diff --cached --quiet; then
    echo "gh-pages already holds this site"
else
    git -C "$WORK/site" commit --quiet -m "Publish the web demo $VERSION ($HASH)" \
        -m "Built by tools/publish_web.sh from commit $HASH (the web preset) and checked in headless Chrome on the Mac."
    git -C "$WORK/site" push origin "$TEMP_BRANCH:gh-pages"
    echo "published: branch gh-pages"
fi
