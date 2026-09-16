#include "gxm/gxm_shader_gen.hpp"
#include "gfx/vita_pipeline_key.hpp"
#include "gfx/vita_texture_decode.hpp"
#include "gfx/vita_vertex_decode.hpp"
#include "gfx/vita_vertex_pipeline.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace aurora::vita;
using namespace gfx;
unsigned checks = 0;
void require(bool success, const char* expression, int line) {
  ++checks;
  if (!success) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    std::exit(1);
  }
}
#define REQUIRE(expression) require(bool(expression), #expression, __LINE__)
PipelineDesc basic() {
  PipelineDesc d;
  d.layout = gpu_vertex_layout();
  d.tev.stages[0].color.d = TevColorArg::RasColor;
  d.tev.stages[0].alpha.d = TevAlphaArg::RasAlpha;
  return d;
}
void no_undeclared_inputs(const gxm::ShaderSources& s) {
  REQUIRE(s.ok());
  const std::regex names("(v_tex[0-7]|v_color[01]|u_tex[0-7])");
  for (auto it = std::sregex_iterator(s.fragment.begin(), s.fragment.end(), names);
       it != std::sregex_iterator(); ++it) {
    const std::string name = it->str();
    if (name.compare(0, 5, "u_tex") == 0)
      REQUIRE(s.fragment.find("uniform sampler2D " + name + " : TEXUNIT") != std::string::npos);
    else {
      const std::string type = name.compare(0, 5, "v_tex") == 0 ? "float3 " : "float4 ";
      REQUIRE(s.fragment.find(type + name + " : ") != std::string::npos);
      REQUIRE(s.vertex.find("out " + type + name + " : ") != std::string::npos);
    }
  }
  REQUIRE(s.vertex.find("gl_") == std::string::npos);
  REQUIRE(s.fragment.find("gl_") == std::string::npos);
  REQUIRE(s.vertex.find("attribute ") == std::string::npos);
  REQUIRE(s.fragment.find("varying ") == std::string::npos);
  REQUIRE(s.fragment.find("texture2D(") == std::string::npos);
}
void shader_masks() {
  for (unsigned textureMask = 0; textureMask < 256; ++textureMask) {
    for (unsigned colorMask = 0; colorMask < 4; ++colorMask) {
      auto d = basic();
      d.texgenCount = 8;
      d.layout = gpu_vertex_layout(uint8_t(textureMask), uint8_t(colorMask));
      d.tev.stageCount = 0;
      for (unsigned i = 0; i < 8; ++i) if (textureMask & (1u << i)) {
        auto& s = d.tev.stages[d.tev.stageCount++];
        s = TevStage{}; s.texture = i; s.texCoord = i;
        s.color.d = TevColorArg::TexColor;
      }
      for (unsigned i = 0; i < 2; ++i) if (colorMask & (1u << i)) {
        auto& s = d.tev.stages[d.tev.stageCount++];
        s = TevStage{}; s.rasterSource = i ? RasterSource::Color1 : RasterSource::Color0;
        s.color.d = TevColorArg::RasColor;
      }
      if (!d.tev.stageCount) { d.tev.stageCount = 1; d.tev.stages[0] = TevStage{}; }
      const auto sources = gxm::build_tev_cg(d);
      no_undeclared_inputs(sources);
      REQUIRE(sources.textureMask == textureMask);
      REQUIRE(sources.texcoordMask == textureMask);
      REQUIRE(sources.colorMask == colorMask);
    }
  }
  // Regression case: the stage names a coordinate but does not sample a texture.
  auto d = basic();
  d.tev.stages[0].texCoord = 7;
  d.tev.stages[0].texture = 2;
  auto source = gxm::build_tev_cg(d);
  REQUIRE(source.ok());
  REQUIRE(source.fragment.find("v_tex7") == std::string::npos);
  REQUIRE(source.fragment.find("u_tex2") == std::string::npos);
  d.tev.stages[0].texture = 0xff;
  d.tev.stages[0].color.d = TevColorArg::TexColor;
  source = gxm::build_tev_cg(d);
  no_undeclared_inputs(source);
  REQUIRE(source.textureMask == 0);
  REQUIRE(source.fragment.find("raw_tex=float4(1.0)") != std::string::npos);
}
void shader_operations() {
  for (unsigned op = 0; op <= unsigned(TevOp::CompRGB8Equal); ++op) {
    auto d = basic();
    d.tev.stageCount = 16;
    for (unsigned i = 0; i < 16; ++i) {
      auto& s = d.tev.stages[i];
      s.colorOp = s.alphaOp = static_cast<TevOp>(op);
      s.color = {TevColorArg::Prev, TevColorArg::RasColor, TevColorArg::Konst, TevColorArg::Reg2};
      s.alpha = {TevAlphaArg::PrevA, TevAlphaArg::RasAlpha, TevAlphaArg::Konst, TevAlphaArg::Reg2A};
      s.colorOut = TevReg(i % 4); s.alphaOut = TevReg((i + 1) % 4);
      s.colorClamp = i % 2; s.alphaClamp = i % 3;
      s.colorBias = TevBias(i % 3); s.alphaBias = TevBias((i + 1) % 3);
      s.colorScale = s.alphaScale = TevScale(i % 4);
    }
    const auto source = gxm::build_tev_cg(d);
    no_undeclared_inputs(source);
    REQUIRE(source.fragment.find("float result_a=") < source.fragment.find(".rgb=result_c;"));
  }
  for (unsigned k = 0; k <= unsigned(KonstColorSel::K3A); ++k) {
    auto d = basic();
    d.tev.stages[0].color.d = TevColorArg::Konst;
    d.tev.stages[0].konstColor = KonstColorSel(k);
    REQUIRE(gxm::build_tev_cg(d).ok());
  }
  for (unsigned k = 0; k <= unsigned(KonstAlphaSel::K3A); ++k) {
    auto d = basic();
    d.tev.stages[0].alpha.d = TevAlphaArg::Konst;
    d.tev.stages[0].konstAlpha = KonstAlphaSel(k);
    REQUIRE(gxm::build_tev_cg(d).ok());
  }
  for (unsigned c0 = 0; c0 < 8; ++c0) for (unsigned c1 = 0; c1 < 8; ++c1) for (unsigned op = 0; op < 4; ++op) {
    auto d = basic();
    d.tev.alphaCompare = {Compare(c0), 37, uint8_t(op), Compare(c1), 201};
    REQUIRE(gxm::build_tev_cg(d).ok());
  }
  auto d = basic();
  d.tev.stages[0].texture = 0;
  d.tev.stages[0].texCoord = 3;
  d.tev.stages[0].color.d = TevColorArg::TexColor;
  d.texgenCount = 4;
  d.texgens[3].type = TexGenType::Matrix3x4;
  const auto source = gxm::build_tev_cg(d);
  REQUIRE(source.fragment.find("v_tex3.xy/v_tex3.z") != std::string::npos);
}
void rejection_tests() {
  const auto reject = [](const PipelineDesc& d) {
    const auto source = gxm::build_tev_cg(d);
    REQUIRE(!source.ok()); REQUIRE(!source.error.empty());
    REQUIRE(source.vertex.empty() && source.fragment.empty());
  };
  auto d = basic(); d.fogMode = FogMode::Linear; reject(d);
  d = basic(); d.fogRangeEnabled = true; reject(d);
  d = basic(); d.fixedVertexOnGpu = true; reject(d);
  d = basic(); d.polygonOffset = true; reject(d);
  d = basic(); d.blendMode = BlendMode::Logic; reject(d);
  d = basic(); d.primitive = Primitive::Lines; reject(d);
  d = basic(); d.tev.stages[0].indirectEnabled = true; reject(d);
  d = basic(); d.tev.stages[0].rasterSource = RasterSource::AlphaBump; reject(d);
  d = basic(); d.tev.stageCount = 0; reject(d);
  d = basic(); d.tev.stageCount = 17; reject(d);
  d = basic(); d.tev.stages[0].texture = 8; reject(d);
  d = basic(); d.tev.stages[0].colorOp = TevOp(255); reject(d);
  d = basic(); d.tev.stages[0].colorOut = TevReg(255); reject(d);
  d = basic(); d.tev.swapTable[0].r = TevChannel(255); reject(d);
  d = basic(); d.tev.alphaCompare.op = 4; reject(d);
  d = basic(); d.layout.count = 0; reject(d);
  d = basic(); d.layout.count = 17; reject(d);
  d = basic(); d.layout.attributes[0].components = 3; reject(d);
  d = basic(); d.layout.attributes[1].location = 0; reject(d);
  d = basic(); d.layout.attributes[1].offset = 65535; reject(d);
  d = basic(); d.layout.attributes[1].stride = 8; reject(d);
  d = basic(); d.layout.attributes[3].offset = 25; reject(d);
  d = basic(); d.layout = gpu_vertex_layout(0, 0); reject(d);
  d = basic(); d.layout.attributes[0].scalar = VertexScalar(255); reject(d);
  d = basic(); d.dstAlpha = 256; reject(d);
}
void projection_contract() {
  auto d = basic();
  auto source = gxm::build_tev_cg(d);
  REQUIRE(source.vertex.find("u_mvp[0]*a_position.x+u_mvp[1]*a_position.y") != std::string::npos);
  REQUIRE(source.vertex.find("p.z=-2.0*p.z-p.w") != std::string::npos);
  d.reversedZ = false;
  source = gxm::build_tev_cg(d);
  REQUIRE(source.vertex.find("p.z=2.0*p.z+p.w") != std::string::npos);
  d.positionIsClipSpace = true;
  source = gxm::build_tev_cg(d);
  REQUIRE(source.vertex.find("u_mvp") == std::string::npos);
  // Check the shared projection's depth contract independently of GPU execution.
  for (const bool reversed : {false, true}) {
    for (const float fraction : {0.f, .25f, .5f, 1.f}) {
      const float w = 4.f;
      const float gxZ = reversed ? -fraction * w : (fraction-1.f) * w;
      const float clipZ = reversed ? -2.f*gxZ-w : 2.f*gxZ+w;
      const float depth = .5f + .5f*clipZ/w;
      REQUIRE(std::abs(depth - fraction) < 1e-6f);
    }
  }
  constexpr float n = .1f, f = 100.f;
  for (const float distance : {n, f}) {
    const float z = n/(n-f)*(-distance) + f*n/(n-f);
    const float depth = 1.f + z/distance;
    REQUIRE(std::abs(depth - (distance == n ? 0.f : 1.f)) < 1e-5f);
  }
}
void common_decode_contract() {
  // Big-endian signed fixed-point source, not native host floats.
  const uint8_t raw[]{0x00,0x02,0xff,0xfc,0x00,0x06};
  VertexDecodeLayout layout;
  layout.streamStride = sizeof(raw); layout.count = 1;
  layout.attributes[0] = {VertexSemantic::Position, VertexSource::Direct, VertexComponent::S16, 3, 1, 0, 0, {}};
  auto decoded = decode_vertices(raw, sizeof(raw), 1, layout);
  REQUIRE(decoded.ok && decoded.vertices.size() == 1);
  REQUIRE(decoded.vertices[0].position[0] == 1.f);
  REQUIRE(decoded.vertices[0].position[1] == -2.f);
  REQUIRE(decoded.vertices[0].position[2] == 3.f);
  REQUIRE(!decode_vertices(raw, sizeof(raw)-1, 1, layout).ok);
  auto d = basic();
  d.colorChannels[0].materialSource = ColorSource::Vertex;
  d.colorChannels[2].materialSource = ColorSource::Vertex;
  VertexTransformState state;
  state.postexMatrices[0].v[3] = 4.f;
  state.postexMatrices[0].v[7] = 5.f;
  state.postexMatrices[0].v[11] = 6.f;
  REQUIRE(run_vertex_pipeline(decoded.vertices, d, state));
  REQUIRE(decoded.vertices[0].position[0] == 5.f);
  REQUIRE(decoded.vertices[0].position[1] == 3.f);
  REQUIRE(decoded.vertices[0].position[2] == 9.f);
  std::array<uint8_t,32> tiled{};
  for (unsigned i = 0; i < 16; ++i) { tiled[i*2] = 0xf8; tiled[i*2+1] = 0; }
  TextureDesc texture;
  texture.width = texture.height = 4;
  texture.format = TextureFormat::RGB565;
  texture.data = tiled.data(); texture.dataSize = tiled.size();
  const auto rgba = decode_texture_rgba8(texture);
  REQUIRE(rgba.ok && rgba.rgba.size() == 64);
  for (unsigned i = 0; i < 16; ++i) {
    REQUIRE(rgba.rgba[i*4] == 255 && rgba.rgba[i*4+1] == 0 && rgba.rgba[i*4+2] == 0 && rgba.rgba[i*4+3] == 255);
  }
  texture.dataSize = 31;
  REQUIRE(!decode_texture_rgba8(texture).ok);
}
void keys_and_defaults() {
  auto d = basic();
  const auto key = pipeline_key(d);
  auto changed = d; changed.reversedZ = !d.reversedZ;
  REQUIRE(pipeline_key(changed) != key);
  changed = d; changed.blendMode = BlendMode::Blend;
  REQUIRE(pipeline_key(changed) != key);
  changed = d; changed.layout = gpu_vertex_layout(0, 1);
  REQUIRE(pipeline_key(changed) != key);
  REQUIRE(sizeof(GpuVertex) == 120);
  REQUIRE(d.layout.attributes[0].stride == sizeof(GpuVertex));
  REQUIRE(d.layout.attributes[1].offset == offsetof(GpuVertex, color0));
}
} // namespace
int main() {
  shader_masks(); shader_operations(); rejection_tests(); projection_contract(); common_decode_contract(); keys_and_defaults();
  std::printf("PASS: %u checks, 1024 input-mask combinations; native Cg generation and shared CPU contracts (not GPU execution).\n", checks);
  return 0;
}
