# `cmake -P` patch for the pinned PPQSort source (FetchContent PATCH_COMMAND, cmake/QuenchTracy.cmake); run in its
# source directory. The two changes Tracy v0.15.0 applies to it (its cmake/ppqsort-nodebug.patch and
# ppqsort-semaphore.patch): no debug printing includes (<syncstream>) without NDEBUG, and a counting semaphore where
# several workers may release the binary one at once. Idempotent.
set(_marker "Quench patch (cmake/patches/TracyPpqsort.cmake)")

set(_file include/ppqsort/parameters.h)
file(READ ${_file} _src)
string(FIND "${_src}" "${_marker}" _found)
if(_found EQUAL -1)
    string(FIND "${_src}" "#ifndef NDEBUG\n    #include <bitset>" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "TracyPpqsort.cmake: the debug includes are not in ${_file}")
    endif()
    string(REPLACE "#ifndef NDEBUG\n    #include <bitset>" "#if 0 // ${_marker}\n    #include <bitset>" _src "${_src}")
    file(WRITE ${_file} "${_src}")
endif()

set(_file include/ppqsort/parallel/cpp/thread_pool.h)
file(READ ${_file} _src)
string(FIND "${_src}" "${_marker}" _found)
if(_found EQUAL -1)
    set(_old "std::binary_semaphore threads_done_semaphore_{0};")
    string(FIND "${_src}" "${_old}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "TracyPpqsort.cmake: the binary semaphore is not in ${_file}")
    endif()
    string(REPLACE "${_old}" "std::counting_semaphore<> threads_done_semaphore_{0}; // ${_marker}" _src "${_src}")
    file(WRITE ${_file} "${_src}")
endif()
