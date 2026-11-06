# Game executables: quench_add_game(). The engine owns main (quench_main, SDL main callbacks); the game defines
# quench::app::createGame() (include/quench/app/App.h).
#
#   macOS: a plain executable; ASSETS and the output of COOKED_ASSETS are copied to assets/ next to it
#          (SDL_GetBasePath() is the executable's directory). Run it from the build tree.
#   iOS, Ninja (Simulator and device): <OUTPUT_NAME>.app with Info.plist (cmake/Info.ios.plist.in), the icon compiled
#          by actool, the assets in assets/ at the bundle root, signed with QUENCH_IOS_SIGN_IDENTITY ("-" = ad-hoc,
#          enough for the Simulator).
#   iOS, Xcode generator (device): Xcode bundles and signs (Automatic signing with QUENCH_IOS_TEAM).

set(QUENCH_IOS_SIGN_IDENTITY "-" CACHE STRING "codesign identity for iOS bundles built with Ninja (- = ad-hoc)")
set(QUENCH_IOS_TEAM "" CACHE STRING "Apple development team id for Xcode Automatic signing (empty: choose in Xcode)")
if(CMAKE_GENERATOR STREQUAL "Xcode")
    # Bundle ids are global per Apple account: a free Personal Team cannot register one another account uses.
    string(REGEX REPLACE "[^A-Za-z0-9-]" "-" _quench_user "$ENV{USER}")
    set(_quench_bundle_suffix ".${_quench_user}")
else()
    set(_quench_bundle_suffix "")
endif()
set(QUENCH_IOS_BUNDLE_ID_SUFFIX "${_quench_bundle_suffix}" CACHE STRING "Appended to every iOS bundle id")

# quench_add_game(<target> NAME <display name> SOURCES <file>...
#                 [OUTPUT_NAME <file name>] [BUNDLE_ID <reverse dns>] [VERSION <x.y.z>]
#                 [ASSETS <dir>] [COOKED_ASSETS <quench_cook_assets name>...]
#                 [ICON <square png, 1024 px or larger>] [ORIENTATIONS landscape|portrait|all] [CUSTOM_MAIN])
# ASSETS is copied as it is; COOKED_ASSETS (cmake/QuenchCook.cmake) are cooked first and merged into the same assets/.
# CUSTOM_MAIN: the target defines main itself instead of linking quench_main (the device test app).
# The sources get QUENCH_GAME_NAME and QUENCH_GAME_VERSION (string literals) for AppDesc.
function(quench_add_game target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "CUSTOM_MAIN" "NAME;OUTPUT_NAME;BUNDLE_ID;VERSION;ASSETS;ICON;ORIENTATIONS"
                          "SOURCES;COOKED_ASSETS")
    if(NOT arg_NAME OR NOT arg_SOURCES)
        message(FATAL_ERROR "quench_add_game: NAME and SOURCES are required")
    endif()
    if(NOT arg_OUTPUT_NAME)
        set(arg_OUTPUT_NAME ${target})
    endif()
    if(NOT arg_BUNDLE_ID)
        string(MAKE_C_IDENTIFIER ${target} _id)
        string(REPLACE "_" "-" _id ${_id})
        set(arg_BUNDLE_ID "org.quench.${_id}")
    endif()
    if(NOT arg_VERSION)
        set(arg_VERSION ${PROJECT_VERSION})
    endif()
    if(NOT arg_ORIENTATIONS)
        set(arg_ORIENTATIONS landscape)
    endif()
    if(NOT arg_ORIENTATIONS MATCHES "^(landscape|portrait|all)$")
        message(FATAL_ERROR "quench_add_game: ORIENTATIONS must be landscape, portrait or all")
    endif()
    if(arg_ASSETS)
        get_filename_component(arg_ASSETS ${arg_ASSETS} ABSOLUTE)
    endif()
    if(arg_ICON)
        get_filename_component(arg_ICON ${arg_ICON} ABSOLUTE)
    endif()
    set(_cooked_dirs "")
    foreach(_cook IN LISTS arg_COOKED_ASSETS)
        if(NOT TARGET ${_cook})
            message(FATAL_ERROR "quench_add_game: COOKED_ASSETS '${_cook}' is not a quench_cook_assets() target")
        endif()
        get_target_property(_dir ${_cook} QUENCH_COOK_OUTPUT_DIR)
        if(NOT _dir)
            message(FATAL_ERROR "quench_add_game: COOKED_ASSETS '${_cook}' is not a quench_cook_assets() target")
        endif()
        list(APPEND _cooked_dirs ${_dir})
    endforeach()
    # The asset directories in copy order: cooked files win over plain ones of the same name.
    set(_asset_dirs ${arg_ASSETS} ${_cooked_dirs})

    add_executable(${target} ${arg_SOURCES})
    if(arg_CUSTOM_MAIN)
        target_link_libraries(${target} PRIVATE quench_app)
    else()
        target_link_libraries(${target} PRIVATE quench_main)
    endif()
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${arg_OUTPUT_NAME})
    target_compile_definitions(${target} PRIVATE QUENCH_GAME_NAME="${arg_NAME}" QUENCH_GAME_VERSION="${arg_VERSION}")

    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        if(_asset_dirs)
            set(_copies "")
            foreach(_dir IN LISTS _asset_dirs)
                list(APPEND _copies COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${_dir}
                     $<TARGET_FILE_DIR:${target}>/assets)
            endforeach()
            add_custom_target(${target}_assets ALL ${_copies} COMMENT "Copying the assets of ${target}" VERBATIM)
            add_dependencies(${target} ${target}_assets)
            if(arg_COOKED_ASSETS)
                add_dependencies(${target}_assets ${arg_COOKED_ASSETS})
            endif()
        endif()
        return()
    endif()

    # ---------------------------------------------------------------- iOS
    set(QUENCH_GAME_NAME ${arg_NAME})
    set(QUENCH_GAME_EXECUTABLE ${arg_OUTPUT_NAME})
    set(QUENCH_GAME_VERSION ${arg_VERSION})
    set(QUENCH_GAME_MINIMUM_OS ${CMAKE_OSX_DEPLOYMENT_TARGET})
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
        set(QUENCH_GAME_PLATFORM iPhoneSimulator)
        set(_sdk iphonesimulator)
    else()
        set(QUENCH_GAME_PLATFORM iPhoneOS)
        set(_sdk iphoneos)
    endif()
    set(_landscape "<string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string>")
    set(_portrait "<string>UIInterfaceOrientationPortrait</string>")
    # An iPad app without UIRequiresFullScreen must support all four orientations; the key is deprecated since iOS 26
    # but still the only way to keep one orientation on an iPad.
    set(QUENCH_GAME_FULL_SCREEN "<key>UIRequiresFullScreen</key><true/>")
    if(arg_ORIENTATIONS STREQUAL "landscape")
        set(QUENCH_GAME_ORIENTATIONS "${_landscape}")
        set(QUENCH_GAME_ORIENTATIONS_IPAD "${_landscape}")
    elseif(arg_ORIENTATIONS STREQUAL "portrait")
        set(QUENCH_GAME_ORIENTATIONS "${_portrait}")
        set(QUENCH_GAME_ORIENTATIONS_IPAD "${_portrait}")
    else()
        set(QUENCH_GAME_ORIENTATIONS "${_portrait}${_landscape}")
        set(QUENCH_GAME_ORIENTATIONS_IPAD
            "${_portrait}<string>UIInterfaceOrientationPortraitUpsideDown</string>${_landscape}")
        set(QUENCH_GAME_FULL_SCREEN "")
    endif()
    set(QUENCH_GAME_ICONS "")
    if(arg_ICON)
        set(QUENCH_GAME_ICONS "<key>CFBundleIcons</key><dict><key>CFBundlePrimaryIcon</key><dict>\
<key>CFBundleIconFiles</key><array><string>AppIcon60x60</string></array>\
<key>CFBundleIconName</key><string>AppIcon</string></dict></dict>\
<key>CFBundleIcons~ipad</key><dict><key>CFBundlePrimaryIcon</key><dict>\
<key>CFBundleIconFiles</key><array><string>AppIcon60x60</string><string>AppIcon76x76</string></array>\
<key>CFBundleIconName</key><string>AppIcon</string></dict></dict>")
    endif()

    set(_gen ${CMAKE_CURRENT_BINARY_DIR}/quench_game/${target})
    file(MAKE_DIRECTORY ${_gen})
    if(arg_ICON)
        # One 1024 px image in an asset catalog; actool writes Assets.car and the AppIcon*.png files.
        set(_xcassets ${_gen}/Assets.xcassets)
        file(WRITE ${_xcassets}/Contents.json "{\"info\":{\"author\":\"xcode\",\"version\":1}}\n")
        file(WRITE ${_xcassets}/AppIcon.appiconset/Contents.json
             "{\"images\":[{\"filename\":\"icon_1024.png\",\"idiom\":\"universal\",\"platform\":\"ios\","
             "\"size\":\"1024x1024\"}],\"info\":{\"author\":\"xcode\",\"version\":1}}\n")
    endif()

    if(CMAKE_GENERATOR STREQUAL "Xcode")
        set(QUENCH_GAME_BUNDLE_ID "$(PRODUCT_BUNDLE_IDENTIFIER)")
        set(QUENCH_GAME_DEVICE_FAMILY "") # from TARGETED_DEVICE_FAMILY
        configure_file(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Info.ios.plist.in ${_gen}/Info.plist @ONLY)
        set(_resources "")
        if(arg_ICON)
            set(_icon ${_xcassets}/AppIcon.appiconset/icon_1024.png)
            if(NOT EXISTS ${_icon} OR ${arg_ICON} IS_NEWER_THAN ${_icon})
                execute_process(COMMAND sips -s format png -z 1024 1024 ${arg_ICON} --out ${_icon}
                                OUTPUT_QUIET RESULT_VARIABLE _sips)
                if(NOT _sips EQUAL 0)
                    message(FATAL_ERROR "quench_add_game: sips could not convert ${arg_ICON}")
                endif()
            endif()
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${arg_ICON})
            list(APPEND _resources ${_xcassets})
        endif()
        if(_asset_dirs)
            # One folder reference named assets/ (Xcode copies it to the bundle root and signs it): the plain and the
            # cooked assets are merged into it first, and the folder is touched when a file in it changed
            # (StageAssets.cmake). Xcode decides whether to copy the folder when a build starts, before any target of
            # it runs: a change merged by a build reaches the bundle with the next one, or with this one when
            # <target>_assets is built first (docs/app.md, "iOS runs"; tools/package_demo.sh).
            set(_stage ${_gen}/assets)
            file(MAKE_DIRECTORY ${_stage})
            string(REPLACE ";" "|" _sources "${_asset_dirs}")
            add_custom_target(${target}_assets
                COMMAND ${CMAKE_COMMAND} -DSTAGE=${_stage} -DSOURCES=${_sources}
                        -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StageAssets.cmake
                COMMENT "Merging the assets of ${target}" VERBATIM)
            if(arg_COOKED_ASSETS)
                add_dependencies(${target}_assets ${arg_COOKED_ASSETS})
            endif()
            add_dependencies(${target} ${target}_assets)
            list(APPEND _resources ${_stage})
        endif()
        if(_resources)
            target_sources(${target} PRIVATE ${_resources})
            set_source_files_properties(${_resources} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        endif()
        set_target_properties(${target} PROPERTIES
            MACOSX_BUNDLE ON
            MACOSX_BUNDLE_INFO_PLIST ${_gen}/Info.plist
            RESOURCE "${_resources}"
            XCODE_GENERATE_SCHEME ON
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${arg_BUNDLE_ID}${QUENCH_IOS_BUNDLE_ID_SUFFIX}"
            XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
            XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
            XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${QUENCH_IOS_TEAM}"
            XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET ${CMAKE_OSX_DEPLOYMENT_TARGET}
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
            XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS iphoneos
            XCODE_ATTRIBUTE_SUPPORTS_MACCATALYST NO
            XCODE_ATTRIBUTE_SUPPORTS_MAC_DESIGNED_FOR_IPHONE_IPAD NO
            XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon
            XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym")
        return()
    endif()

    set(QUENCH_GAME_BUNDLE_ID "${arg_BUNDLE_ID}${QUENCH_IOS_BUNDLE_ID_SUFFIX}")
    set(QUENCH_GAME_DEVICE_FAMILY "<key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>")
    configure_file(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Info.ios.plist.in ${_gen}/Info.plist @ONLY)
    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_INFO_PLIST ${_gen}/Info.plist)
    set(_app $<TARGET_BUNDLE_DIR:${target}>)

    set(_bundle_steps "")
    set(_bundle_depends "")
    if(arg_ICON)
        set(_icon_out ${_gen}/actool)
        add_custom_command(
            OUTPUT ${_icon_out}/Assets.car
            COMMAND ${CMAKE_COMMAND} -E rm -rf ${_icon_out}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_icon_out}
            COMMAND sips -s format png -z 1024 1024 ${arg_ICON} --out ${_xcassets}/AppIcon.appiconset/icon_1024.png
            COMMAND xcrun actool ${_xcassets} --compile ${_icon_out} --platform ${_sdk}
                    --minimum-deployment-target ${CMAKE_OSX_DEPLOYMENT_TARGET} --app-icon AppIcon
                    --target-device iphone --target-device ipad --output-partial-info-plist ${_gen}/actool.plist
            DEPENDS ${arg_ICON}
            COMMENT "Compiling the app icon of ${target} (actool)"
            VERBATIM)
        list(APPEND _bundle_depends ${_icon_out}/Assets.car)
        list(APPEND _bundle_steps COMMAND ${CMAKE_COMMAND} -E copy ${_icon_out}/Assets.car
             ${_icon_out}/AppIcon60x60@2x.png ${_icon_out}/AppIcon76x76@2x~ipad.png ${_app}/)
    endif()
    foreach(_dir IN LISTS _asset_dirs)
        list(APPEND _bundle_steps COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${_dir} ${_app}/assets)
    endforeach()
    if(arg_COOKED_ASSETS)
        add_dependencies(${target} ${arg_COOKED_ASSETS})
    endif()
    # Runs on every build: the signature covers the executable and every resource.
    add_custom_target(${target}_bundle ALL
        ${_bundle_steps}
        COMMAND codesign --force --sign ${QUENCH_IOS_SIGN_IDENTITY} --timestamp=none ${_app}
        DEPENDS ${_bundle_depends}
        COMMENT "Bundling and signing ${arg_OUTPUT_NAME}.app (${QUENCH_IOS_SIGN_IDENTITY})"
        VERBATIM)
    add_dependencies(${target}_bundle ${target})
endfunction()
