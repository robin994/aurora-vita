# Select exactly one hardware implementation. The common target has no GL/GXM API.
get_filename_component(AURORA_VITA_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/aurora_vita_frontend.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/AuroraVitaRendererSelection.cmake")
option(AURORA_VITA_WITH_UPSTREAM_GX "Compile the Vita bridge against Aurora's real GX structs (requires AURORA_ENABLE_GX)" OFF)
option(AURORA_VITA_WITH_GX_FRONTEND "Build the Dawn-free Dolphin GX/VI frontend for native Vita ports" OFF)
option(AURORA_VITA_BUILD_PROBE "Build the standalone probe for the selected Vita renderer" OFF)
option(AURORA_VITA_BUILD_SDL3_PROBE "Build SDL3 native-platform + vitaGL coexistence probe" OFF)
set(AURORA_VITA_PORT_ABI "SDK" CACHE STRING "Public ABI: SDK or GAMECUBE (32-bit enums, short wchar)")
set_property(CACHE AURORA_VITA_PORT_ABI PROPERTY STRINGS SDK GAMECUBE)
if(NOT AURORA_VITA_PORT_ABI MATCHES "^(SDK|GAMECUBE)$")
    message(FATAL_ERROR "AURORA_VITA_PORT_ABI must be SDK or GAMECUBE")
endif()
if(AURORA_VITA_WITH_GX_FRONTEND AND AURORA_VITA_WITH_UPSTREAM_GX)
    message(FATAL_ERROR "Select one GX frontend owner, not both native and Dawn-backed GX")
endif()
message(STATUS "Aurora Vita renderer: ${AURORA_VITA_RENDERER} (compile-time selection)")
include("${CMAKE_CURRENT_LIST_DIR}/aurora_vita_common.cmake")
if (AURORA_VITA_RENDERER STREQUAL "GXM")
    include("${CMAKE_CURRENT_LIST_DIR}/aurora_vita_gxm.cmake")
else ()
    include("${CMAKE_CURRENT_LIST_DIR}/aurora_vitagl.cmake")
endif ()

if(VITA AND AURORA_VITA_WITH_GX_FRONTEND AND AURORA_VITA_BUILD_PROBE)
    include("${VITASDK}/share/vita.cmake")
    add_executable(aurora_vita_gx_probe ${AURORA_VITA_SOURCE_DIR}/platforms/vita/probe/gx_frontend_main.cpp)
    target_compile_definitions(aurora_vita_gx_probe PRIVATE TARGET_PC=1 MKW_TARGET_VITA=1
        AURORA_TEST_RENDERER="${AURORA_VITA_RENDERER}")
    target_link_libraries(aurora_vita_gx_probe PRIVATE aurora::vita_backend SceCtrl_stub)
    target_link_options(aurora_vita_gx_probe PRIVATE -Wl,--gc-sections -Wl,-q
        -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/aurora_vita_gx_probe.map)
    if(AURORA_VITA_RENDERER STREQUAL "GXM")
        set(_gx_probe_title AURGXGM01)
    else()
        set(_gx_probe_title AURGXGL01)
    endif()
    vita_create_self(aurora_vita_gx_probe.self aurora_vita_gx_probe)
    vita_create_vpk(aurora_vita_gx_probe.vpk ${_gx_probe_title} aurora_vita_gx_probe.self
        VERSION 00.20 NAME "Aurora GX ${AURORA_VITA_RENDERER} Probe")
endif()

option(AURORA_VITA_BUILD_BACKEND_TESTS "Build dependency-free Vita backend contract tests" OFF)
if (AURORA_VITA_BUILD_BACKEND_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_executable(aurora_vita_backend_contract_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_backend_contract_test.cpp
        ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gxm/gxm_shader_gen.cpp)
    target_link_libraries(aurora_vita_backend_contract_test PRIVATE aurora::vita_common)
    add_test(NAME vita_backend_contract COMMAND aurora_vita_backend_contract_test)
    add_test(NAME vita_renderer_selection
        COMMAND ${CMAKE_COMMAND}
            -DSELECTION_MODULE=${CMAKE_CURRENT_LIST_DIR}/AuroraVitaRendererSelection.cmake
            -P ${AURORA_VITA_SOURCE_DIR}/tests/vita_renderer_selection_test.cmake)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME vita_binary_audit_contract
            COMMAND ${Python3_EXECUTABLE} ${AURORA_VITA_SOURCE_DIR}/tests/vita_binary_audit_test.py)
    endif()
endif ()
