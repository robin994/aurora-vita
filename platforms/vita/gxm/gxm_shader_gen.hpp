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
  bool ok() const noexcept { return error.empty() && !vertex.empty() && !fragment.empty(); }
};
// Native Cg emission from the shared TEV description, not a GLSL transpiler.
// Unsupported GX features are rejected rather than silently approximated.
ShaderSources build_tev_cg(const gfx::PipelineDesc& desc);
// Arithmetic materials are lowered once to direct immutable expressions.
// Compare/indirect effects retain the reference generator. No frame-time TEV
// register interpreter is introduced, and the two paths share sampling/epilogue.
ShaderSources build_material_cg(const gfx::PipelineDesc& desc, bool native = true);
bool native_material_supported(const gfx::PipelineDesc& desc) noexcept;
std::string validate_pipeline(const gfx::PipelineDesc& desc);
} // namespace aurora::vita::gxm
