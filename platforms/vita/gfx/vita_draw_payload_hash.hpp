#pragma once

#include "gfx/vita_gfx_types.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <type_traits>

namespace aurora::vita::gfx {

// Diagnostic-only FNV-1a 64-bit hashes. These are collision-resistant enough
// to flag likely regressions, NOT cryptographic proof of GPU/pixel equivalence.
constexpr uint64_t PayloadHashOffset = UINT64_C(14695981039346656037);
constexpr uint64_t PayloadHashPrime = UINT64_C(1099511628211);

inline uint64_t append_payload_bytes(uint64_t hash, const void* pointer, size_t size) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(pointer);
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * PayloadHashPrime;
    return hash;
}

inline void append_payload_u32(uint64_t& hash, uint32_t word) noexcept {
    // Specify byte order so host and Vita produce the same hashes.
    for (unsigned i = 0; i < 4; ++i)
        hash = (hash ^ static_cast<uint8_t>(word >> (i * 8))) * PayloadHashPrime;
}

inline void append_payload_float(uint64_t& hash, float value) noexcept {
    uint32_t bits = 0;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&bits, &value, sizeof(bits));
    append_payload_u32(hash, bits);
}

struct DrawPayloadHashes {
    uint64_t vertex = 0;
    uint64_t index = 0;
    uint64_t uniforms = 0;
    uint64_t state = 0;
};

// The caller supplies the vertex/index buffers at the instant GXM submits the
// draw. Different GPU addresses or allocation offsets cannot alter the hash.
// Hash *referenced* stride bytes in index order to catch stale skinning poses
// even when topology, pipeline, and draw count are unchanged.
inline bool hash_indexed_draw_payload(const void* vertices, size_t vertexBytes,
                                      size_t base, size_t declaredSliceOffset,
                                      size_t declaredSliceBytes, unsigned stride,
                                      const uint16_t* indices, unsigned indexCount,
                                      uint64_t& vertexHash, uint64_t& indexHash) noexcept {
    if (!vertices || !indices || !stride || stride > 4096 || !indexCount ||
        indexCount > 65536 || declaredSliceOffset > vertexBytes ||
        declaredSliceBytes > vertexBytes - declaredSliceOffset)
        return false;
    const auto* data = static_cast<const uint8_t*>(vertices);
    uint64_t vertexDigest = PayloadHashOffset;
    uint64_t indexDigest = PayloadHashOffset;
    append_payload_u32(vertexDigest, stride);
    append_payload_u32(indexDigest, indexCount);
    // U16 indices are explicitly serialized in LE byte order for portability.
    for (unsigned i = 0; i < indexCount; ++i) {
        const uint32_t index = indices[i];
        append_payload_u32(indexDigest, index);
        const size_t offset = base + static_cast<size_t>(index) * stride;
        if (index >= 64000 || offset < base || offset < declaredSliceOffset ||
            offset > vertexBytes || stride > vertexBytes - offset ||
            offset - declaredSliceOffset > declaredSliceBytes ||
            stride > declaredSliceBytes - (offset - declaredSliceOffset))
            return false;
        vertexDigest = append_payload_bytes(vertexDigest, data + offset, stride);
    }
    vertexHash = vertexDigest;
    indexHash = indexDigest;
    return true;
}

inline uint64_t hash_draw_uniform_payload(const GpuDrawUniforms& u,
                                           const FixedVertexUniforms* fixed) noexcept {
    static_assert(std::is_trivially_copyable_v<GpuDrawUniforms>);
    static_assert(std::is_standard_layout_v<FixedVertexUniforms>);
    uint64_t hash = append_payload_bytes(PayloadHashOffset, &u, sizeof(u));
    append_payload_u32(hash, fixed ? 1u : 0u);
    if (fixed) {
        // Snapshot revision changes with storage identity, not shader inputs.
        // Excluding it makes equivalent snapshots compare equal.
        hash = append_payload_bytes(hash, fixed, offsetof(FixedVertexUniforms, revision));
    }
    return hash;
}

inline uint64_t hash_draw_state_payload(const Viewport& vp, const Scissor& scissor,
                                         const std::array<TextureBinding, MaxTextures>& bindings,
                                         uint8_t usedMask) noexcept {
    uint64_t hash = PayloadHashOffset;
    for (float value : {vp.x, vp.y, vp.width, vp.height, vp.znear, vp.zfar})
        append_payload_float(hash, value);
    for (int32_t value : {scissor.x, scissor.y, scissor.width, scissor.height})
        append_payload_u32(hash, static_cast<uint32_t>(value));
    append_payload_u32(hash, usedMask);
    for (unsigned i = 0; i < MaxTextures; ++i) {
        if (!(usedMask & (1u << i))) continue;
        const auto& binding = bindings[i];
        // Deliberately exclude ephemeral GXM texture handles, but include all
        // explicitly selected sampler, UV, and EFB copy conversion state.
        append_payload_u32(hash, i);
        append_payload_u32(hash, static_cast<uint32_t>(binding.source));
        append_payload_u32(hash, static_cast<uint32_t>(binding.sampler.wrapS));
        append_payload_u32(hash, static_cast<uint32_t>(binding.sampler.wrapT));
        append_payload_u32(hash, static_cast<uint32_t>(binding.sampler.minFilter));
        append_payload_u32(hash, static_cast<uint32_t>(binding.sampler.magFilter));
        append_payload_float(hash, binding.sampler.lodBias);
        append_payload_float(hash, binding.sampler.minLod);
        append_payload_float(hash, binding.sampler.maxLod);
        append_payload_u32(hash, binding.flipX ? 1u : 0u);
        append_payload_u32(hash, binding.flipY ? 1u : 0u);
        append_payload_u32(hash, binding.forceOpaque ? 1u : 0u);
        append_payload_u32(hash, static_cast<uint32_t>(binding.sampleFormat));
        append_payload_float(hash, binding.uvScaleX);
        append_payload_float(hash, binding.uvScaleY);
        append_payload_float(hash, binding.uvBiasX);
        append_payload_float(hash, binding.uvBiasY);
    }
    return hash;
}

} // namespace aurora::vita::gfx
