# Merges asset directories into one folder (quench_add_game's Xcode build): cmake -DSTAGE=<dir>
# -DSOURCES=<dir>|<dir>... -P StageAssets.cmake. With -DXCODE_BUNDLE=ON instead of STAGE, in a script phase of an
# app target, the folder is the bundle's assets/, from the build settings Xcode passes to the phase. A file counts as
# changed when it is missing in the folder or differs from it in size or content, and is copied (file(COPY) keeps the
# source's time); one that differs only in its modification time (cooked again with the same result) gets the
# source's time. Files removed from a source stay in the folder, as copy_directory_if_different leaves them. The
# folder is touched whenever a file was copied, and a signed bundle is signed again.

if(XCODE_BUNDLE)
    if(NOT DEFINED ENV{TARGET_BUILD_DIR} OR NOT DEFINED ENV{UNLOCALIZED_RESOURCES_FOLDER_PATH})
        message(FATAL_ERROR "StageAssets.cmake: XCODE_BUNDLE needs Xcode's TARGET_BUILD_DIR and "
                            "UNLOCALIZED_RESOURCES_FOLDER_PATH (run it in a script phase)")
    endif()
    set(STAGE "$ENV{TARGET_BUILD_DIR}/$ENV{UNLOCALIZED_RESOURCES_FOLDER_PATH}/assets")
endif()
if(NOT STAGE OR NOT SOURCES)
    message(FATAL_ERROR "StageAssets.cmake: -DSTAGE=<dir> (or -DXCODE_BUNDLE=ON) -DSOURCES=<dir>|<dir>... are required")
endif()
string(REPLACE "|" ";" sources "${SOURCES}")
file(MAKE_DIRECTORY ${STAGE})
set(copied 0)
foreach(source IN LISTS sources)
    file(GLOB_RECURSE files RELATIVE ${source} LIST_DIRECTORIES false ${source}/*)
    foreach(file IN LISTS files)
        set(target ${STAGE}/${file})
        if(EXISTS ${target})
            file(SIZE ${source}/${file} source_size)
            file(SIZE ${target} target_size)
            if(source_size EQUAL target_size)
                file(TIMESTAMP ${source}/${file} source_time "%Y%m%d%H%M%S" UTC)
                file(TIMESTAMP ${target} target_time "%Y%m%d%H%M%S" UTC)
                if(source_time STREQUAL target_time)
                    continue()
                endif()
                file(SHA256 ${source}/${file} source_hash)
                file(SHA256 ${target} target_hash)
                if(source_hash STREQUAL target_hash)
                    # Only the time: set it without writing the file (a copy replaces it, which changes its folder).
                    execute_process(COMMAND touch -r ${source}/${file} ${target})
                    continue()
                endif()
            endif()
        endif()
        get_filename_component(target_dir ${target} DIRECTORY)
        file(COPY ${source}/${file} DESTINATION ${target_dir})
        math(EXPR copied "${copied} + 1")
    endforeach()
endforeach()
if(copied GREATER 0)
    file(TOUCH_NOCREATE ${STAGE})
    message(STATUS "Staged ${copied} changed asset files in ${STAGE}")
    # Xcode signs the bundle only when a file it copied or linked changed: sign it again as Xcode does (its identity
    # and entitlements), or the changed assets break the signature.
    if(XCODE_BUNDLE AND "$ENV{CODE_SIGNING_ALLOWED}" STREQUAL "YES"
       AND NOT "$ENV{EXPANDED_CODE_SIGN_IDENTITY}" STREQUAL "")
        set(sign_args --force --sign "$ENV{EXPANDED_CODE_SIGN_IDENTITY}" --timestamp=none --generate-entitlement-der)
        set(entitlements "$ENV{TARGET_TEMP_DIR}/$ENV{FULL_PRODUCT_NAME}.xcent")
        if(EXISTS "${entitlements}")
            list(APPEND sign_args --entitlements "${entitlements}")
        endif()
        execute_process(COMMAND codesign ${sign_args} "$ENV{CODESIGNING_FOLDER_PATH}" RESULT_VARIABLE signed)
        if(NOT signed EQUAL 0)
            message(FATAL_ERROR "StageAssets.cmake: codesign failed for $ENV{CODESIGNING_FOLDER_PATH}")
        endif()
    endif()
endif()
