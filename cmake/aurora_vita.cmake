# Select exactly one hardware implementation. The common target has no GL/GXM API.
include("${CMAKE_CURRENT_LIST_DIR}/AuroraVitaRendererSelection.cmake")
option(AURORA_VITA_WITH_UPSTREAM_GX "Compile the Vita bridge against Aurora's real GX structs (requires AURORA_ENABLE_GX)" OFF)
option(AURORA_VITA_WITH_GX_FRONTEND "Build the Dawn-free Dolphin GX/VI frontend for native Vita ports" OFF)
option(AURORA_VITA_BUILD_PROBE "Build the standalone probe for the selected Vita renderer" OFF)
option(AURORA_VITA_BUILD_SDL3_PROBE "Build SDL3 native-platform + vitaGL coexistence probe" OFF)
message(STATUS "Aurora Vita renderer: ${AURORA_VITA_RENDERER} (compile-time selection)")
include("${CMAKE_CURRENT_LIST_DIR}/aurora_vita_common.cmake")
if (AURORA_VITA_RENDERER STREQUAL "GXM")
    include("${CMAKE_CURRENT_LIST_DIR}/aurora_vita_gxm.cmake")
else ()
    include("${CMAKE_CURRENT_LIST_DIR}/aurora_vitagl.cmake")
endif ()

option(AURORA_VITA_BUILD_BACKEND_TESTS "Build dependency-free Vita backend contract tests" OFF)
if (AURORA_VITA_BUILD_BACKEND_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_executable(aurora_vita_backend_contract_test
        ${PROJECT_SOURCE_DIR}/tests/vita_backend_contract_test.cpp
        ${PROJECT_SOURCE_DIR}/platforms/vita/gxm/gxm_shader_gen.cpp)
    target_link_libraries(aurora_vita_backend_contract_test PRIVATE aurora::vita_common)
    add_test(NAME vita_backend_contract COMMAND aurora_vita_backend_contract_test)
    add_test(NAME vita_renderer_selection
        COMMAND ${CMAKE_COMMAND}
            -DSELECTION_MODULE=${CMAKE_CURRENT_LIST_DIR}/AuroraVitaRendererSelection.cmake
            -P ${PROJECT_SOURCE_DIR}/tests/vita_renderer_selection_test.cmake)
endif ()
