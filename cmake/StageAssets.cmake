# Merges asset directories into one folder (quench_add_game's Xcode and web builds): cmake -DSTAGE=<dir>
# -DSOURCES=<dir>|<dir>... -P StageAssets.cmake. With -DXCODE_BUNDLE=ON instead of STAGE, in a script phase of an
# app target, the folder is the bundle's assets/, from the build settings Xcode passes to the phase. A later source
# wins over an earlier one for a file both have. A file counts as changed when it is missing in the folder or differs
# from it in size or content, and is copied (file(COPY) keeps the source's time); one that differs only in its
# modification time (cooked again with the same result) gets the source's time. Files removed from a source stay in
# the folder, as copy_directory_if_different leaves them. Finder's .DS_Store files are left out. The folder is
# touched whenever a file was copied or removed, and a signed bundle is signed again.
#
# Deferred assets (a web build's DEFERRED_ASSETS): -DDEFERRED=<pattern>|<pattern>... -DDEFERRED_DIR=<dir>
# -DMANIFEST=<file> -DFOLDER=<name>. A pattern is relative to the asset root: a file, a directory (everything below
# it) or a file(GLOB) expression (`music/*.mp3`; `*` stays within one directory). The files it matches are copied to
# DEFERRED_DIR instead of STAGE (and removed from STAGE if an earlier build put them there; files in DEFERRED_DIR
# that are no longer deferred are removed), and MANIFEST lists them: "folder <FOLDER>", then "<size> <path>" per
# file. MANIFEST is written only when its text changes (the page is linked again then).

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
string(REPLACE "|" ";" deferred_patterns "${DEFERRED}")
file(MAKE_DIRECTORY ${STAGE})
set(copied 0)

# Copies `source_root`/`file` to `target_root`/`file` when it changed (see the top).
macro(stage_file source_root file target_root)
    set(_source ${source_root}/${file})
    set(_target ${target_root}/${file})
    set(_copy ON)
    if(EXISTS ${_target})
        file(SIZE ${_source} _source_size)
        file(SIZE ${_target} _target_size)
        if(_source_size EQUAL _target_size)
            file(TIMESTAMP ${_source} _source_time "%Y%m%d%H%M%S" UTC)
            file(TIMESTAMP ${_target} _target_time "%Y%m%d%H%M%S" UTC)
            if(_source_time STREQUAL _target_time)
                set(_copy OFF)
            else()
                file(SHA256 ${_source} _source_hash)
                file(SHA256 ${_target} _target_hash)
                if(_source_hash STREQUAL _target_hash)
                    # Only the time: set it without writing the file (a copy replaces it, which changes its folder).
                    execute_process(COMMAND touch -r ${_source} ${_target})
                    set(_copy OFF)
                endif()
            endif()
        endif()
    endif()
    if(_copy)
        get_filename_component(_target_dir ${_target} DIRECTORY)
        file(COPY ${_source} DESTINATION ${_target_dir})
        math(EXPR copied "${copied} + 1")
    endif()
endmacro()

# Every file and the source it comes from: the last source that has it.
set(all_files "")
foreach(source IN LISTS sources)
    file(GLOB_RECURSE files RELATIVE ${source} LIST_DIRECTORIES false ${source}/*)
    foreach(file IN LISTS files)
        get_filename_component(name ${file} NAME)
        if(name STREQUAL ".DS_Store")
            continue()
        endif()
        list(APPEND all_files ${file})
        set("source_of_${file}" ${source})
    endforeach()
endforeach()
list(REMOVE_DUPLICATES all_files)

set(deferred_files "")
foreach(pattern IN LISTS deferred_patterns)
    set(matched "")
    foreach(source IN LISTS sources)
        if(IS_DIRECTORY ${source}/${pattern})
            file(GLOB_RECURSE found RELATIVE ${source} LIST_DIRECTORIES false ${source}/${pattern}/*)
        else()
            file(GLOB found RELATIVE ${source} LIST_DIRECTORIES false ${source}/${pattern})
        endif()
        list(APPEND matched ${found})
    endforeach()
    if(NOT matched)
        message(WARNING "StageAssets.cmake: the deferred asset pattern '${pattern}' matches no file")
    endif()
    list(APPEND deferred_files ${matched})
endforeach()
list(REMOVE_DUPLICATES deferred_files)
list(FILTER deferred_files EXCLUDE REGEX "(^|/)\\.DS_Store$")
list(SORT deferred_files)

foreach(file IN LISTS all_files)
    if(file IN_LIST deferred_files)
        if(EXISTS ${STAGE}/${file})
            file(REMOVE ${STAGE}/${file})
            math(EXPR copied "${copied} + 1")
        endif()
        set(_staged ${copied}) # a deferred file is not in the folder: it does not count as a change of it
        stage_file(${source_of_${file}} ${file} ${DEFERRED_DIR})
        set(copied ${_staged})
    else()
        stage_file(${source_of_${file}} ${file} ${STAGE})
    endif()
endforeach()

# A .DS_Store an earlier build staged.
file(GLOB_RECURSE stale LIST_DIRECTORIES false ${STAGE}/.DS_Store) # in every directory below STAGE
foreach(file IN LISTS stale)
    file(REMOVE ${file})
    math(EXPR copied "${copied} + 1")
endforeach()

if(MANIFEST)
    file(MAKE_DIRECTORY ${DEFERRED_DIR})
    file(GLOB_RECURSE present RELATIVE ${DEFERRED_DIR} LIST_DIRECTORIES false ${DEFERRED_DIR}/*)
    foreach(file IN LISTS present)
        if(NOT file IN_LIST deferred_files)
            file(REMOVE ${DEFERRED_DIR}/${file})
        endif()
    endforeach()
    set(manifest "folder ${FOLDER}\n")
    set(deferred_bytes 0)
    foreach(file IN LISTS deferred_files)
        file(SIZE ${DEFERRED_DIR}/${file} size)
        string(APPEND manifest "${size} ${file}\n")
        math(EXPR deferred_bytes "${deferred_bytes} + ${size}")
    endforeach()
    set(old_manifest "")
    if(EXISTS ${MANIFEST})
        file(READ ${MANIFEST} old_manifest)
    endif()
    if(NOT old_manifest STREQUAL manifest)
        file(WRITE ${MANIFEST} "${manifest}")
        list(LENGTH deferred_files deferred_count)
        message(STATUS "Deferred ${deferred_count} asset files (${deferred_bytes} bytes) to ${DEFERRED_DIR}")
    endif()
endif()
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
