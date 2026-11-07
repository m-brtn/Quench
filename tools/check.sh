#!/usr/bin/env bash
# The engine's gates (AGENTS.md, "Build and test"). Every app run is offscreen: no window on the screen, no input from
# real devices (docs/app.md, "Offscreen runs"); the tests that need a window on the screen are never run from here.
#
#   tools/check.sh quick [--preset NAME] [--base REF] [LABEL...]
#       A small change: format the changed C and C++ files (check only), an incremental build of the preset
#       (default: release, where the captures are compared), and ctest of the labels the changed files map to
#       (or of the LABELs given): core physics assets audio animation cook gpu app render hello demo, without the
#       variants on Vulkan through MoltenVK (label vulkan; full and release run them). The changed files are those
#       that differ from HEAD (from REF with --base) and the untracked ones. A change to a shared foundation (core,
#       gpu, app, the build) is reported: it needs the full gate.
#   tools/check.sh full         the steps format debug release asan tsan wasm ios new-game: the gate at the end of a
#                               stage and for a change to a shared foundation
#   tools/check.sh release      full, then the step package (tools/package_demo.sh, macOS): before a release
#   tools/check.sh step NAME... single steps, in the order given: format, debug, release, asan and tsan (build and
#                               ctest of the preset), wasm (wasm-node), ios (host-tools, ios-sim-release,
#                               ios-sim-tests), new-game (tools/new_game.py into a temporary directory), package
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
# The window tests (tests/gpu/WindowTests.cpp) show a window on the screen: they run only on explicit request.
unset QUENCH_WINDOW_TESTS

now() { perl -MTime::HiRes=time -e 'printf "%.1f", time'; }
START="$(now)"
step() { printf '\n=== %s\n' "$*"; }
fail() {
    echo "FAIL: $*" >&2
    exit 1
}
elapsed() { perl -e "printf '%.1f', $(now) - $1"; }

configure() { [[ -f "build/$1/CMakeCache.txt" ]] || cmake --preset "$1" >/dev/null; }
build_and_test() {
    local preset=$1 jobs=${2:-8} t
    t="$(now)"
    step "$preset: build"
    configure "$preset"
    cmake --build --preset "$preset"
    step "$preset: ctest"
    ctest --preset "$preset" -j"$jobs" --output-on-failure
    echo "$preset: $(elapsed "$t") s"
}

run_step() {
    case "$1" in
    format)
        step "format"
        tools/format.sh --check
        ;;
    debug | release | asan | tsan) build_and_test "$1" ;;
    wasm)
        local t
        t="$(now)"
        step "wasm-node (headless tests as WebAssembly under Node.js)"
        [[ -f build/host-tools/CMakeCache.txt ]] || cmake --preset host-tools >/dev/null
        cmake --build --preset host-tools
        [[ -f build/wasm-node/CMakeCache.txt ]] || emcmake cmake --preset wasm-node >/dev/null
        cmake --build --preset wasm-node
        ctest --preset wasm-node -j8 --output-on-failure
        echo "wasm-node: $(elapsed "$t") s"
        ;;
    ios)
        local t booted=0
        t="$(now)"
        step "iOS: host-tools, ios-sim-release, ios-sim-tests (iOS Simulator, booted without its app window)"
        configure host-tools
        cmake --build --preset host-tools
        configure ios-sim-release
        cmake --build --preset ios-sim-release
        if ! xcrun simctl list devices booted | grep -q Booted; then
            xcrun simctl boot "iPhone 18 Pro Max"
            booted=1
        fi
        configure ios-sim-tests
        cmake --build --preset ios-sim-tests
        local status=0
        ctest --preset ios-sim-tests -j4 --output-on-failure || status=$?
        [[ $booted -eq 0 ]] || xcrun simctl shutdown "iPhone 18 Pro Max"
        [[ $status -eq 0 ]] || fail "ios-sim-tests"
        echo "ios: $(elapsed "$t") s"
        ;;
    new-game)
        local t dir
        t="$(now)"
        step "new game from templates/game, built against this checkout and started once; the quench-game skill"
        python3 tools/check_skill.py || fail "check_skill.py: update .agents/skills/quench-game"
        dir="$(mktemp -d -t quench_new_game)"
        local status=0 copied=0
        python3 tools/new_game.py "Check Game" "$dir/check-game" || status=$?
        diff -r .agents/skills/quench-game "$dir/check-game/.agents/skills/quench-game" || copied=$?
        rm -rf "$dir"
        [[ $status -eq 0 ]] || fail "new_game.py"
        [[ $copied -eq 0 ]] || fail "the new game's copy of .agents/skills/quench-game"
        echo "new-game: $(elapsed "$t") s"
        ;;
    package)
        step "demo package (macOS)"
        # After the release step the demo's checks have passed on this build.
        if [[ " ${STEPS[*]} " == *" release "* ]]; then
            tools/package_demo.sh --no-checks
        else
            tools/package_demo.sh
        fi
        ;;
    *) fail "unknown step: $1 (format, debug, release, asan, tsan, wasm, ios, new-game, package)" ;;
    esac
}

# Labels of the tests a changed file can affect; "shared" marks the foundations that need the full gate.
labels_for() {
    case "$1" in
    src/core/* | include/quench/core/* | tests/core/*) echo "core shared" ;;
    src/physics/* | include/quench/physics/* | tests/physics/*) echo "physics" ;;
    src/assets/* | include/quench/assets/* | tests/assets/*) echo "assets" ;;
    src/audio/* | include/quench/audio/* | tests/audio/*) echo "audio" ;;
    src/animation/* | include/quench/animation/* | tests/animation/*) echo "animation" ;;
    tools/cook/* | tests/cook/* | tests/CookCli.cmake | cmake/CookToolchain.cmake | cmake/QuenchCook.cmake) echo "cook" ;;
    src/gpu/* | include/quench/gpu/* | tests/gpu/*) echo "gpu render shared" ;;
    src/app/* | include/quench/app/* | tests/app/* | tests/harness/*) echo "app hello demo shared" ;;
    src/render/* | include/quench/render/* | tests/render/* | shaders/*) echo "render demo" ;;
    examples/demo/* | tools/assets/* | tests/golden/*/demo_* | tests/DemoUserFiles.cmake) echo "demo" ;;
    examples/hello/* | tests/golden/*/hello*) echo "hello" ;;
    tests/RunBatch.cmake | tests/CheckPart.cmake | tools/imagecompare/*) echo "hello demo" ;;
    CMakeLists.txt | CMakePresets.json | cmake/* | tests/CMakeLists.txt | tests/DiscoverTests.cmake | tools/shaderc/*)
        echo "shared"
        ;;
    *) echo "" ;;
    esac
}

quick() {
    local preset=release base=HEAD
    local -a given=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
        --preset) preset="${2:?--preset NAME}"; shift ;;
        --base) base="${2:?--base REF}"; shift ;;
        -*) fail "unknown option: $1" ;;
        *) given+=("$1") ;;
        esac
        shift
    done

    local -a changed=()
    while IFS= read -r f; do [[ -n "$f" ]] && changed+=("$f"); done < <(
        git diff --name-only "$base" --
        git ls-files --others --exclude-standard
    )
    echo "changed files (against $base): ${#changed[@]}"

    local labels="" shared=0 f l
    for f in ${changed[@]+"${changed[@]}"}; do
        for l in $(labels_for "$f"); do
            if [[ $l == shared ]]; then
                shared=1
            elif [[ " $labels " != *" $l "* ]]; then
                labels="$labels $l"
            fi
        done
    done
    if [[ ${#given[@]} -gt 0 ]]; then labels=" ${given[*]}"; fi

    step "format (changed files)"
    local -a sources=()
    for f in ${changed[@]+"${changed[@]}"}; do
        [[ -f "$f" && "$f" =~ \.(h|hpp|c|cpp|m|mm)$ ]] && sources+=("$f")
    done
    if [[ ${#sources[@]} -gt 0 ]]; then
        if command -v clang-format >/dev/null 2>&1; then
            clang-format --dry-run --Werror "${sources[@]}"
        else
            xcrun clang-format --dry-run --Werror "${sources[@]}"
        fi
        echo "${#sources[@]} files formatted"
    else
        echo "no C or C++ files changed"
    fi

    if [[ -z "$labels" ]]; then
        echo "no tests map to the changed files"
    else
        step "$preset: build"
        configure "$preset"
        cmake --build --preset "$preset"
        local regex
        regex="^($(echo $labels | tr ' ' '|'))\$"
        step "$preset: ctest -L '$regex' -LE '^vulkan\$'"
        ctest --preset "$preset" -L "$regex" -LE '^vulkan$' -j8 --output-on-failure
    fi
    if [[ $shared -eq 1 ]]; then
        echo
        echo "NOTE: the change touches a shared foundation (core, gpu, app or the build): run tools/check.sh full"
        echo "before committing it."
    fi
}

usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; }
STEPS=()
case "${1:-}" in
-h | --help) usage; exit 0 ;;
quick) shift; quick "$@" ;;
full | release)
    [[ $# -eq 1 ]] || fail "$1 takes no arguments"
    STEPS=(format debug release asan tsan wasm ios new-game)
    [[ $1 == full ]] || STEPS+=(package)
    ;;
step)
    shift
    [[ $# -gt 0 ]] || fail "step: name at least one step"
    STEPS=("$@")
    ;;
*) usage >&2; exit 2 ;;
esac
for s in ${STEPS[@]+"${STEPS[@]}"}; do run_step "$s"; done
echo
echo "passed: $(elapsed "$START") s"
