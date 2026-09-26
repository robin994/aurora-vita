if (NOT TARGET xxhash)
    include(FetchContent)
    FetchContent_Declare(xxhash
        URL https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz
        URL_HASH SHA256=aae608dfe8213dfd05d909a57718ef82f30722c392344583d3f39050c7f29a80
        SOURCE_SUBDIR cmake_unofficial
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL)
    set(XXHASH_BUILD_XXHSUM OFF CACHE INTERNAL "Build the xxhsum binary")
    set(_aurora_vita_xxhash_saved_bsl "${BUILD_SHARED_LIBS}")
    FetchContent_MakeAvailable(xxhash)
    unset(BUILD_SHARED_LIBS CACHE)
    set(BUILD_SHARED_LIBS "${_aurora_vita_xxhash_saved_bsl}")
    unset(_aurora_vita_xxhash_saved_bsl)
endif ()
if (NOT TARGET robin_hood)
    include(FetchContent)
    FetchContent_Declare(robin_hood
        GIT_REPOSITORY https://github.com/martinus/robin-hood-hashing.git
        GIT_TAG 9145f963d80d6a02f0f96a47758050a89184a3ed
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(robin_hood)
endif ()

add_library(aurora_vita_common STATIC
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/vita_data_paths.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_cpu_workers.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_memory_revision.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_vertex_decode.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_vertex_pipeline.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_texture_decode.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_pipeline_key.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_efb_copy.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gxm/gxm_texture_layout.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_telemetry.cpp)
add_library(aurora::vita_common ALIAS aurora_vita_common)
target_compile_features(aurora_vita_common PUBLIC cxx_std_20)
target_include_directories(aurora_vita_common PUBLIC
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx)
target_link_libraries(aurora_vita_common PUBLIC xxhash robin_hood)
if (AURORA_VITA_RUNTIME_LOGGING)
    target_compile_definitions(aurora_vita_common PUBLIC AURORA_VITA_RUNTIME_LOGGING=1)
else ()
    target_compile_definitions(aurora_vita_common PUBLIC AURORA_VITA_RUNTIME_LOGGING=0)
endif ()
if (VITA OR CMAKE_SYSTEM_NAME STREQUAL "Vita" OR CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi")
    target_compile_definitions(aurora_vita_common PUBLIC __vita__=1)
    if(AURORA_VITA_PORT_ABI STREQUAL "GAMECUBE")
        target_compile_options(aurora_vita_common PUBLIC -fshort-wchar -fno-short-enums)
    endif()
    target_compile_options(aurora_vita_common PRIVATE
        -O3 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti
        -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -fsigned-char
        -fno-math-errno -funsafe-math-optimizations -fno-signed-zeros -ffp-contract=fast)
    if (AURORA_VITA_LTO)
        target_compile_options(aurora_vita_common PRIVATE -flto=${AURORA_VITA_LTO_JOBS} -ffat-lto-objects)
    endif ()
    target_link_libraries(aurora_vita_common PUBLIC SceLibKernel_stub pthread m)
endif ()
