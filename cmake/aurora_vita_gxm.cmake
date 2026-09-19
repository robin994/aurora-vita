if (NOT VITA AND NOT CMAKE_SYSTEM_NAME STREQUAL "Vita" AND NOT CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi")
    message(FATAL_ERROR "GXM runtime requires VitaSDK. Use VITAGL + AURORA_VITA_BUILD_BACKEND_TESTS for host tests.")
endif ()
if (AURORA_VITA_WITH_UPSTREAM_GX)
    message(FATAL_ERROR "Use the Dawn-free AURORA_VITA_WITH_GX_FRONTEND with native GXM")
endif ()
if (AURORA_VITA_BUILD_SDL3_PROBE)
    message(FATAL_ERROR "The SDL3/vitaGL probe cannot be linked into the GXM-only target")
endif ()
option(AURORA_VITA_NATIVE_CMPR "Experimental GXM CMPR/BC1 uploads; requires hardware image comparison" OFF)
option(AURORA_VITA_NATIVE_GX_TEXTURES "Experimental GXM I/I+A/RGB565 uploads; requires hardware image comparison" OFF)
add_library(aurora_vita_gxm_backend STATIC
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gxm/gxm_memory.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gxm/gxm_program_cache.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gxm/gxm_shader_gen.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gxm/gxm_facade.cpp
    ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gxm/gxm_renderer.cpp)
aurora_vita_attach_frontend(aurora_vita_gxm_backend)
add_library(aurora::vita_backend ALIAS aurora_vita_gxm_backend)
add_library(aurora::vita_gxm_backend ALIAS aurora_vita_gxm_backend)
target_compile_features(aurora_vita_gxm_backend PUBLIC cxx_std_20)
target_compile_definitions(aurora_vita_gxm_backend PUBLIC AURORA_VITA_RENDERER_GXM=1)
target_compile_definitions(aurora_vita_gxm_backend PRIVATE AURORA_VITA_DIRECT_STREAM_WRITE=0)
target_compile_definitions(aurora_vita_gxm_backend PRIVATE
    AURORA_VITA_NATIVE_CMPR=$<BOOL:${AURORA_VITA_NATIVE_CMPR}>
    AURORA_VITA_NATIVE_GX_TEXTURES=$<BOOL:${AURORA_VITA_NATIVE_GX_TEXTURES}>)
target_compile_options(aurora_vita_gxm_backend PRIVATE
    -O3 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti
    -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -fsigned-char
    -fno-math-errno -funsafe-math-optimizations -fno-signed-zeros -ffp-contract=fast)
if (AURORA_VITA_LTO)
    target_compile_options(aurora_vita_gxm_backend PRIVATE -flto=auto -ffat-lto-objects)
    target_link_options(aurora_vita_gxm_backend INTERFACE -flto=auto)
endif ()
target_link_libraries(aurora_vita_gxm_backend PUBLIC aurora::vita_common
    vitashark SceShaccCgExt SceShaccCg_stub taihen_stub
    SceGxm_stub SceDisplay_stub SceSysmodule_stub SceLibKernel_stub SceCtrl_stub
    SceAppMgr_stub SceCommonDialog_stub SceKernelDmacMgr_stub mathneon m)
if (AURORA_VITA_BUILD_PROBE)
    include("${VITASDK}/share/vita.cmake")
    add_executable(aurora_vita_gxm_probe ${AURORA_VITA_SOURCE_DIR}/platforms/vita/probe/gxm_main.cpp)
    target_link_options(aurora_vita_gxm_probe PRIVATE
        -Wl,--gc-sections -Wl,-q -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/aurora_vita_gxm_probe.map)
    target_link_libraries(aurora_vita_gxm_probe PRIVATE aurora::vita_gxm_backend SceCtrl_stub)
    vita_create_self(aurora_vita_gxm_probe.self aurora_vita_gxm_probe)
    vita_create_vpk(aurora_vita_gxm_probe.vpk AURVGXM01 aurora_vita_gxm_probe.self
        VERSION 00.10 NAME "Aurora Native GXM Probe")
endif ()
