# One frontend/source contract, shared by both hardware implementations and by
# downstream ports. The hardware files never supply an alternative GX decoder.
function(aurora_vita_attach_frontend target)
    target_sources(${target} PRIVATE
        ${AURORA_VITA_SOURCE_DIR}/platforms/vita/aurora_vita_backend.cpp
        ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gx/aurora_vita_draw_sink.cpp
        ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_memory_budget.cpp
        ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_streaming_arena.cpp
        ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx/vita_draw_adapter.cpp)
    foreach(name vita_feature_coverage vita_frame_trace vita_fifo_packet_queue
                 vita_gx_capture vita_gx_replay wiicompiled_aurora_adapter vita_gx_backend)
        target_sources(${target} PRIVATE ${AURORA_VITA_SOURCE_DIR}/platforms/vita/integration/${name}.cpp)
    endforeach()
    target_include_directories(${target} PUBLIC
        ${AURORA_VITA_SOURCE_DIR}/include
        ${AURORA_VITA_SOURCE_DIR}/lib
        ${AURORA_VITA_SOURCE_DIR}/platforms/vita
        ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gfx
        ${AURORA_VITA_SOURCE_DIR}/platforms/vita/integration)
    target_compile_definitions(${target} PUBLIC AURORA_PLATFORM_VITA=1 AURORA_GFX_VITA=1)
    if(AURORA_VITA_WITH_GX_FRONTEND OR AURORA_VITA_WITH_UPSTREAM_GX)
        target_sources(${target} PRIVATE ${AURORA_VITA_SOURCE_DIR}/platforms/vita/gx/aurora_gx_bridge.cpp)
        # DrawSink's public layout contains frontend fields under this define.
        # Keeping it PRIVATE gives consumers a different C++ object layout.
        target_compile_definitions(${target} PUBLIC AURORA_VITA_UPSTREAM=1)
    endif()
    if(AURORA_VITA_WITH_GX_FRONTEND)
        target_sources(${target} PRIVATE
            ${AURORA_VITA_SOURCE_DIR}/lib/vita/runtime.cpp
            ${AURORA_VITA_SOURCE_DIR}/lib/vita/gx_frontend_state.cpp
            ${AURORA_VITA_SOURCE_DIR}/lib/gx/fifo.cpp
            ${AURORA_VITA_SOURCE_DIR}/lib/gx/command_processor.cpp
            ${AURORA_VITA_SOURCE_DIR}/lib/dolphin/vi/vi.cpp)
        foreach(name GXBump GXCull GXCpu2Efb GXDispList GXDraw GXExtra GXFifo GXFrameBuffer
                     GXGeometry GXGet GXLighting GXManage GXPixel GXTev GXTexture GXTransform GXVert GXAurora)
            target_sources(${target} PRIVATE ${AURORA_VITA_SOURCE_DIR}/lib/dolphin/gx/${name}.cpp)
        endforeach()
        target_compile_definitions(${target} PUBLIC MKW_TARGET_VITA=1 TARGET_PC=1)
        if(AURORA_VITA_ASYNC_GX)
            target_compile_definitions(${target} PRIVATE AURORA_VITA_ASYNC_GX=1)
        endif()
    endif()
endfunction()
