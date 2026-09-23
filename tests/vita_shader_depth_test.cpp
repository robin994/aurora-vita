#include "../platforms/vita/gfx/vita_shader_gen.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using aurora::vita::gfx::FogMode;
using aurora::vita::gfx::PipelineDesc;
using aurora::vita::gfx::build_tev_glsl;
using aurora::vita::gfx::AlphaTestStaticResult;
using aurora::vita::gfx::Compare;
using aurora::vita::gfx::alpha_compare_static_result;

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

TEST(VitaShaderInputs, StreamsOnlyTexcoordsUsedByPipeline) {
  PipelineDesc desc{};
  desc.texgenCount = 1;
  desc.tev.stageCount = 1;
  desc.tev.stages[0].texCoord = 0;
  desc.tev.stages[0].texture = 0;
  desc.tev.stages[0].color.a = aurora::vita::gfx::TevColorArg::TexColor;

  EXPECT_EQ(aurora::vita::gfx::pipeline_texcoord_mask(desc), 0x01u);
  const auto shader = build_tev_glsl(desc);
  EXPECT_NE(shader.vertex.find("attribute vec3 a_tex0"), std::string::npos);
  EXPECT_EQ(shader.vertex.find("attribute vec3 a_tex1"), std::string::npos);
  EXPECT_EQ(shader.vertex.find("attribute vec4 a_color0"), std::string::npos);
  EXPECT_EQ(shader.vertex.find("attribute vec4 a_color1"), std::string::npos);
  EXPECT_NE(shader.fragment.find("varying vec3 v_tex0"), std::string::npos);
  EXPECT_EQ(shader.fragment.find("varying vec3 v_tex1"), std::string::npos);
  EXPECT_NE(shader.fragment.find("uniform sampler2D u_tex0"), std::string::npos);
  EXPECT_EQ(shader.fragment.find("uniform sampler2D u_tex1"), std::string::npos);
}

TEST(VitaShaderInputs, IndirectStageAddsItsLookupTexcoord) {
  PipelineDesc desc{};
  desc.tev.stageCount = 1;
  desc.tev.indirectStageCount = 1;
  desc.tev.indirectStages[0].texCoord = 3;
  desc.tev.indirectStages[0].texture = 0;
  desc.tev.stages[0].texCoord = 1;
  desc.tev.stages[0].indirectEnabled = true;
  desc.tev.stages[0].indirectStage = 0;

  EXPECT_EQ(aurora::vita::gfx::pipeline_texcoord_mask(desc),
            static_cast<uint8_t>((1u << 1) | (1u << 3)));
  const auto shader = build_tev_glsl(desc);
  EXPECT_NE(shader.vertex.find("attribute vec3 a_tex1"), std::string::npos);
  EXPECT_NE(shader.vertex.find("attribute vec3 a_tex3"), std::string::npos);
  EXPECT_EQ(shader.vertex.find("attribute vec3 a_tex2"), std::string::npos);
  EXPECT_NE(shader.fragment.find("uniform sampler2D u_tex0"), std::string::npos);
}

TEST(VitaShaderInputs, UnusedTexgenDoesNotConsumeVertexBandwidth) {
  PipelineDesc desc{};
  desc.texgenCount = 4;
  desc.tev.stageCount = 1;
  desc.tev.stages[0].texCoord = 2;
  desc.tev.stages[0].texture = 0;
  desc.tev.stages[0].color.a = aurora::vita::gfx::TevColorArg::TexColor;

  EXPECT_EQ(aurora::vita::gfx::pipeline_texcoord_mask(desc), static_cast<uint8_t>(1u << 2));
  const auto shader = build_tev_glsl(desc);
  EXPECT_EQ(shader.vertex.find("attribute vec3 a_tex0"), std::string::npos);
  EXPECT_EQ(shader.vertex.find("attribute vec3 a_tex1"), std::string::npos);
  EXPECT_NE(shader.vertex.find("attribute vec3 a_tex2"), std::string::npos);
  EXPECT_EQ(shader.vertex.find("attribute vec3 a_tex3"), std::string::npos);
}

TEST(VitaShaderInputs, BumpTexgenKeepsItsCpuDependencyWithoutStreamingIt) {
  PipelineDesc desc{};
  desc.texgenCount = 4;
  desc.texgens[3].type = aurora::vita::gfx::TexGenType::Bump0;
  desc.texgens[3].embossSource = 1;
  desc.tev.stageCount = 1;
  desc.tev.stages[0].texCoord = 3;
  desc.tev.stages[0].texture = 0;
  desc.tev.stages[0].color.a = aurora::vita::gfx::TevColorArg::TexColor;

  EXPECT_EQ(aurora::vita::gfx::pipeline_texcoord_mask(desc), static_cast<uint8_t>(1u << 3));
  EXPECT_EQ(aurora::vita::gfx::pipeline_texgen_compute_mask(desc),
            static_cast<uint8_t>((1u << 1) | (1u << 3)));
}

TEST(VitaShaderInputs, UnusedDirectTextureStateDoesNotFetchOrStreamTexcoord) {
  PipelineDesc desc{};
  desc.texgenCount = 1;
  desc.tev.stageCount = 1;
  desc.tev.stages[0].texCoord = 0;
  desc.tev.stages[0].texture = 0;

  EXPECT_EQ(aurora::vita::gfx::pipeline_texcoord_mask(desc), 0u);
  const auto shader = build_tev_glsl(desc);
  EXPECT_EQ(shader.vertex.find("attribute vec3 a_tex0"), std::string::npos);
  EXPECT_EQ(shader.fragment.find("texture2D(u_tex0,tev_uv)"), std::string::npos);
  EXPECT_EQ(shader.fragment.find("uniform sampler2D u_tex0"), std::string::npos);
  EXPECT_EQ(shader.fragment.find("v_tex0"), std::string::npos);
}

TEST(VitaShaderInputs, UnsampledStageNeverReferencesAnUndeclaredVarying) {
  // The six-stage stadium pipeline leaves TEXCOORD5 in its last TEV stage,
  // although only TEXCOORD0..2 are sampled. GLSL must still compile.
  PipelineDesc desc{};
  desc.texgenCount = 3;
  desc.tev.stageCount = 2;
  desc.tev.stages[0].texCoord = 0;
  desc.tev.stages[0].texture = 0;
  desc.tev.stages[0].color.a = aurora::vita::gfx::TevColorArg::TexColor;
  desc.tev.stages[1].texCoord = 5;
  desc.tev.stages[1].texture = 0xff;
  desc.tev.stages[1].color.d = aurora::vita::gfx::TevColorArg::Prev;
  const auto shader = build_tev_glsl(desc);
  EXPECT_NE(shader.fragment.find("v_tex0"), std::string::npos);
  EXPECT_EQ(shader.fragment.find("v_tex5"), std::string::npos);
  EXPECT_EQ(shader.vertex.find("a_tex5"), std::string::npos);
}

TEST(VitaShaderInputs, MissingTextureDoesNotNeedDirectTexcoord) {
  PipelineDesc desc{};
  desc.texgenCount = 1;
  desc.tev.stageCount = 1;
  desc.tev.stages[0].texCoord = 0;
  desc.tev.stages[0].texture = 0xff;
  desc.tev.stages[0].color.a = aurora::vita::gfx::TevColorArg::TexColor;

  EXPECT_EQ(aurora::vita::gfx::pipeline_texcoord_mask(desc), 0u);
}

TEST(VitaShaderInputs, StreamsOnlyRasterColorActuallyConsumedByTev) {
  PipelineDesc desc{};
  desc.tev.stageCount = 1;
  desc.tev.stages[0].color.a = aurora::vita::gfx::TevColorArg::RasColor;
  desc.tev.stages[0].rasterSource = aurora::vita::gfx::RasterSource::Color1;

  EXPECT_EQ(aurora::vita::gfx::pipeline_raster_color_mask(desc), 0x02u);
  const auto shader = build_tev_glsl(desc);
  EXPECT_EQ(shader.vertex.find("attribute vec4 a_color0"), std::string::npos);
  EXPECT_NE(shader.vertex.find("attribute vec4 a_color1"), std::string::npos);
  EXPECT_EQ(shader.fragment.find("varying vec4 v_color0"), std::string::npos);
  EXPECT_NE(shader.fragment.find("varying vec4 v_color1"), std::string::npos);
}

TEST(VitaShaderInputs, AlphaBumpRasterDoesNotRequireVertexColor) {
  PipelineDesc desc{};
  desc.tev.stageCount = 1;
  desc.tev.stages[0].alpha.a = aurora::vita::gfx::TevAlphaArg::RasAlpha;
  desc.tev.stages[0].rasterSource = aurora::vita::gfx::RasterSource::AlphaBump;

  EXPECT_EQ(aurora::vita::gfx::pipeline_raster_color_mask(desc), 0u);
  const auto shader = build_tev_glsl(desc);
  EXPECT_EQ(shader.vertex.find("attribute vec4 a_color0"), std::string::npos);
  EXPECT_EQ(shader.vertex.find("attribute vec4 a_color1"), std::string::npos);
  EXPECT_NE(shader.fragment.find("vec4 raw_ras=vec4(vec3(ind_alpha),ind_alpha)"), std::string::npos);
}

TEST(VitaShaderAlpha, StaticBooleanCombinationsAvoidUnnecessaryDiscard) {
  PipelineDesc desc{};
  desc.tev.alphaCompare = {Compare::Always, 7, 1, Compare::Less, 128}; // true OR dynamic
  EXPECT_EQ(alpha_compare_static_result(desc.tev.alphaCompare), AlphaTestStaticResult::Pass);
  auto shader = build_tev_glsl(desc);
  EXPECT_EQ(shader.fragment.find("discard"), std::string::npos);

  desc.tev.alphaCompare = {Compare::Never, 7, 0, Compare::Greater, 128}; // false AND dynamic
  EXPECT_EQ(alpha_compare_static_result(desc.tev.alphaCompare), AlphaTestStaticResult::Fail);
  shader = build_tev_glsl(desc);
  EXPECT_NE(shader.fragment.find("discard;"), std::string::npos);

  desc.tev.alphaCompare = {Compare::Less, 7, 0, Compare::Greater, 128};
  EXPECT_EQ(alpha_compare_static_result(desc.tev.alphaCompare), AlphaTestStaticResult::Dynamic);
  shader = build_tev_glsl(desc);
  EXPECT_NE(shader.fragment.find("discard"), std::string::npos);
}

} // namespace
