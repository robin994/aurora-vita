#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opt-in CPU diagnostic. Records actual GXM draw submissions and FIFO-ordered
// view markers. No GPU finishes or shader suppression are involved.
enum { AURORA_VIEW_DRAW_MARKER = 1, AURORA_VIEW_DRAW_DRAW = 2,
       AURORA_VIEW_DRAW_FRAME_COMPLETE = 3 };

typedef struct AuroraViewDrawRecord {
    uint64_t sequence;
    uint64_t producer_frame;
    uint64_t consumer_frame;
    uint64_t pipeline_requested;
    uint64_t pipeline_active;
    uint64_t vertex_hash;
    uint64_t fragment_hash;
    uint64_t vertex_payload_hash;
    uint64_t index_payload_hash;
    uint64_t uniform_payload_hash;
    uint64_t draw_state_hash;
    uint64_t submit_cpu_us;
    uint64_t uniform_bytes;
    uint32_t logical_draw;
    uint32_t target;
    uint32_t vertex_count;
    uint32_t index_count;
    uint8_t type;
    uint8_t view;
    uint8_t tev_stages;
    uint8_t texture_mask;
    uint8_t texgen_mask;
    uint8_t program_native;
    uint8_t indexed_pn;
    uint8_t blend;
    uint8_t depth;
    uint8_t alpha;
    uint8_t scissor;
    uint8_t alpha_ref0;
    uint8_t alpha_ref1;
    uint8_t alpha_op;
    uint8_t payload_hashes_present;
} AuroraViewDrawRecord;

// Start/stop/read ONLY from the game thread after the frame has been joined.
int aurora_vita_view_draw_start(size_t maximum_records);
// Additional byte-hashing cost is limited to diagnostics and disabled by
// default. Consumer and game thread may access this after the frame fence.
int aurora_vita_view_draw_start_ex(size_t maximum_records, int capture_payloads);
void aurora_vita_view_draw_stop(void);
size_t aurora_vita_view_draw_count(void);
size_t aurora_vita_view_draw_capacity(void);
uint64_t aurora_vita_view_draw_lost(void);
size_t aurora_vita_view_draw_read(size_t start, AuroraViewDrawRecord* out, size_t max_records);

// Called by the game before/after its view sequence. Markers travel in FIFO
// order; the producer never modifies the worker's current-view state directly.
void aurora_vita_view_draw_mark(uint32_t view, uint64_t producer_frame);

#ifdef __cplusplus
}
#endif
