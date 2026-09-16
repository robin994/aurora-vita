add_library(aurora_vita_common STATIC
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
if (VITA OR CMAKE_SYSTEM_NAME STREQUAL "Vita" OR CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi")
    target_compile_definitions(aurora_vita_common PUBLIC __vita__=1)
    if(AURORA_VITA_PORT_ABI STREQUAL "GAMECUBE")
        target_compile_options(aurora_vita_common PUBLIC -fshort-wchar -fno-short-enums)
    endif()
    target_compile_options(aurora_vita_common PRIVATE
        -O3 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti
        -mtune=cortex-a9 -mfpu=neon -fsigned-char)
    # Both standalone backends use VitaSDK's default wchar/enum ABI. Keep only
    # the historical vitaGL floating-point optimization backend-specific.
    if (AURORA_VITA_RENDERER STREQUAL "VITAGL")
        target_compile_options(aurora_vita_common PRIVATE -ffast-math)
    endif ()
    target_link_libraries(aurora_vita_common PUBLIC SceLibKernel_stub pthread m)
endif ()
