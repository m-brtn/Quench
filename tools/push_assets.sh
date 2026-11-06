#!/usr/bin/env bash
# Copies changed asset files onto an iOS device, into the asset overlay of a development build of a game: the game
# reads them instead of its bundled ones, and with hot reload on it loads them again while it runs
# (docs/assets.md, "Hot reload on a device").
#
#   tools/push_assets.sh --device ID --bundle-id ID --app NAME [--org NAME] [--all] [--watch [SECONDS]] DIR
#
# DIR holds the game's assets as the bundle has them: in an Xcode build the merged folder
# build/ios-xcode/<game dir>/quench_game/<target>/assets, which building the <target>_assets target cooks and fills.
# --device is devicectl's device (UDID, name, ...); --bundle-id the app's; --app and --org the AppDesc name and
# organization (the user directory is "Library/Application Support/<org>/<app>" in the app's container; --org
# defaults to Quench). Files in DIR whose content differs from what was last pushed to that device and app are
# copied one by one (a list of hashes next to DIR); the first push copies nothing unless --all. --watch pushes every
# SECONDS (default 1) until stopped. devicectl takes about a second per file. Nothing is removed on the device:
# devicectl's --remove-existing-content deleted files outside the destination folder (docs/assets.md); to read the
# bundled assets again, push the original files or delete the app.
set -euo pipefail

DEVICE=""
BUNDLE=""
APP=""
ORG="Quench"
ALL=0
WATCH=""
DIR=""
while [[ $# -gt 0 ]]; do
    case "$1" in
    --device) DEVICE="${2:?--device ID}"; shift ;;
    --bundle-id) BUNDLE="${2:?--bundle-id ID}"; shift ;;
    --app) APP="${2:?--app NAME}"; shift ;;
    --org) ORG="${2?--org NAME}"; shift ;;
    --all) ALL=1 ;;
    --watch)
        WATCH=1
        if [[ $# -gt 1 && "$2" =~ ^[0-9]+([.][0-9]+)?$ ]]; then WATCH="$2"; shift; fi
        ;;
    -h | --help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) DIR="$1" ;;
    esac
    shift
done
fail() { echo "push_assets: $*" >&2; exit 1; }
[[ -n "$DEVICE" && -n "$BUNDLE" && -n "$APP" ]] || fail "--device, --bundle-id and --app are required (--help)"

USER_DIR="Library/Application Support/${ORG:+$ORG/}$APP"
OVERLAY="$USER_DIR/asset_overlay"
copy_file() {
    xcrun devicectl device copy to --device "$DEVICE" --domain-type appDataContainer --domain-identifier "$BUNDLE" \
        --quiet --source "$1" --destination "$2"
}

[[ -n "$DIR" && -d "$DIR" ]] || fail "no assets directory '$DIR' (--help)"
DIR="$(cd "$DIR" && pwd)"
KEY="$(printf '%s %s' "$DEVICE" "$BUNDLE" | shasum | cut -c1-12)"
# Next to DIR: the SHA-256 and path of every file the device has (from this script or the first run), and a stamp
# of the last scan. A build cooks files again with the same content and gives them a new time, so a file is copied
# when its content differs, and only files newer than the last scan are hashed.
MANIFEST="$(dirname "$DIR")/.push_assets.$KEY.sha256"
SCAN="$(dirname "$DIR")/.push_assets.$KEY.scan"

hash_line() { printf '%s  %s\n' "$(shasum -a 256 <"$DIR/$1" | cut -c1-64)" "$1"; }
candidates() {
    if [[ -f "$SCAN" ]]; then find "$DIR" -type f -newer "$SCAN" -print0; else find "$DIR" -type f -print0; fi
}

push() {
    local next="$SCAN.next" count=0 file rel line
    touch "$next"
    while IFS= read -r -d '' file; do
        rel="${file#"$DIR"/}"
        line="$(hash_line "$rel")"
        if grep -qxF -- "$line" "$MANIFEST"; then continue; fi
        copy_file "$file" "$OVERLAY/$rel" || fail "copy failed: $rel"
        awk -v p="$rel" 'substr($0, 67) != p' "$MANIFEST" >"$MANIFEST.next"
        echo "$line" >>"$MANIFEST.next"
        mv "$MANIFEST.next" "$MANIFEST"
        echo "pushed $rel"
        count=$((count + 1))
    done < <(candidates)
    mv "$next" "$SCAN"
    ((count == 0)) || echo "push_assets: $count file(s) to $OVERLAY ($(date +%H:%M:%S))"
}

if [[ ! -f "$MANIFEST" ]]; then
    rm -f "$SCAN"
    : >"$MANIFEST"
    if ((!ALL)); then
        touch "$SCAN.next"
        while IFS= read -r -d '' file; do hash_line "${file#"$DIR"/}" >>"$MANIFEST"; done < <(candidates)
        mv "$SCAN.next" "$SCAN"
        echo "push_assets: first push to this device and app: files that change from now on are copied" \
            "(--all copies every file)"
    fi
fi
if [[ -z "$WATCH" ]]; then
    push
    exit 0
fi
echo "push_assets: watching $DIR every ${WATCH} s (Ctrl-C stops)"
while true; do
    push
    sleep "$WATCH"
done
