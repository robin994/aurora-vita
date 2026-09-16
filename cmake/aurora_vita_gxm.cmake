if (NOT VITA AND NOT CMAKE_SYSTEM_NAME STREQUAL "Vita" AND NOT CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi")
    message(FATAL_ERROR "GXM runtime requires VitaSDK. Use VITAGL + AURORA_VITA_BUILD_BACKEND_TESTS for host tests.")
endif ()
if (AURORA_VITA_WITH_GX_FRONTEND OR AURORA_VITA_WITH_UPSTREAM_GX)
    message(FATAL_ERROR "GXM is currently a standalone native renderer/probe. The GX DrawSink/EFB adapter is not implemented; select VITAGL for games.")
endif ()
if (AURORA_VITA_BUILD_SDL3_PROBE)
    message(FATAL_ERROR "The SDL3/vitaGL probe cannot be linked into the GXM-only target")
endif ()
add_library(aurora_vita_gxm_backend STATIC
    ${PROJECT_SOURCE_DIR}/platforms/vita/gxm/gxm_memory.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gxm/gxm_shader_gen.cpp
    ${PROJECT_SOURCE_DIR}/platforms/vita/gxm/gxm_renderer.cpp)
add_library(aurora::vita_backend ALIAS aurora_vita_gxm_backend)
add_library(aurora::vita_gxm_backend ALIAS aurora_vita_gxm_backend)
target_compile_features(aurora_vita_gxm_backend PUBLIC cxx_std_20)
target_compile_definitions(aurora_vita_gxm_backend PUBLIC AURORA_VITA_RENDERER_GXM=1)
target_compile_options(aurora_vita_gxm_backend PRIVATE
    -O3 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti
    -mtune=cortex-a9 -mfpu=neon -fsigned-char)
target_link_libraries(aurora_vita_gxm_backend PUBLIC aurora::vita_common
    vitashark SceShaccCgExt SceShaccCg_stub taihen_stub
    SceGxm_stub SceDisplay_stub SceSysmodule_stub SceLibKernel_stub m)
if (AURORA_VITA_BUILD_PROBE)
    include("${VITASDK}/share/vita.cmake")
    add_executable(aurora_vita_gxm_probe ${PROJECT_SOURCE_DIR}/platforms/vita/probe/gxm_main.cpp)
    target_link_options(aurora_vita_gxm_probe PRIVATE
        -Wl,--gc-sections -Wl,-q -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/aurora_vita_gxm_probe.map)
    target_link_libraries(aurora_vita_gxm_probe PRIVATE aurora::vita_gxm_backend SceCtrl_stub)
    vita_create_self(aurora_vita_gxm_probe.self aurora_vita_gxm_probe)
    vita_create_vpk(aurora_vita_gxm_probe.vpk AURVGXM01 aurora_vita_gxm_probe.self
        VERSION 00.10 NAME "Aurora Native GXM Probe")
endif ()
