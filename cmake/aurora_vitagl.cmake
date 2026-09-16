# Aurora Vita backend target. The default target is intentionally Dawn-free and
# can be built with AURORA_ENABLE_GX=OFF. Optional upstream-GX mode is a desktop
# integration/syntax gate while Aurora's upstream GX target still owns Dawn.
option(AURORA_VITA_DIRECT_STREAM_WRITE "Write frame streaming data directly into mapped Vita GL buffers" OFF)
option(AURORA_VITA_RUNTIME_MIPMAP_GENERATION "Generate missing texture mip chains at runtime on Vita" OFF)
option(AURORA_VITA_NATIVE_CMPR "Upload GameCube CMPR through vitaGL's native DXT1 path" ON)
option(AURORA_VITA_NATIVE_GX_TEXTURES "Upload exact GX I/I+A/RGB565 formats through native vitaGL texture formats" ON)

set(AURORA_VITA_BACKEND_SOURCES
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_gl_util.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_shader_gen.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_buffer_pool.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_texture_cache.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_pipeline_cache.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_efb.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_renderer.cpp
)

if (AURORA_VITA_WITH_UPSTREAM_GX)
    if (NOT AURORA_ENABLE_GX)
        message(FATAL_ERROR "AURORA_VITA_WITH_UPSTREAM_GX requires AURORA_ENABLE_GX=ON until the upstream GX/Dawn split is complete")
    endif ()
endif ()

add_library(aurora_vita_backend STATIC ${AURORA_VITA_BACKEND_SOURCES})
aurora_vita_attach_frontend(aurora_vita_backend)
add_library(aurora::vita_backend ALIAS aurora_vita_backend)
add_library(aurora::vita_vitagl_backend ALIAS aurora_vita_backend)
target_link_libraries(aurora_vita_backend PUBLIC aurora::vita_common)
set_target_properties(aurora_vita_backend PROPERTIES FOLDER "aurora")
target_compile_features(aurora_vita_backend PUBLIC cxx_std_20)
target_include_directories(aurora_vita_backend PUBLIC
    ${AURORA_VITA_SOURCE_DIR}/include
    ${AURORA_VITA_SOURCE_DIR}/lib
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/integration
)
target_compile_definitions(aurora_vita_backend PUBLIC
    AURORA_PLATFORM_VITA=1 AURORA_GFX_VITA=1 AURORA_VITA_RENDERER_VITAGL=1)

if (AURORA_VITA_WITH_UPSTREAM_GX)
    target_compile_definitions(aurora_vita_backend PRIVATE AURORA_VITA_UPSTREAM=1)
    # This is intentionally a desktop/integration dependency today. Removing
    # this link is the completion criterion for the Dawn-free upstream GX split.
    target_link_libraries(aurora_vita_backend PRIVATE aurora::gx)
endif ()

if (AURORA_VITA_WITH_GX_FRONTEND)
    target_compile_definitions(aurora_vita_backend PRIVATE
        AURORA_VITA_UPSTREAM=1 MKW_TARGET_VITA=1 TARGET_PC=1)
endif ()

# When cross-compiling with VitaSDK the toolchain normally exposes these by name.
if (CMAKE_SYSTEM_NAME STREQUAL "Vita" OR DEFINED VITASDK OR CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi")
    if (AURORA_VITA_WITH_UPSTREAM_GX)
        message(FATAL_ERROR "Do not enable AURORA_VITA_WITH_UPSTREAM_GX on Vita yet: upstream aurora::gx still links Dawn")
    endif ()
    target_compile_definitions(aurora_vita_backend PUBLIC __vita__=1)
    if (AURORA_VITA_DIRECT_STREAM_WRITE)
        target_compile_definitions(aurora_vita_backend PRIVATE AURORA_VITA_DIRECT_STREAM_WRITE=1)
    endif ()
    if (AURORA_VITA_RUNTIME_MIPMAP_GENERATION)
        target_compile_definitions(aurora_vita_backend PRIVATE AURORA_VITA_RUNTIME_MIPMAP_GENERATION=1)
    endif ()
    if (AURORA_VITA_NATIVE_CMPR)
        target_compile_definitions(aurora_vita_backend PRIVATE AURORA_VITA_NATIVE_CMPR=1)
    endif ()
    if (AURORA_VITA_NATIVE_GX_TEXTURES)
        target_compile_definitions(aurora_vita_backend PRIVATE AURORA_VITA_NATIVE_GX_TEXTURES=1)
    endif ()
    target_compile_options(aurora_vita_backend PRIVATE
        -O3 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti
        -mtune=cortex-a9 -mfpu=neon -ffast-math -fsigned-char
    )
    target_link_options(aurora_vita_backend PRIVATE -Wl,--gc-sections -Wl,-q)
    include("${CMAKE_CURRENT_LIST_DIR}/AuroraVitaProgramCache.cmake")
    aurora_vita_bind_vitagl(aurora_vita_backend)
    target_link_libraries(aurora_vita_backend PUBLIC
        vitashark SceShaccCgExt SceShaccCg_stub taihen_stub
        SceGxm_stub SceDisplay_stub SceCtrl_stub SceAppMgr_stub SceCommonDialog_stub
        SceKernelDmacMgr_stub SceSysmodule_stub SceLibKernel_stub
        mathneon pthread m
    )
endif()

if (VITA AND AURORA_VITA_BUILD_PROBE)
    include("${VITASDK}/share/vita.cmake")
    add_executable(aurora_vita_probe
        ${AURORA_VITA_SOURCE_DIR}/platforms/vita/probe/main.cpp
    )
    target_compile_features(aurora_vita_probe PRIVATE cxx_std_20)
    target_link_libraries(aurora_vita_probe PRIVATE aurora::vita_backend)
    vita_create_self(aurora_vita_probe.self aurora_vita_probe)
    # Keep diagnostic probe installs independent from older AURVPRB01 builds so
    # VitaShell cannot accidentally launch an already-installed stale eboot.
    vita_create_vpk(aurora_vita_probe_diag2.vpk AURVPRB02 aurora_vita_probe.self
        VERSION 01.10 NAME "Aurora Vita 3D Probe D2")
endif ()

if (VITA AND AURORA_VITA_SDL3_NATIVE AND AURORA_VITA_BUILD_SDL3_PROBE)
    include("${VITASDK}/share/vita.cmake")
    add_executable(aurora_vita_sdl3_probe
        ${AURORA_VITA_SOURCE_DIR}/platforms/vita/probe/sdl3_vitagl_probe.cpp
    )
    target_compile_features(aurora_vita_sdl3_probe PRIVATE cxx_std_20)
    target_compile_definitions(aurora_vita_sdl3_probe PRIVATE AURORA_VITA_SDL3_NATIVE=1)
    target_link_libraries(aurora_vita_sdl3_probe PRIVATE aurora::vita_backend ${AURORA_SDL3_TARGET})
    vita_create_self(aurora_vita_sdl3_probe.self aurora_vita_sdl3_probe)
    vita_create_vpk(aurora_vita_sdl3_probe.vpk AURVSDL01 aurora_vita_sdl3_probe.self
        VERSION 01.00 NAME "Aurora SDL3 + vitaGL Probe")
endif ()
