#include "gfx/vita_shader_debug_registry.hpp"
#include <array>
#include <atomic>
#ifdef NDEBUG
#undef NDEBUG // Assertions are the test contract, including in Release builds.
#endif
#include <cassert>
#include <cstdio>
#include <thread>

using aurora::vita::gfx::ShaderDebugRegistry;

int main() {
    ShaderDebugRegistry registry;
    assert(!registry.recording());
    assert(registry.register_fragment(0x11, 2, true) == nullptr);
    registry.set_recording(true);

    auto* alpha = registry.register_fragment(0x11, 2, true);
    auto* beta = registry.register_fragment(0x22, 4, false);
    assert(alpha && beta && alpha != beta);
    assert(registry.register_fragment(0x11, 2, true) == alpha);
    assert(registry.register_fragment(0, 2, true) == nullptr);

    std::array<AuroraVitaDebugFragment, 3> seen{};
    assert(registry.snapshot(seen.data(), seen.size()) == 0);
    assert(!registry.skip_draw(alpha));
    assert(!registry.skip_draw(beta));
    assert(registry.set_enabled(0x11, false));
    assert(!registry.set_enabled(0x33, false));
    assert(registry.skip_draw(alpha));
    registry.set_bypass(true);
    assert(!registry.skip_draw(alpha)); // Pause/HUD is never suppressed.
    registry.set_bypass(false);
    assert(registry.skip_draw(alpha));
    assert(registry.snapshot(seen.data(), 1) == 1);
    assert(registry.snapshot(seen.data(), seen.size()) == 2);
    assert(seen[0].shader_hash == 0x11 && seen[0].draws == 3 && seen[0].skipped_draws == 2);
    assert(seen[0].tev_stages == 2 && seen[0].native_material == 1 && seen[0].enabled == 0);
    assert(seen[1].shader_hash == 0x22 && seen[1].draws == 1 && seen[1].enabled == 1);

    // Concurrent menu writes/snapshots cannot race the per-draw atomics.
    std::atomic<bool> done{false};
    std::thread toggler([&] {
        for (int i = 0; i < 2000; ++i) {
            assert(registry.set_enabled(0x11, (i & 1) != 0));
            if (i % 7 == 0) registry.snapshot(seen.data(), seen.size());
        }
        done.store(true);
    });
    while (!done.load()) registry.skip_draw(alpha);
    toggler.join();

    registry.restore_all();
    assert(!registry.skip_draw(alpha));
    registry.set_recording(false);
    assert(!registry.skip_draw(alpha));
    registry.set_recording(true);
    registry.clear_catalog();
    assert(registry.snapshot(seen.data(), seen.size()) == 0);

    for (size_t i = 0; i < ShaderDebugRegistry::MaxFragments; ++i)
        assert(registry.register_fragment(i + 1, 1, true) != nullptr);
    assert(registry.register_fragment(ShaderDebugRegistry::MaxFragments + 1, 1, true) == nullptr);
    std::puts("Vita shader debug registry: thread safety, filtering, restore, capacity passed");
}
