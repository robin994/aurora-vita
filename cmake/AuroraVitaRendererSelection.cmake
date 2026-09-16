# Shared by Aurora and downstream source-list builds. Select one context owner
# at configure time; unsupported integrations must not silently fall back.
set(AURORA_VITA_RENDERER "VITAGL" CACHE STRING "Vita hardware renderer: VITAGL or GXM (SceGxm)")
set_property(CACHE AURORA_VITA_RENDERER PROPERTY STRINGS VITAGL GXM)
if (NOT AURORA_VITA_RENDERER MATCHES "^(VITAGL|GXM)$")
    message(FATAL_ERROR "AURORA_VITA_RENDERER must be VITAGL or GXM")
endif ()
