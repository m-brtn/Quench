#!/usr/bin/env bash
# Builds the engine demo (examples/demo) in Release and packs it into dist/: the build that ends every stage
# (AGENTS.md, "Build and test").
#
#   tools/package_demo.sh                # macOS: dist/QuenchDemo-<version>-<hash>-macos.zip
#   tools/package_demo.sh --ios          # also iOS: dist/QuenchDemo-<version>-<hash>-ios.zip (development signed)
#   tools/package_demo.sh --ios --team ID  # the team that signs it (default: $QUENCH_IOS_TEAM, else build/ios-xcode's)
#   tools/package_demo.sh --preset NAME  # the macOS configure and build preset (default: release)
#   tools/package_demo.sh --no-checks    # skip the demo's CTest checks (tools/check.sh release ran them on this build)
#
# macOS: build quench_demo with the preset, run the demo's CTest checks (ctest -L demo: one batch run per GPU
# backend and the runs with files of their own; docs/demo.md, "Checks"), assemble QuenchDemo.app (the executable, an
# Info.plist, an icns made from examples/demo/icon/icon.png, the licences, the assets), check its version, minimum
# macOS and dynamic dependencies (system libraries only), sign it ad hoc and zip it together with LICENSE and
# THIRD_PARTY_LICENSES. Then the app is started from a fresh unzip with examples/demo/scenarios/smoke.txt (offscreen,
# like every scripted run: no window on the screen, no sound): exit code 0 and a capture, left in dist/ to be looked
# at.
# iOS: the ios-xcode preset, configured with the team (an Apple ID signed in to Xcode), builds quench_demo with
# Automatic signing; the signed app is packed as an .ipa. Nothing is installed on a device. A free Personal Team is
# refused (Xcode's account data marks it): its profiles expire after 7 days (docs/app.md, "iOS runs").
# <hash> is the short commit hash, with "-dirty" when the working tree has changes (the build is then not that commit).
# The version comes from project(Quench VERSION x.y.z) in CMakeLists.txt.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"
PRESET=release
IOS=0
CHECKS=1
TEAM="${QUENCH_IOS_TEAM:-}"
while [[ $# -gt 0 ]]; do
    case "$1" in
    --ios) IOS=1 ;;
    --team) TEAM="${2:?--team ID}"; shift ;;
    --preset) PRESET="${2:?--preset NAME}"; shift ;;
    --no-checks) CHECKS=0 ;;
    -h | --help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n=== %s\n' "$*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }
# Without --norsrc --noextattr, ditto stores extended attributes (com.apple.provenance is on every file here) as
# AppleDouble ._ files in the zip; a plain unzip turns them into files inside the bundle and breaks its signature.
zip_dir() { ditto -c -k --norsrc --noextattr --noqtn --keepParent "$1" "$2"; }
check_zip() {
    if zipinfo -1 "$1" | grep -E '(^|/)\._'; then fail "AppleDouble ._ files in $1"; fi
}

VERSION="$(sed -n 's/^project(Quench VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
[[ -n "$VERSION" ]] || fail "cannot read the version from CMakeLists.txt"
HASH="$(git -C "$ROOT" rev-parse --short HEAD)"
[[ -z "$(git -C "$ROOT" status --porcelain)" ]] || HASH="$HASH-dirty"
NAME="QuenchDemo-$VERSION-$HASH"
BUILD="$ROOT/build/$PRESET"
EXE="$BUILD/examples/demo/quench-demo"

step "Quench Demo $VERSION ($HASH): $PRESET build"
cmake --preset "$PRESET" >/dev/null
cmake --build --preset "$PRESET" --target quench_demo quench_image_compare
[[ "$("$EXE" --version)" == "Quench Demo $VERSION" ]] || fail "binary version mismatch"

if [[ $CHECKS -eq 1 ]]; then
    step "demo checks (ctest -L demo)"
    (cd "$ROOT" && ctest --preset "$PRESET" -L '^demo$' -j8 --output-on-failure)
fi

step "assemble QuenchDemo.app"
STAGE="$DIST/$NAME-macos"
APP="$STAGE/QuenchDemo.app"
ZIP="$DIST/$NAME-macos.zip"
rm -rf "$STAGE" "$ZIP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$EXE" "$APP/Contents/MacOS/quench-demo"
# The asset directory (plain and cooked) goes to Resources: SDL_GetBasePath() of a bundled app.
cp -R "$(dirname "$EXE")/assets" "$APP/Contents/Resources/assets"
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_LICENSES" "$APP/Contents/Resources/"
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_LICENSES" "$STAGE/"

ICONSET="$(mktemp -d -t quench_icon)/QuenchDemo.iconset"
mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
    sips -s format png -z $size $size "$ROOT/examples/demo/icon/icon.png" --out "$ICONSET/icon_${size}x${size}.png" \
        >/dev/null
    sips -s format png -z $((size * 2)) $((size * 2)) "$ROOT/examples/demo/icon/icon.png" \
        --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/QuenchDemo.icns"
rm -rf "$(dirname "$ICONSET")"

# LaunchServices refuses an app whose LSMinimumSystemVersion is above the running macOS, so it is the binary's minos.
MIN_BINARY="$(vtool -show-build "$APP/Contents/MacOS/quench-demo" | awk '$1 == "minos" { print $2; exit }')"
[[ -n "$MIN_BINARY" ]] || fail "cannot read the binary minos"
cat >"$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleDisplayName</key><string>Quench Demo</string>
    <key>CFBundleExecutable</key><string>quench-demo</string>
    <key>CFBundleIconFile</key><string>QuenchDemo</string>
    <key>CFBundleIdentifier</key><string>org.quench.demo</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>Quench Demo</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSMinimumSystemVersion</key><string>$MIN_BINARY</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSHumanReadableCopyright</key><string>Quench engine demo, MIT licence (Contents/Resources/LICENSE)</string>
</dict>
</plist>
EOF
plutil -lint "$APP/Contents/Info.plist" >/dev/null || fail "Info.plist is not valid"
[[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP/Contents/Info.plist")" == "$VERSION" ]] ||
    fail "Info.plist version mismatch"
ver_le() { [[ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -n 1)" == "$1" ]]; }
HOST_MACOS="$(sw_vers -productVersion)"
echo "LSMinimumSystemVersion $MIN_BINARY (the binary's minos), host macOS $HOST_MACOS"
ver_le "$MIN_BINARY" "$HOST_MACOS" || fail "the binary needs macOS $MIN_BINARY, this Mac runs $HOST_MACOS"
cmp -s "$EXE" "$APP/Contents/MacOS/quench-demo" || fail "bundle executable differs from the build"

step "dynamic dependencies (only system libraries expected)"
otool -L "$APP/Contents/MacOS/quench-demo"
if otool -L "$APP/Contents/MacOS/quench-demo" | tail -n +2 | awk '{print $1}' | grep -vE '^(/System/Library/|/usr/lib/)'; then
    fail "non-system dynamic dependency"
fi

step "ad hoc codesign"
codesign --force --deep -s - "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"

step "zip"
zip_dir "$STAGE" "$ZIP"
check_zip "$ZIP"
ls -l "$ZIP"
shasum -a 256 "$ZIP"

step "smoke run from a fresh unzip"
UNZIP="$(mktemp -d -t quench_demo_unzip)"
unzip -q "$ZIP" -d "$UNZIP"
UNZIPPED="$UNZIP/$NAME-macos"
for f in QuenchDemo.app LICENSE THIRD_PARTY_LICENSES QuenchDemo.app/Contents/Resources/QuenchDemo.icns \
         QuenchDemo.app/Contents/Resources/LICENSE QuenchDemo.app/Contents/Resources/THIRD_PARTY_LICENSES \
         QuenchDemo.app/Contents/Resources/assets/models/crate.qmodel \
         QuenchDemo.app/Contents/Resources/assets/sounds/music_a.wav; do
    [[ -e "$UNZIPPED/$f" ]] || fail "$f missing in the archive"
done
codesign --verify --deep --strict "$UNZIPPED/QuenchDemo.app"
CAPTURE="$DIST/$NAME-macos-smoke.png"
LOG="$DIST/$NAME-macos-smoke.log"
rm -f "$CAPTURE"
(cd / && "$UNZIPPED/QuenchDemo.app/Contents/MacOS/quench-demo" --window 960x540 --frame-size 960x540 \
    --script "$ROOT/examples/demo/scenarios/smoke.txt" --capture "$CAPTURE" --capture-at 1.5) >"$LOG" 2>&1 ||
    { cat "$LOG"; fail "smoke run failed (log: $LOG)"; }
grep -E "scenario: .*: [0-9]+ passed, 0 failed" "$LOG" || { cat "$LOG"; fail "smoke scenario did not pass"; }
[[ -s "$CAPTURE" ]] || fail "smoke run wrote no capture"
rm -rf "$UNZIP"
echo "capture (look at it): $CAPTURE"

if [[ $IOS -eq 1 ]]; then
    step "iOS: ios-xcode (Automatic signing), quench_demo"
    XBUILD="$ROOT/build/ios-xcode"
    if [[ -z "$TEAM" && -f "$XBUILD/CMakeCache.txt" ]]; then
        TEAM="$(sed -n 's/^QUENCH_IOS_TEAM:STRING=//p' "$XBUILD/CMakeCache.txt")"
    fi
    [[ -n "$TEAM" ]] || fail "no team: --team ID, QUENCH_IOS_TEAM, or cmake --preset ios-xcode -DQUENCH_IOS_TEAM=<team>"
    # Xcode's accounts list every team with isFreeProvisioningTeam. The list can be empty while Xcode still signs
    # with the account (seen 2026-11-06): then only the signed app's profile tells (below).
    TEAMS="$(defaults read com.apple.dt.Xcode IDEProvisioningTeamByIdentifier 2>/dev/null || true)"
    TEAM_ENTRY="$(grep -B1 -A2 "teamID = $TEAM;" <<<"$TEAMS" || true)"
    if [[ -n "$TEAM_ENTRY" ]]; then
        grep -q "isFreeProvisioningTeam = 1" <<<"$TEAM_ENTRY" &&
            fail "team $TEAM is a free Personal Team (profiles expire after 7 days): use a paid team"
        echo "team $TEAM: $(sed -n 's/.*teamName = "\{0,1\}\([^";]*\)"\{0,1\};/\1/p' <<<"$TEAM_ENTRY")"
    else
        echo "team $TEAM: not in Xcode's team list (Xcode > Settings > Accounts); the app's profile is checked instead"
    fi
    cmake --preset host-tools >/dev/null
    cmake --build --preset host-tools
    cmake --preset ios-xcode -DQUENCH_IOS_TEAM="$TEAM" >/dev/null
    # The assets merged in a build of their own (once needed, not reproduced since; docs/app.md), compared after.
    xcodebuild -project "$XBUILD/Quench.xcodeproj" -target quench_demo_assets -configuration Release \
        -sdk iphoneos -quiet build
    xcodebuild -project "$XBUILD/Quench.xcodeproj" -scheme quench_demo -configuration Release \
        -destination generic/platform=iOS -allowProvisioningUpdates -quiet build
    IOS_APP="$XBUILD/examples/demo/Release-iphoneos/quench-demo.app"
    [[ -d "$IOS_APP" ]] || fail "no $IOS_APP"
    diff -r "$XBUILD/examples/demo/quench_game/quench_demo/assets" "$IOS_APP/assets" ||
        fail "the iOS app's assets/ differs from the staged assets"
    codesign --verify --deep --strict "$IOS_APP"
    SIGNATURE="$(codesign -dvv "$IOS_APP" 2>&1)"
    grep -E "^(Identifier|Authority|TeamIdentifier)=" <<<"$SIGNATURE" | grep -v "Certification Authority"
    grep -qx "TeamIdentifier=$TEAM" <<<"$SIGNATURE" || fail "the iOS app is not signed by team $TEAM"
    [[ -f "$IOS_APP/embedded.mobileprovision" ]] || fail "the iOS app has no provisioning profile"
    security cms -D -i "$IOS_APP/embedded.mobileprovision" >"$DIST/.profile.plist"
    echo "profile: $(/usr/libexec/PlistBuddy -c 'Print :Name' "$DIST/.profile.plist"), expires" \
        "$(/usr/libexec/PlistBuddy -c 'Print :ExpirationDate' "$DIST/.profile.plist"), devices:" \
        "$(/usr/libexec/PlistBuddy -c 'Print :ProvisionedDevices' "$DIST/.profile.plist" | grep -c -- '-')"
    # A free Personal Team's profiles last 7 days; a paid team's a year.
    profile_time() {
        date -j -u -f "%Y-%m-%dT%H:%M:%SZ" "$(plutil -extract "$1" raw -o - "$DIST/.profile.plist")" +%s
    }
    PROFILE_DAYS=$((($(profile_time ExpirationDate) - $(profile_time CreationDate)) / 86400))
    rm -f "$DIST/.profile.plist"
    ((PROFILE_DAYS > 7)) ||
        fail "the profile lasts $PROFILE_DAYS days: team $TEAM is a free Personal Team, use a paid team"

    ISTAGE="$DIST/$NAME-ios"
    IZIP="$DIST/$NAME-ios.zip"
    rm -rf "$ISTAGE" "$IZIP"
    mkdir -p "$ISTAGE/Payload"
    ditto "$IOS_APP" "$ISTAGE/Payload/quench-demo.app"
    (cd "$ISTAGE" && zip_dir Payload QuenchDemo.ipa && rm -rf Payload)
    check_zip "$ISTAGE/QuenchDemo.ipa"
    IUNZIP="$(mktemp -d -t quench_demo_ipa)"
    unzip -q "$ISTAGE/QuenchDemo.ipa" -d "$IUNZIP"
    codesign --verify --deep --strict "$IUNZIP/Payload/quench-demo.app"
    rm -rf "$IUNZIP"
    cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_LICENSES" "$ISTAGE/"
    zip_dir "$ISTAGE" "$IZIP"
    check_zip "$IZIP"
    ls -l "$IZIP"
    shasum -a 256 "$IZIP"
fi

step "done"
ls -l "$DIST"/"$NAME"-*.zip
