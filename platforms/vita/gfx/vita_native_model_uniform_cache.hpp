#pragma once

#include "vita_vertex_pipeline.hpp"
#include "../../../lib/gx/native_model_options.hpp"
#include "../../../lib/gx/state_revisions.hpp"
#include <algorithm>
#include <cstdint>

namespace aurora::vita::gfx {

inline uint64_t fragment_uniform_revision(const PipelineDesc& pipeline,
                                         const aurora::gx::StateRevisions& revisions) noexcept {
  uint64_t revision = revisions.fragment;
  // Fog reads viewport coordinates; indirect TEV reads texture size/LOD.
  // Domain revisions share the same monotonic serial, so max preserves changes.
  if (pipeline.fogMode != FogMode::None) revision = std::max(revision, revisions.vertex);
  if (pipeline.tev.indirectStageCount) revision = std::max(revision, revisions.textures);
  return revision;
}

struct NativeModelUniformStamp {
  uint64_t recipe = 0, vertex = 0, fragment = 0;
  uint32_t stateIdentity = 0, vertexProgram = 0;
};

// One owner-thread CPU snapshot, independent of the bounded resident meshes.
// Queued draws receive an immutable pool publication; no transient GPU uniform
// address is cached here, and no reservation is shared across GXM scenes.
class NativeModelUniformCache {
  NativeModelUniformStamp stamp_{};
  bool valid_ = false;
  VertexTransformState vertex_{};
  DrawUniforms uniforms_{};

 public:
  struct Update { bool vertex = false, fragment = false; };

  template<class Build>
  Update prepare(const NativeModelUniformStamp& stamp, bool force, Build&& build) noexcept {
    const bool changedMaterial = !valid_ || stamp.recipe != stamp_.recipe ||
        stamp.stateIdentity != stamp_.stateIdentity || stamp.vertexProgram != stamp_.vertexProgram;
    Update update{force || changedMaterial || stamp.vertex != stamp_.vertex,
                  force || changedMaterial || stamp.fragment != stamp_.fragment};
    if (force || changedMaterial) {
      vertex_ = {};
      uniforms_ = {};
    }
    if (update.vertex || update.fragment)
      build(vertex_, uniforms_, update.vertex, update.fragment);
    stamp_ = stamp;
    valid_ = true;
    return update;
  }

  void invalidate() noexcept { valid_ = false; }
  const VertexTransformState& vertex() const noexcept { return vertex_; }
  const DrawUniforms& uniforms() const noexcept { return uniforms_; }
};

} // namespace aurora::vita::gfx
