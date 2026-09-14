# Aurora Vita backend target. The default target is intentionally Dawn-free and
# can be built with AURORA_ENABLE_GX=OFF. Optional upstream-GX mode is a desktop
# integration/syntax gate while Aurora's upstream GX target still owns Dawn.
option(AURORA_VITA_WITH_UPSTREAM_GX "Compile the Vita bridge against Aurora's real GX structs (requires AURORA_ENABLE_GX)" OFF)
option(AURORA_VITA_WITH_GX_FRONTEND "Build the Dawn-free Dolphin GX/VI frontend for native Vita ports" OFF)
option(AURORA_VITA_DIRECT_STREAM_WRITE "Write frame streaming data directly into mapped Vita GL buffers" OFF)
option(AURORA_VITA_RUNTIME_MIPMAP_GENERATION "Generate missing texture mip chains at runtime on Vita" OFF)
option(AURORA_VITA_BUILD_SDL3_PROBE "Build SDL3 native-platform + vitaGL coexistence probe" OFF)

set(AURORA_VITA_BACKEND_SOURCES
    ${PROJECT_SOURCE_DIR}/platforms/vita/aurora_vita_backend.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gx/aurora_vita_draw_sink.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/integration/vita_feature_coverage.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/integration/vita_frame_trace.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/integration/vita_fifo_packet_queue.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/integration/vita_gx_capture.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/integration/vita_gx_replay.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/integration/wiicompiled_aurora_adapter.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/integration/vita_gx_backend.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_telemetry.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_memory_budget.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_gl_util.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_cpu_workers.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_texture_decode.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_vertex_decode.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_vertex_pipeline.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_streaming_arena.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_draw_adapter.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_shader_gen.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_pipeline_key.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_buffer_pool.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_texture_cache.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_pipeline_cache.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_efb.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx/vita_renderer.cpp
)

if (AURORA_VITA_WITH_UPSTREAM_GX)
    if (NOT AURORA_ENABLE_GX)
        message(FATAL_ERROR "AURORA_VITA_WITH_UPSTREAM_GX requires AURORA_ENABLE_GX=ON until the upstream GX/Dawn split is complete")
    endif ()
    list(APPEND AURORA_VITA_BACKEND_SOURCES
        ${PROJECT_SOURCE_DIR}/platforms/vita/gx/aurora_gx_bridge.cpp
    )
endif ()

if (AURORA_VITA_WITH_GX_FRONTEND)
    list(APPEND AURORA_VITA_BACKEND_SOURCES
        ${PROJECT_SOURCE_DIR}/platforms/vita/gx/aurora_gx_bridge.cpp
        ${PROJECT_SOURCE_DIR}/lib/vita/runtime.cpp
        ${PROJECT_SOURCE_DIR}/lib/vita/gx_frontend_state.cpp
        ${PROJECT_SOURCE_DIR}/lib/gx/fifo.cpp
        ${PROJECT_SOURCE_DIR}/lib/gx/command_processor.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXBump.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXCull.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXCpu2Efb.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXDispList.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXDraw.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXExtra.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXFifo.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXFrameBuffer.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXGeometry.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXGet.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXLighting.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXManage.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXPixel.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXTev.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXTexture.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXTransform.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXVert.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/gx/GXAurora.cpp
        ${PROJECT_SOURCE_DIR}/lib/dolphin/vi/vi.cpp
    )
endif ()

add_library(aurora_vita_backend STATIC ${AURORA_VITA_BACKEND_SOURCES})
add_library(aurora::vita_backend ALIAS aurora_vita_backend)
set_target_properties(aurora_vita_backend PROPERTIES FOLDER "aurora")
target_compile_features(aurora_vita_backend PUBLIC cxx_std_20)
target_include_directories(aurora_vita_backend PUBLIC
    ${PROJECT_SOURCE_DIR}/platforms/vita
    ${PROJECT_SOURCE_DIR}/platforms/vita/gfx
    ${PROJECT_SOURCE_DIR}/platforms/vita/integration
)
target_compile_definitions(aurora_vita_backend PUBLIC AURORA_PLATFORM_VITA=1 AURORA_GFX_VITA=1)

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
    target_compile_options(aurora_vita_backend PRIVATE
        -O3 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fshort-wchar
        -mtune=cortex-a9 -mfpu=neon -ffast-math -fsigned-char
    )
    target_link_options(aurora_vita_backend PRIVATE -Wl,--gc-sections -Wl,-q)
    target_link_libraries(aurora_vita_backend PUBLIC
        vitaGL vitashark SceShaccCgExt SceShaccCg_stub taihen_stub
        SceGxm_stub SceDisplay_stub SceCtrl_stub SceAppMgr_stub SceCommonDialog_stub
        SceKernelDmacMgr_stub SceSysmodule_stub SceLibKernel_stub
        mathneon pthread m
    )
endif()

if (VITA AND AURORA_VITA_SDL3_NATIVE AND AURORA_VITA_BUILD_SDL3_PROBE)
    include("${VITASDK}/share/vita.cmake")
    add_executable(aurora_vita_sdl3_probe
        ${PROJECT_SOURCE_DIR}/platforms/vita/probe/sdl3_vitagl_probe.cpp
    )
    target_compile_features(aurora_vita_sdl3_probe PRIVATE cxx_std_20)
    target_compile_definitions(aurora_vita_sdl3_probe PRIVATE AURORA_VITA_SDL3_NATIVE=1)
    target_link_libraries(aurora_vita_sdl3_probe PRIVATE aurora::vita_backend ${AURORA_SDL3_TARGET})
    vita_create_self(aurora_vita_sdl3_probe.self aurora_vita_sdl3_probe)
    vita_create_vpk(aurora_vita_sdl3_probe.vpk AURVSDL01 aurora_vita_sdl3_probe.self
        VERSION 01.00 NAME "Aurora SDL3 + vitaGL Probe")
endif ()
