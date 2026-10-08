#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Diagnostic fragment-program catalogue. IDs are hashes of the exact Cg stage
// source, not scene-local pipeline indices. Draw counts include suppressed draws.
typedef struct AuroraVitaDebugFragment {
    uint64_t shader_hash;
    uint64_t draws;
    uint64_t skipped_draws;
    uint8_t tev_stages;
    uint8_t native_material;
    uint8_t enabled;
    uint8_t reserved;
} AuroraVitaDebugFragment;

// Configure at boot. Disabled is the zero-overhead production default.
void aurora_vita_shader_debug_capture(int enabled);
// Thread-safe snapshot; returns number of entries written (up to capacity).
size_t aurora_vita_shader_debug_snapshot(AuroraVitaDebugFragment* out, size_t capacity);
// Restore all suppressed draws, or selectively enable/disable one fragment shader.
int aurora_vita_shader_debug_set_enabled(uint64_t shader_hash, int enabled);
void aurora_vita_shader_debug_restore_all(void);
// Keep the pause/debug menu visible regardless of the selected shader filter.
void aurora_vita_shader_debug_bypass(int enabled);

#ifdef __cplusplus
}
#endif
