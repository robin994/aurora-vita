#include "../platforms/vita/gfx/vita_shader_gen.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using aurora::vita::gfx::FogMode;
using aurora::vita::gfx::PipelineDesc;
using aurora::vita::gfx::build_tev_glsl;

TEST(VitaShaderDepth, ReversedZMatchesAuroraThenMapsToOpenGLClipRange) {
  PipelineDesc desc{};
  desc.reversedZ = true;
  desc.fogMode = FogMode::Linear;

  const auto shader = build_tev_glsl(desc);
  EXPECT_NE(shader.vertex.find("gl_Position.z=-gl_Position.z;"), std::string::npos);
  EXPECT_NE(shader.vertex.find("gl_Position.z=2.0*gl_Position.z-gl_Position.w;"), std::string::npos);
  EXPECT_NE(shader.fragment.find("float fd=gl_FragCoord.z"), std::string::npos);
}

TEST(VitaShaderDepth, ForwardZMatchesAuroraThenMapsToOpenGLClipRange) {
  PipelineDesc desc{};
  desc.reversedZ = false;
  desc.fogMode = FogMode::Linear;

  const auto shader = build_tev_glsl(desc);
  EXPECT_NE(shader.vertex.find("gl_Position.z+=gl_Position.w;"), std::string::npos);
  EXPECT_NE(shader.vertex.find("gl_Position.z=2.0*gl_Position.z-gl_Position.w;"), std::string::npos);
  EXPECT_NE(shader.fragment.find("float fd=(1.0-gl_FragCoord.z)"), std::string::npos);
}

} // namespace
