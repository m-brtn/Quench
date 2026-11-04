# Runtime dependencies, pinned to an archive of a specific commit + SHA256 (no floating branches). The FetchContent
# names and pins match the games' (cmake/Dependencies.cmake in TrailForge and Ashfall), so whichever project declares a
# dependency first, the same sources are used. GitHub serves commit archives deterministically; if a hash ever
# changes on GitHub's side, the build fails with an explicit URL_HASH error.
include(FetchContent)

# ---------------------------------------------------------------- Jolt Physics
# v5.6.0
set(DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
set(GENERATE_DEBUG_SYMBOLS ON CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED ON CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(JoltPhysics
    URL https://github.com/jrouwe/JoltPhysics/archive/e77f175595e64cb44218cc9d9d56fc365ad0e36a.tar.gz
    URL_HASH SHA256=1f32328fb763135de10a244568d6ccb2ed9b1e6593fafe6dc6db5b2719d330bd
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR Build
    SYSTEM
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(JoltPhysics)
