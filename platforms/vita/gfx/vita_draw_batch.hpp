#pragma once
#include "vita_gfx_types.hpp"
#include <cstring>
#include "vita_byte_compare.h"

namespace aurora::vita::gfx {
// Verify all render state before considering a merge. The pending arena may
// contain alignment padding between otherwise compatible GPU buffer slices;
// compacting such padding is separate from proving state equivalence.
inline bool local_draw_states_mergeable(const DrawPacket& a, const DrawPacket& b) noexcept {
  if (!a.pipelineKey || a.pipelineKey != b.pipelineKey ||
      a.absoluteVertexIndices || b.absoluteVertexIndices ||
      a.firstVertex || b.firstVertex || a.instanceCount != 1 || b.instanceCount != 1 ||
      !a.indexCount || !b.indexCount ||
      a.indexCount % 3 || b.indexCount % 3 || !a.vertexCount || !b.vertexCount ||
      uint64_t(a.vertexCount) + b.vertexCount >= 64000u) return false;
  if (a.fixedVertexUniforms != b.fixedVertexUniforms) {
    if (!a.fixedVertexUniforms || !b.fixedVertexUniforms ||
        !aurora_vita_bytes_equal(a.fixedVertexUniforms,b.fixedVertexUniforms,sizeof(FixedVertexUniforms))) return false;
  }
  if (a.vertices.buffer != b.vertices.buffer || a.indices.buffer != b.indices.buffer) return false;
  if (a.viewport.x != b.viewport.x || a.viewport.y != b.viewport.y ||
      a.viewport.width != b.viewport.width || a.viewport.height != b.viewport.height ||
      a.viewport.znear != b.viewport.znear || a.viewport.zfar != b.viewport.zfar ||
      a.scissor.x != b.scissor.x || a.scissor.y != b.scissor.y ||
      a.scissor.width != b.scissor.width || a.scissor.height != b.scissor.height ||
      (&a.gpu_uniforms()!=&b.gpu_uniforms()&&
       !aurora_vita_bytes_equal(&a.gpu_uniforms(), &b.gpu_uniforms(), sizeof(a.uniforms)))) return false;
  if(&a.texture_bindings()==&b.texture_bindings())return true;
  for (unsigned i = 0; i < MaxTextures; ++i) {
    const auto& x = a.texture_bindings()[i]; const auto& y = b.texture_bindings()[i];
    if (x.texture != y.texture || x.source != y.source || x.flipX != y.flipX ||
        x.flipY != y.flipY || x.forceOpaque != y.forceOpaque || x.sampleFormat != y.sampleFormat ||
        x.sampler.wrapS != y.sampler.wrapS || x.sampler.wrapT != y.sampler.wrapT ||
        x.sampler.minFilter != y.sampler.minFilter || x.sampler.magFilter != y.sampler.magFilter ||
        x.sampler.lodBias != y.sampler.lodBias || x.sampler.minLod != y.sampler.minLod ||
        x.sampler.maxLod != y.sampler.maxLod ||
        x.uvScaleX != y.uvScaleX || x.uvScaleY != y.uvScaleY ||
        x.uvBiasX != y.uvBiasX || x.uvBiasY != y.uvBiasY) return false;
  }
  return true;
}
// Original strict predicate: no byte movements, only exactly adjacent slices.
inline bool local_draws_mergeable(const DrawPacket& a, const DrawPacket& b) noexcept {
  return local_draw_states_mergeable(a,b) &&
      uint64_t(a.vertices.offset) + a.vertices.size == b.vertices.offset &&
      uint64_t(a.indices.offset) + a.indices.size == b.indices.offset;
}
} // namespace aurora::vita::gfx
