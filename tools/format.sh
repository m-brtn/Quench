#!/bin/sh
# Formats the engine's C and C++ sources with .clang-format; `--check` only reports files that differ (exit 1).
# Uses $CLANG_FORMAT if set, else clang-format from PATH, else the one from Xcode (xcrun).
set -eu
cd "$(dirname "$0")/.."

if [ -n "${CLANG_FORMAT:-}" ]; then
    clang_format=$CLANG_FORMAT
elif command -v clang-format >/dev/null 2>&1; then
    clang_format=clang-format
else
    clang_format="xcrun clang-format"
fi

case "${1:-}" in
--check) args="--dry-run --Werror" ;;
"") args="-i" ;;
*)
    echo "usage: tools/format.sh [--check]" >&2
    exit 2
    ;;
esac

files=$(git ls-files --cached --others --exclude-standard -- '*.h' '*.hpp' '*.c' '*.cpp' '*.m' '*.mm')
[ -n "$files" ] || exit 0
# shellcheck disable=SC2086
$clang_format $args $files
