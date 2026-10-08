#include "gfx/vita_view_draw_capture.hpp"
#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>

using aurora::vita::gfx::ViewDrawCapture;

int main() {
    ViewDrawCapture capture;
    assert(!capture.enabled());
    assert(!capture.payload_enabled());
    assert(!capture.start(0));
    assert(!capture.start(65537));
    assert(capture.start(16, true));
    assert(!capture.start(16));
    assert(capture.enabled());
    assert(capture.payload_enabled());
    assert(capture.read(0,nullptr,0) == 0);

    capture.mark(900,255);
    capture.mark(900,3);
    AuroraViewDrawRecord first{};
    first.consumer_frame = 25; // No assumption of identical producer/consumer epochs.
    first.pipeline_requested = 0x1234;
    first.pipeline_active = 0x5678;
    first.fragment_hash = 0xaa12;
    first.vertex_count = 120;
    first.payload_hashes_present = 1;
    first.vertex_payload_hash = 0x123456abcdefULL;
    first.uniform_payload_hash = 0xcafefeedabcdULL;
    capture.draw(first);
    capture.mark(900,11);
    first.fragment_hash = 0xbb34;
    capture.draw(first);
    capture.mark(900,255);
    capture.complete(25);
    capture.mark(901,255);
    capture.mark(901,3);
    capture.draw(first);
    capture.complete(26);
    capture.stop();
    assert(!capture.payload_enabled());

    std::array<AuroraViewDrawRecord,16> events{};
    const size_t n = capture.read(0,events.data(),events.size());
    assert(n == 11);
    for (size_t i = 0; i < n; ++i) assert(events[i].sequence == i + 1);
    assert(events[2].type == AURORA_VIEW_DRAW_DRAW && events[2].view == 3);
    assert(events[4].type == AURORA_VIEW_DRAW_DRAW && events[4].view == 11);
    assert(events[2].pipeline_requested == 0x1234);
    assert(events[2].pipeline_active == 0x5678);
    assert(events[2].fragment_hash == 0xaa12);
    assert(events[2].payload_hashes_present == 1);
    assert(events[2].vertex_payload_hash == 0x123456abcdefULL);
    assert(events[2].uniform_payload_hash == 0xcafefeedabcdULL);
    assert(events[5].type == AURORA_VIEW_DRAW_MARKER && events[5].view == 255);
    assert(events[6].type == AURORA_VIEW_DRAW_FRAME_COMPLETE && events[6].consumer_frame == 25);
    assert(events[9].type == AURORA_VIEW_DRAW_DRAW && events[9].view == 3);
    assert(events[10].type == AURORA_VIEW_DRAW_FRAME_COMPLETE);
    assert(capture.lost() == 0);

    assert(capture.start(2));
    assert(!capture.payload_enabled());
    capture.mark(902,3);
    capture.draw(first);
    capture.complete(27);
    capture.stop();
    assert(capture.count() == 2 && capture.lost() == 1);

    assert(capture.start(4));
    capture.draw(first); // No view marker: cannot attribute a draw silently.
    capture.mark(901,34); // Invalid view index.
    capture.mark(903,3);
    capture.suppressed_draw(); // Disabled shader must invalidate the capture.
    capture.stop();
    assert(capture.lost() == 3);
    std::puts("View/draw capture: ordered views, frame boundaries, bounded loss passed");
}
