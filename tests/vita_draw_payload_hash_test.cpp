#include "gfx/vita_draw_payload_hash.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <cstdio>
#include <cstring>

using namespace aurora::vita::gfx;

int main() {
    // Equal indexed vertex bytes in different allocations/offsets hash equally.
    std::array<uint8_t, 32> original{};
    for (unsigned i = 0; i < original.size(); ++i)
        original[i] = static_cast<uint8_t>(i * 7 + 1);
    std::array<uint8_t, 48> relocated{};
    std::memcpy(relocated.data() + 16, original.data(), original.size());
    const std::array<uint16_t, 3> indices{{2, 1, 2}};
    uint64_t v1 = 0, i1 = 0, v2 = 0, i2 = 0;
    assert(hash_indexed_draw_payload(original.data(), original.size(), 0, 0, 32, 8,
                                     indices.data(), 3, v1, i1));
    assert(hash_indexed_draw_payload(relocated.data(), relocated.size(), 16, 16, 32, 8,
                                     indices.data(), 3, v2, i2));
    assert(v1 == v2 && i1 == i2 && v1 != 0 && i1 != 0);
    relocated[16 + 8 + 2] ^= 1;
    assert(hash_indexed_draw_payload(relocated.data(), relocated.size(), 16, 16, 32, 8,
                                     indices.data(), 3, v2, i2));
    assert(v1 != v2 && i1 == i2); // A pose change, same draw topology.
    const std::array<uint16_t, 3> differentIndices{{1, 2, 1}};
    assert(hash_indexed_draw_payload(original.data(), original.size(), 0, 0, 32, 8,
                                     differentIndices.data(), 3, v2, i2));
    assert(i1 != i2);
    assert(!hash_indexed_draw_payload(original.data(), original.size(), 0, 0, 8, 8,
                                      indices.data(), 3, v2, i2));
    assert(!hash_indexed_draw_payload(original.data(), original.size(), 0, 0, 32, 0,
                                      indices.data(), 3, v2, i2));
    const std::array<uint16_t, 3> invalid{{2, 64000, 1}};
    assert(!hash_indexed_draw_payload(original.data(), original.size(), 0, 0, 32, 8,
                                      invalid.data(), 3, v2, i2));

    GpuDrawUniforms gpu{};
    FixedVertexUniforms fixed{};
    const auto withoutFixed = hash_draw_uniform_payload(gpu, nullptr);
    const auto withFixed = hash_draw_uniform_payload(gpu, &fixed);
    assert(withoutFixed != withFixed);
    fixed.revision = 100;
    assert(hash_draw_uniform_payload(gpu, &fixed) == withFixed);
    fixed.revision = 200;
    assert(hash_draw_uniform_payload(gpu, &fixed) == withFixed);
    fixed.positionPalette[7][5] += 0.5f;
    assert(hash_draw_uniform_payload(gpu, &fixed) != withFixed);
    const auto changed = hash_draw_uniform_payload(gpu, &fixed);
    gpu.mvp[3] = 4.f;
    assert(hash_draw_uniform_payload(gpu, &fixed) != changed);

    Viewport viewport{};
    Scissor scissor{};
    std::array<TextureBinding, MaxTextures> textures{};
    const auto rasterHash = hash_draw_state_payload(viewport, scissor, textures, 1);
    textures[0].sampler.minFilter = Filter::Nearest;
    assert(hash_draw_state_payload(viewport, scissor, textures, 1) != rasterHash);
    textures[0].sampler.minFilter = Filter::Linear;
    textures[0].texture = 999; // GXM resource handle identity is excluded.
    assert(hash_draw_state_payload(viewport, scissor, textures, 1) == rasterHash);
    scissor.x += 1;
    assert(hash_draw_state_payload(viewport, scissor, textures, 1) != rasterHash);
    scissor.x -= 1;
    viewport.zfar = -0.f;
    assert(hash_draw_state_payload(viewport, scissor, textures, 1) != rasterHash);

    std::puts("Vita payload hash: canonical geometry, uniforms and state passed");
}
