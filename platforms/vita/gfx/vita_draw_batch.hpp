#pragma once
#include "vita_gfx_types.hpp"
#include <cstring>
#include "vita_byte_compare.h"

namespace aurora::vita::gfx {
// Only adjacent, CPU-prepared triangle lists may use this predicate. Their
// caller rebases the second index range to the first draw before submission.
inline bool local_draws_mergeable(const DrawPacket& a, const DrawPacket& b) noexcept {
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
  if (a.vertices.buffer != b.vertices.buffer || a.indices.buffer != b.indices.buffer ||
      uint64_t(a.vertices.offset) + a.vertices.size != b.vertices.offset ||
      uint64_t(a.indices.offset) + a.indices.size != b.indices.offset) return false;
  if (a.viewport.x != b.viewport.x || a.viewport.y != b.viewport.y ||
      a.viewport.width != b.viewport.width || a.viewport.height != b.viewport.height ||
      a.viewport.znear != b.viewport.znear || a.viewport.zfar != b.viewport.zfar ||
      a.scissor.x != b.scissor.x || a.scissor.y != b.scissor.y ||
      a.scissor.width != b.scissor.width || a.scissor.height != b.scissor.height ||
      !aurora_vita_bytes_equal(&a.uniforms, &b.uniforms, sizeof(a.uniforms))) return false;
  for (unsigned i = 0; i < MaxTextures; ++i) {
    const auto& x = a.textures[i]; const auto& y = b.textures[i];
    if (x.texture != y.texture || x.source != y.source || x.flipX != y.flipX ||
        x.flipY != y.flipY || x.forceOpaque != y.forceOpaque || x.sampleFormat != y.sampleFormat ||
        x.sampler.wrapS != y.sampler.wrapS || x.sampler.wrapT != y.sampler.wrapT ||
        x.sampler.minFilter != y.sampler.minFilter || x.sampler.magFilter != y.sampler.magFilter ||
        x.sampler.lodBias != y.sampler.lodBias || x.sampler.minLod != y.sampler.minLod ||
        x.sampler.maxLod != y.sampler.maxLod) return false;
  }
  return true;
}
} // namespace aurora::vita::gfx
