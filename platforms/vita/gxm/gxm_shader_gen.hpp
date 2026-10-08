#pragma once
#include "gfx/vita_gfx_types.hpp"
#include <string>

namespace aurora::vita::gxm {
struct ShaderSources {
  std::string vertex;
  std::string fragment;
  std::string error;
  uint8_t textureMask = 0;
  uint8_t texcoordMask = 0;
  uint8_t colorMask = 0;
  bool discardAll = false;
  bool nativeMaterial = false;
  // A4 compiler-time savings; never interpreted as measured GPU execution.
  uint8_t a4ReusedSamples = 0;
  uint8_t a4RemovedColorChannels = 0;
  uint8_t a4RemovedAlphaChannels = 0;
  uint8_t a4RemovedStages = 0;
  bool ok() const noexcept { return error.empty() && !vertex.empty() && !fragment.empty(); }
};
// Native Cg emission from the shared TEV description, not a GLSL transpiler.
// Unsupported GX features are rejected rather than silently approximated.
ShaderSources build_tev_cg(const gfx::PipelineDesc& desc);
// Arithmetic materials are lowered once to direct immutable expressions.
// Compare/indirect effects retain the reference generator. No frame-time TEV
// register interpreter is introduced, and the two paths share sampling/epilogue.
// mask: 1=deduplicate identical texture/coord fetches, 2=remove TEV register
// channels with no path to the final color/alpha. Zero is byte-identical to
// the original shader source; it is the permanent reference/fallback.
ShaderSources build_material_cg(const gfx::PipelineDesc& desc, bool native = true, uint8_t mask = 0);
bool native_material_supported(const gfx::PipelineDesc& desc) noexcept;
std::string validate_pipeline(const gfx::PipelineDesc& desc);
} // namespace aurora::vita::gxm
