include_guard(GLOBAL)

# Bind and fingerprint the same archive. VitaGL program binaries contain private
# serializer data and cannot be shared across differently configured libraries.
function(aurora_vita_bind_vitagl target)
    find_library(AURORA_VITAGL_BINARY_LIBRARY NAMES vitaGL
        HINTS "${VITASDK}/arm-vita-eabi/lib" "$ENV{VITASDK}/arm-vita-eabi/lib"
        REQUIRED)
    mark_as_advanced(AURORA_VITAGL_BINARY_LIBRARY)
    file(SHA256 "${AURORA_VITAGL_BINARY_LIBRARY}" _aurora_vitagl_binary_abi)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${AURORA_VITAGL_BINARY_LIBRARY}")
    target_compile_definitions(${target} PRIVATE
        AURORA_VITAGL_CACHE_ABI="${_aurora_vitagl_binary_abi}")
    target_link_libraries(${target} PUBLIC "${AURORA_VITAGL_BINARY_LIBRARY}")
endfunction()
