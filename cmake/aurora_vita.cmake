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
    add_executable(aurora_vita_native_material_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_native_material_test.cpp
        ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gxm/gxm_shader_gen.cpp)
    target_link_libraries(aurora_vita_native_material_test PRIVATE aurora::vita_common)
    add_test(NAME vita_native_material COMMAND aurora_vita_native_material_test)
    add_executable(aurora_vita_compile_native_assets
        ${AURORA_VITA_SOURCE_DIR}/tools/vita_compile_native_assets.cpp)
    target_link_libraries(aurora_vita_compile_native_assets PRIVATE aurora::vita_backend)
    add_executable(aurora_vita_native_assets_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_native_assets_test.cpp)
    target_link_libraries(aurora_vita_native_assets_test PRIVATE aurora::vita_backend)
    add_test(NAME vita_native_assets COMMAND aurora_vita_native_assets_test)
    add_executable(aurora_vita_prepared_display_list_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_prepared_display_list_test.cpp)
    add_test(NAME vita_prepared_display_list COMMAND aurora_vita_prepared_display_list_test)
    add_executable(aurora_vita_compact_texture_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_compact_texture_test.cpp)
    target_link_libraries(aurora_vita_compact_texture_test PRIVATE aurora::vita_common)
    add_test(NAME vita_compact_texture COMMAND aurora_vita_compact_texture_test)
    add_executable(aurora_vita_geometry_recipe_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_geometry_recipe_test.cpp)
    target_link_libraries(aurora_vita_geometry_recipe_test PRIVATE aurora::vita_backend)
    add_test(NAME vita_geometry_recipe COMMAND aurora_vita_geometry_recipe_test)
    add_executable(aurora_vita_vertex_reuse_probe_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_vertex_reuse_probe_test.cpp)
    target_link_libraries(aurora_vita_vertex_reuse_probe_test PRIVATE aurora::vita_backend)
    add_test(NAME vita_vertex_reuse_probe COMMAND aurora_vita_vertex_reuse_probe_test)
    # Explicit benchmark only, excluded from CTest and device FPS claims.
    add_executable(aurora_vita_fixed_builder_bench
        ${AURORA_VITA_SOURCE_DIR}/tools/vita_fixed_builder_bench.cpp)
    target_link_libraries(aurora_vita_fixed_builder_bench PRIVATE aurora::vita_common)
    add_executable(aurora_vita_byte_compare_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_byte_compare_test.cpp)
    target_link_libraries(aurora_vita_byte_compare_test PRIVATE aurora::vita_common)
    add_test(NAME vita_byte_compare COMMAND aurora_vita_byte_compare_test)
    add_executable(aurora_vita_vertex_pack_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_vertex_pack_test.cpp)
    target_link_libraries(aurora_vita_vertex_pack_test PRIVATE aurora::vita_common)
    add_test(NAME vita_vertex_pack COMMAND aurora_vita_vertex_pack_test)
    add_executable(aurora_vita_fixed_uniform_builder_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_fixed_uniform_builder_test.cpp)
    target_link_libraries(aurora_vita_fixed_uniform_builder_test PRIVATE aurora::vita_common)
    add_test(NAME vita_fixed_uniform_builder COMMAND aurora_vita_fixed_uniform_builder_test)
    add_executable(aurora_vita_regression_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_regression_test.cpp)
    target_link_libraries(aurora_vita_regression_test PRIVATE aurora::vita_backend)
    add_test(NAME vita_regression COMMAND aurora_vita_regression_test)
    # Independent opt-in diagnostics contracts (also tested by the installed
    # host preset, which bypasses the upstream tests/CMakeLists.txt).
    add_executable(aurora_vita_view_draw_capture_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_view_draw_capture_test.cpp)
    target_include_directories(aurora_vita_view_draw_capture_test PRIVATE
        ${AURORA_VITA_SOURCE_DIR}/include ${AURORA_VITA_SOURCE_DIR}/platforms/vita)
    add_test(NAME vita_view_draw_capture COMMAND aurora_vita_view_draw_capture_test)
    add_executable(aurora_vita_draw_payload_hash_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_draw_payload_hash_test.cpp)
    target_include_directories(aurora_vita_draw_payload_hash_test PRIVATE
        ${AURORA_VITA_SOURCE_DIR}/include ${AURORA_VITA_SOURCE_DIR}/platforms/vita)
    add_test(NAME vita_draw_payload_hash COMMAND aurora_vita_draw_payload_hash_test)
    add_executable(aurora_vita_vertex_uniform_delta_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_vertex_uniform_delta_test.cpp)
    add_test(NAME vita_vertex_uniform_delta COMMAND aurora_vita_vertex_uniform_delta_test)
    add_executable(aurora_vita_tev_opt_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_tev_opt_test.cpp)
    target_include_directories(aurora_vita_tev_opt_test PRIVATE
        ${AURORA_VITA_SOURCE_DIR}/platforms/vita)
    add_test(NAME vita_tev_opt COMMAND aurora_vita_tev_opt_test)
    add_executable(aurora_vita_shader_debug_registry_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_shader_debug_registry_test.cpp)
    target_include_directories(aurora_vita_shader_debug_registry_test PRIVATE
        ${AURORA_VITA_SOURCE_DIR}/include ${AURORA_VITA_SOURCE_DIR}/platforms/vita)
    find_package(Threads REQUIRED)
    target_link_libraries(aurora_vita_shader_debug_registry_test PRIVATE Threads::Threads)
    add_test(NAME vita_shader_debug_registry COMMAND aurora_vita_shader_debug_registry_test)
    # Compile the real Dawn-free GX bridge on the host, rather than relying on
    # a stub ShaderConfig whose layout could hide frontend regressions.
    if(AURORA_VITA_WITH_GX_FRONTEND)
        find_package(Threads REQUIRED)
        target_sources(aurora_vita_backend PRIVATE
            ${AURORA_VITA_SOURCE_DIR}/tests/vita_frontend_thread_stubs.cpp)
        target_include_directories(aurora_vita_backend PRIVATE
            ${AURORA_VITA_SOURCE_DIR}/tests/vita_stubs)
        target_link_libraries(aurora_vita_backend PRIVATE Threads::Threads)
        add_executable(aurora_vita_frontend_translation_test
            ${AURORA_VITA_SOURCE_DIR}/tests/vita_frontend_translation_test.cpp)
        target_link_libraries(aurora_vita_frontend_translation_test PRIVATE aurora::vita_backend)
        add_test(NAME vita_frontend_translation COMMAND aurora_vita_frontend_translation_test)
        add_executable(aurora_vita_native_model_recipe_test
            ${AURORA_VITA_SOURCE_DIR}/tests/vita_native_model_recipe_test.cpp)
        target_link_libraries(aurora_vita_native_model_recipe_test PRIVATE aurora::vita_backend)
        add_test(NAME vita_native_model_recipe COMMAND aurora_vita_native_model_recipe_test)
        add_executable(aurora_vita_native_model_textured_test
            ${AURORA_VITA_SOURCE_DIR}/tests/vita_native_model_textured_test.cpp)
        target_link_libraries(aurora_vita_native_model_textured_test PRIVATE aurora::vita_backend)
        add_test(NAME vita_native_model_textured COMMAND aurora_vita_native_model_textured_test)
        add_test(NAME vita_native_model_recipe_cache COMMAND aurora_vita_native_model_recipe_test)
        add_test(NAME vita_native_model_textured_cache COMMAND aurora_vita_native_model_textured_test)
        set_tests_properties(vita_native_model_recipe_cache vita_native_model_textured_cache PROPERTIES
            ENVIRONMENT "STRIKERS_GXM_NATIVE_MODEL_CACHE=1;STRIKERS_GXM_NATIVE_MODEL_CENSUS=1")
        add_executable(aurora_vita_native_model_cache_test
            ${AURORA_VITA_SOURCE_DIR}/tests/vita_native_model_cache_test.cpp)
        target_link_libraries(aurora_vita_native_model_cache_test PRIVATE aurora::vita_backend)
        add_test(NAME vita_native_model_cache COMMAND aurora_vita_native_model_cache_test)
        add_executable(aurora_vita_native_model_census_test
            ${AURORA_VITA_SOURCE_DIR}/tests/vita_native_model_census_test.cpp)
        target_link_libraries(aurora_vita_native_model_census_test PRIVATE aurora::vita_backend)
        add_test(NAME vita_native_model_census COMMAND aurora_vita_native_model_census_test)
    endif()
    add_executable(aurora_vita_command_stream_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_command_stream_test.cpp)
    target_link_libraries(aurora_vita_command_stream_test PRIVATE aurora::vita_common)
    add_test(NAME vita_command_stream COMMAND aurora_vita_command_stream_test)
    find_package(Threads REQUIRED)
    add_executable(aurora_vita_submission_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_submission_test.cpp)
    target_link_libraries(aurora_vita_submission_test PRIVATE aurora::vita_backend Threads::Threads)
    add_test(NAME vita_submission COMMAND aurora_vita_submission_test)
    add_executable(aurora_vita_cpu_workers_test
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_cpu_workers_test.cpp
        ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_cpu_workers.cpp)
    target_compile_features(aurora_vita_cpu_workers_test PRIVATE cxx_std_20)
    target_compile_definitions(aurora_vita_cpu_workers_test PRIVATE __vita__=1)
    target_include_directories(aurora_vita_cpu_workers_test PRIVATE
        ${AURORA_VITA_SOURCE_DIR}/tests/vita_stubs ${AURORA_VITA_SOURCE_DIR}/platforms/vita)
    target_link_libraries(aurora_vita_cpu_workers_test PRIVATE Threads::Threads)
    add_test(NAME vita_cpu_workers COMMAND aurora_vita_cpu_workers_test)
    # The host shim intentionally exercises thousands of semaphore round-trips. macOS
    # scheduling can take several minutes even when every per-wait 3-second
    # lost-wake guard passes, so keep CTest from pre-empting a valid run.
    set_tests_properties(vita_cpu_workers PROPERTIES TIMEOUT 420)
    add_test(NAME vita_renderer_selection
        COMMAND ${CMAKE_COMMAND}
            -DSELECTION_MODULE=${CMAKE_CURRENT_LIST_DIR}/AuroraVitaRendererSelection.cmake
            -P ${AURORA_VITA_SOURCE_DIR}/tests/vita_renderer_selection_test.cmake)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME vita_build_manifest
            COMMAND ${Python3_EXECUTABLE} ${AURORA_VITA_SOURCE_DIR}/tests/vita_build_manifest_test.py)
        add_test(NAME vita_performance_compare
            COMMAND ${Python3_EXECUTABLE} ${AURORA_VITA_SOURCE_DIR}/tests/vita_performance_compare_test.py)
        add_test(NAME vita_binary_audit_contract
            COMMAND ${Python3_EXECUTABLE} ${AURORA_VITA_SOURCE_DIR}/tests/vita_binary_audit_test.py)
    endif()
endif ()
