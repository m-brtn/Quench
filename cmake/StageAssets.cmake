# Merges asset directories into the folder an Xcode build copies into the app bundle (quench_add_game): cmake
# -DSTAGE=<dir> -DSOURCES=<dir>|<dir>... -P StageAssets.cmake. Xcode copies a folder reference again only when the
# folder itself changed, not when a file nested in it did, so the folder is touched whenever a file was copied; with
# copy_directory_if_different a re-cooked file in a subfolder never reached the bundle. A file counts as changed when
# it is missing in the stage or differs from it in size or content, and is copied (file(COPY) keeps the source's
# time); one that differs only in its modification time (the Xcode build cooks every file again in every build) gets
# the source's time. Files removed from a source stay in the stage, as copy_directory_if_different leaves them.

if(NOT STAGE OR NOT SOURCES)
    message(FATAL_ERROR "StageAssets.cmake: -DSTAGE=<dir> -DSOURCES=<dir>|<dir>... are required")
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
endif()
