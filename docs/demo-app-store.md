# Distributing the demo through the App Store

The App Store listing is **Quench — 3D Engine**. The bundle identifier is `org.quench.demo`.
The demo includes `PrivacyInfo.xcprivacy` and its [privacy policy](demo-privacy.md).
The development `.ipa` from `tools/package_demo.sh --ios` is not an App Store distribution package.

## Archive

Use an Apple Account with access to the app in Xcode and the owner's paid development team. Keep account,
certificate, team and device identifiers out of repository files. Run the demo gates before archiving.

```sh
cmake --preset host-tools
cmake --build --preset host-tools
cmake --preset ios-xcode -DQUENCH_IOS_TEAM="$QUENCH_IOS_TEAM" -DQUENCH_IOS_BUNDLE_ID_SUFFIX= \
    -DSDL_CAMERA=OFF -DSDL_HIDAPI=OFF -DQUENCH_DEMO_IOS_BUILD_NUMBER=0.12.2

xcodebuild -project build/ios-xcode/Quench.xcodeproj -scheme quench_demo \
    -configuration RelWithDebInfo -destination generic/platform=iOS \
    -archivePath build/app-store/QuenchDemo-0.12.1-build-0.12.2.xcarchive \
    -allowProvisioningUpdates archive
```

The demo does not use cameras or raw Bluetooth devices. Disabling these SDL subsystems removes their protected
API references instead of adding misleading permission descriptions. Apple's GameController driver remains
enabled (`SDL_JOYSTICK=ON`); the raw HIDAPI Bluetooth controller backend is excluded from this distribution.
These are local distribution settings, not new engine-wide defaults.

Increment `QUENCH_DEMO_IOS_BUILD_NUMBER` for each delivery. The public version remains the engine version;
the demo's generated plist declares that it contains no non-exempt encryption.

`RelWithDebInfo` supplies optimized code and debug information. Check that the archive contains the app, all
cooked assets, the privacy manifest and a dSYM whose UUID matches the executable. The archive's actual
`CFBundleVersion` must match the intended build number. Do not infer this from its filename.

With CMake's Xcode output directories, the generated dSYM can stay beside the build product instead of being
copied into the archive. If the archive's `dSYMs` directory is empty, compare the UUIDs with `xcrun dwarfdump
--uuid`, then copy the matching bundle from `build/ios-xcode/examples/demo/RelWithDebInfo-iphoneos/quench-demo.app.dSYM`
into the archive's `dSYMs/` directory before export. Never substitute symbols from a different build.

## Upload and submit

Distribute the archive through Xcode Organizer or `xcodebuild -exportArchive` with a local export-options plist:
`method=app-store-connect`, `destination=upload`, automatic signing, the paid team, `uploadSymbols=true`, and
`manageAppVersionAndBuildNumber=false` when using the explicit build number above. Store that plist under ignored
`build/app-store/`; it contains local signing information.

A successful upload is not a successful processing result. Check App Store Connect and Apple's delivery email
for validation failures, then select the processed build on the version page. Upload actual iPhone and iPad app
screenshots, complete the listing, privacy information, content rights, pricing and the owner's authorized
private App Review contact, and submit the version for review. A manual-release version remains unpublished
after approval until the owner releases it.

The first delivery on 2026-11-09 uploaded successfully but failed processing with `ITMS-90683` for SDL's unused
camera API; raw Bluetooth also produced a warning. It had no dSYM because the Release compilation lacked debug
information. The distribution settings above address those findings; the [devlog](../devlog/2026-11-09.md) records
the actual result of each delivery.
