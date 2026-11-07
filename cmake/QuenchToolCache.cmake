# The per-machine cache shared by every build tree and game for what is too large or too slow to set up in each one:
# the DXC built from source (cmake/QuenchDxc.cmake) and the parts of the Basis Universal archive the engine builds
# (cmake/QuenchBasisu.cmake). QUENCH_TOOL_CACHE overrides the location; the default is ~/Library/Caches/Quench on
# macOS, %LOCALAPPDATA%/Quench on Windows, $XDG_CACHE_HOME/quench or ~/.cache/quench elsewhere.
include_guard(GLOBAL)

set(QUENCH_TOOL_CACHE "" CACHE PATH "Per-machine cache of host tools built from source (empty: the user's cache)")

function(_quench_tool_cache out)
    if(QUENCH_TOOL_CACHE)
        set(${out} ${QUENCH_TOOL_CACHE} PARENT_SCOPE)
    elseif(CMAKE_HOST_APPLE)
        set(${out} $ENV{HOME}/Library/Caches/Quench PARENT_SCOPE)
    elseif(CMAKE_HOST_WIN32)
        file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Quench" cache)
        set(${out} ${cache} PARENT_SCOPE)
    elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
        set(${out} $ENV{XDG_CACHE_HOME}/quench PARENT_SCOPE)
    else()
        set(${out} $ENV{HOME}/.cache/quench PARENT_SCOPE)
    endif()
endfunction()
