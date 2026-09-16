# Build just Aurora's selected Vita backend inside another project, without
# changing that project's PROJECT_SOURCE_DIR or loading desktop dependencies.
function(aurora_add_vita_backend)
    if(TARGET aurora::vita_backend)
        message(FATAL_ERROR "Aurora Vita backend already exists in this build")
    endif()
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/aurora_vita.cmake")
endfunction()
