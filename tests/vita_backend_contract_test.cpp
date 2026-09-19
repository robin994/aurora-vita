#include "gxm/gxm_shader_gen.hpp"
#include "gfx/vita_pipeline_key.hpp"
#include "gfx/vita_texture_decode.hpp"
#include "gfx/vita_vertex_decode.hpp"
#include "gfx/vita_vertex_pipeline.hpp"
#include "gfx/vita_fixed_vertex.hpp"
#include "gfx/vita_efb_copy.hpp"
#include "gfx/vita_sampler_units.hpp"
#include "gfx/vita_draw_batch.hpp"
#include "gfx/vita_hash_map.hpp"
#include "gxm/gxm_texture_layout.hpp"
#include "gxm/gxm_program_cache.hpp"
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
  {
    auto d = basic();
    const auto clippedKey = pipeline_key(d);
    d.fragmentScissor = false;
    REQUIRE(pipeline_key(d) != clippedKey);
    const auto source = gxm::build_tev_cg(d);
    REQUIRE(source.ok());
    REQUIRE(source.fragment.find("WPOS") == std::string::npos);
    REQUIRE(source.fragment.find("window_position.x<u_clip_rect") == std::string::npos);
    REQUIRE(source.fragment.find("discard") == std::string::npos);
  }
  for (unsigned av = 0; av < 2; ++av) for (unsigned bv = 0; bv < 2; ++bv)
    for (unsigned op = 0; op < 4; ++op) {
      auto d = basic();
      d.fragmentScissor = false;
      d.tev.alphaCompare = {av ? Compare::Always : Compare::Never, 13,
                           uint8_t(op), bv ? Compare::Always : Compare::Never, 217};
      const auto shader = gxm::build_tev_cg(d);
      REQUIRE(shader.ok());
      const bool pass[]{bool(av && bv), bool(av || bv), av != bv, av == bv};
      REQUIRE(shader.fragment.find("if(!(false)) discard;") == std::string::npos);
      REQUIRE(shader.discardAll == !pass[op]);
      REQUIRE(shader.fragment.find("discard") == std::string::npos);
    }
  for (unsigned op = 0; op < 4; ++op) {
    auto d = basic();
    d.fragmentScissor = false;
    d.tev.alphaCompare = {Compare::Less, 91, uint8_t(op), Compare::Less, 91};
    const auto shader = gxm::build_tev_cg(d);
    REQUIRE(shader.ok());
    if (op == 3) REQUIRE(shader.fragment.find("discard") == std::string::npos);
    else if (op == 2) {
      REQUIRE(shader.discardAll);
      REQUIRE(shader.fragment.find("discard") == std::string::npos);
    }
    else REQUIRE(shader.fragment.find("if(!((floor(result.a*255.0+0.5)<91.0))) discard;") != std::string::npos);
  }
  {
    auto d = basic();
    d.tev.stages[0].color = {TevColorArg::Reg0, TevColorArg::Zero,
                            TevColorArg::Zero, TevColorArg::Reg0};
    d.tev.stages[0].alpha = {TevAlphaArg::Reg0A, TevAlphaArg::Zero,
                            TevAlphaArg::Zero, TevAlphaArg::Reg0A};
    const auto source = gxm::build_tev_cg(d);
    REQUIRE(source.ok());
    REQUIRE(source.fragment.find("float3 cA=tev_wrap3(reg0.rgb);") != std::string::npos);
    REQUIRE(source.fragment.find("float aA=tev_wrap1(reg0.a);") != std::string::npos);
    REQUIRE(source.fragment.find("float3 cD=reg0.rgb;") != std::string::npos);
    REQUIRE(source.fragment.find("float aD=reg0.a;") != std::string::npos);
  }
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
  auto d = basic(); d.fogMode = FogMode(255); reject(d);
  d = basic(); d.tev.indirectStageCount = 5; reject(d);
  d = basic(); d.polygonOffset = true; reject(d);
  d = basic(); d.blendMode = BlendMode::Logic; d.logicOp=LogicOp::Xor; reject(d);
  d = basic(); d.primitive = Primitive::Lines; reject(d);
  d = basic(); d.tev.stages[0].indirectEnabled = true; reject(d);
  d = basic(); d.tev.stages[0].rasterSource = RasterSource(255); reject(d);
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

  auto fixed = basic();
  fixed.fixedVertexOnGpu = true;
  fixed.layout = fixed_vertex_gpu_layout(fixed);
  source = gxm::build_tev_cg(fixed);
  REQUIRE(source.ok());
  REQUIRE(source.vertex.find("u_gx_position[3]") != std::string::npos);
  REQUIRE(source.vertex.find("u_gx_material[4]") != std::string::npos);
  REQUIRE(source.vertex.find("dot(u_gx_position[0],object_pos)") != std::string::npos);
  fixed.texgenCount=1;
  fixed.texgens[0].type=TexGenType::Matrix2x4;
  fixed.texgens[0].source=TexGenSource::Tex0;
  fixed.texgens[0].matrix=0;
  fixed.tev.stages[0].texture=0;
  fixed.tev.stages[0].texCoord=0;
  fixed.tev.stages[0].color.d=TevColorArg::TexColor;
  fixed.layout=fixed_vertex_gpu_layout(fixed);
  source=gxm::build_tev_cg(fixed);
  REQUIRE(source.ok());
  REQUIRE(source.vertex.find("u_gx_texture0[3]") != std::string::npos);
  REQUIRE(source.vertex.find("a_tex0 : TEXCOORD0") != std::string::npos);

  auto indexedLit=basic();
  indexedLit.fixedVertexOnGpu=true;
  indexedLit.fixedVertexIndexedPn=true;
  indexedLit.colorChannels[0].lightingEnabled=true;
  indexedLit.colorChannels[0].lightMask=0x03;
  indexedLit.colorChannels[0].diffuse=DiffuseFn::Clamp;
  indexedLit.colorChannels[0].attenuation=AttenuationFn::Spot;
  indexedLit.layout=fixed_vertex_gpu_layout(indexedLit);
  source=gxm::build_tev_cg(indexedLit);
  REQUIRE(source.ok());
  REQUIRE(source.vertex.find("a_pn_mtx : TEXCOORD11") != std::string::npos);
  REQUIRE(source.vertex.find("u_gx_position_palette[30]") != std::string::npos);
  REQUIRE(source.vertex.find("u_gx_normal_palette[30]") != std::string::npos);
  REQUIRE(source.vertex.find("u_gx_light[40]") != std::string::npos);
  REQUIRE(source.vertex.find("u_gx_ambient[4]") != std::string::npos);
  REQUIRE(source.vertex.find("u_gx_light[2]") != std::string::npos);
  REQUIRE(source.vertex.find("u_gx_light[7]") != std::string::npos);
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
void native_extended_contract() {
  REQUIRE(native_lod_bias(0.f)==31);
  REQUIRE(native_lod_bias(1.f)==39);
  REQUIRE(native_lod_bias(-1.f)==23);
  REQUIRE(native_lod_bias(.125f)==32);
  REQUIRE(native_lod_bias(20.f)==63);
  REQUIRE(native_lod_bias(-20.f)==0);
  REQUIRE(vitagl_lod_bias(1.f)==8.f);
  for(unsigned fog=0;fog<=unsigned(FogMode::RevExp2);++fog) for(bool range:{false,true}) {
    auto d=basic();d.fogMode=FogMode(fog);d.fogRangeEnabled=range;
    const auto s=gxm::build_tev_cg(d);no_undeclared_inputs(s);
    if(fog)REQUIRE(s.fragment.find("result.rgb=lerp(result.rgb,u_fog_color.rgb")!=std::string::npos);
  }
  for(unsigned matrix=0;matrix<=unsigned(IndirectMatrix::T2);++matrix)
    for(unsigned format=0;format<4;++format) for(unsigned bias=0;bias<8;++bias) {
      auto d=basic();d.tev.indirectStageCount=1;d.tev.indirectStages[0]={3,5,1,2};
      auto& s=d.tev.stages[0];s.indirectEnabled=true;s.indirectMatrix=IndirectMatrix(matrix);
      s.indirectFormat=IndirectFormat(format);s.indirectBias=IndirectBias(bias);
      s.indirectAlpha=IndirectAlphaSel::T;s.rasterSource=RasterSource::AlphaBumpN;
      s.texCoord=2;s.texture=1;s.color.d=TevColorArg::TexColor;s.indirectAddPrev=true;
      const auto result=gxm::build_tev_cg(d);no_undeclared_inputs(result);
      REQUIRE(result.textureMask==((1u<<1)|(1u<<5)));
      REQUIRE(result.fragment.find("prev_ind_uv+=base_texel+ind_off")!=std::string::npos);
    }
  std::array<uint8_t,84> source{};
  // Explicit RGBA8 4x4, 2x2 and 1x1 levels must survive padding unchanged.
  std::fill(source.begin(),source.begin()+64,17);
  std::fill(source.begin()+64,source.begin()+80,93);
  std::fill(source.begin()+80,source.end(),201);
  TextureDesc d{};d.width=d.height=4;d.format=TextureFormat::RGBA8888;
  d.data=source.data();d.dataSize=source.size();d.mipCount=3;
  auto native=gxm::prepare_linear_texture(d);
  REQUIRE(native.ok());REQUIRE(native.mipCount==3);REQUIRE(native.pixels.size()==224);
  REQUIRE(native.pixels[0]==17);REQUIRE(native.pixels[128]==93);REQUIRE(native.pixels[192]==201);
  d.dataSize=83;REQUIRE(!gxm::prepare_linear_texture(d).ok());
  d.mipCount=1;d.dataSize=64;d.generateMipmaps=true;
  native=gxm::prepare_linear_texture(d);REQUIRE(native.ok());REQUIRE(native.pixels[192]==17);
  d.width=3;REQUIRE(!gxm::prepare_linear_texture(d).ok());
  std::array<uint8_t,92> rectangular{};
  std::fill(rectangular.begin(),rectangular.begin()+64,11);
  std::fill(rectangular.begin()+64,rectangular.begin()+80,33);
  std::fill(rectangular.begin()+80,rectangular.begin()+88,77);
  std::fill(rectangular.begin()+88,rectangular.end(),155);
  d.width=8;d.height=2;d.mipCount=4;d.generateMipmaps=false;
  d.data=rectangular.data();d.dataSize=rectangular.size();
  native=gxm::prepare_linear_texture(d);
  REQUIRE(native.ok());REQUIRE(native.pixels.size()==160);
  REQUIRE(native.pixels[64]==33);REQUIRE(native.pixels[96]==77);REQUIRE(native.pixels[128]==155);

  const uint8_t rgba[]{255,0,0,255, 0,255,0,255, 0,0,255,128, 255,255,255,64};
  std::vector<uint8_t> copy;
  const Scissor all{0,0,2,2};
  REQUIRE(copy_efb_rgba8(rgba,2,2,all,2,2,EfbCopyFormat::Passthrough,false,false,copy));
  REQUIRE(copy==std::vector<uint8_t>(rgba,rgba+16));
  REQUIRE(copy_efb_rgba8(rgba,2,2,all,2,2,EfbCopyFormat::Passthrough,true,true,copy));
  REQUIRE(std::memcmp(copy.data(),rgba+12,4)==0);REQUIRE(std::memcmp(copy.data()+12,rgba,4)==0);
  REQUIRE(!copy_efb_rgba8(rgba,2,2,{-1,0,2,2},2,2,EfbCopyFormat::Passthrough,false,false,copy));
  REQUIRE(!copy_efb_rgba8(rgba,2,2,all,2,2,EfbCopyFormat::DepthZ16,false,false,copy));
  for(unsigned fmt=0;fmt<=unsigned(EfbCopyFormat::GB8);++fmt)
    REQUIRE(copy_efb_rgba8(rgba,2,2,all,3,5,EfbCopyFormat(fmt),false,false,copy));
  REQUIRE(copy_efb_rgba8(rgba,2,2,all,2,2,EfbCopyFormat::RG8,false,false,copy));
  REQUIRE(copy[0]==255 && copy[1]==255 && copy[2]==255 && copy[3]==0);
  REQUIRE(efb_copy_sample_mode(EfbCopyFormat::Passthrough)==0);
  REQUIRE(efb_copy_sample_mode(EfbCopyFormat::R4)==1);
  REQUIRE(efb_copy_sample_mode(EfbCopyFormat::A8)==2);

  const char* cg="float4 main(float4 p:POSITION):POSITION{return p;}";
  const auto vh=gxm::gxm_program_source_hash(cg,gxm::ProgramStage::Vertex);
  const auto fh=gxm::gxm_program_source_hash(cg,gxm::ProgramStage::Fragment);
  REQUIRE(vh!=fh);
  std::array<uint8_t,32> gxp{};
  for(unsigned i=0;i<gxp.size();++i)gxp[i]=uint8_t(i*7u+3u);
  gxm::GxmProgramCacheHeader cache{};
  cache.sourceHash=vh;cache.stage=uint32_t(gxm::ProgramStage::Vertex);cache.length=gxp.size();
  cache.binaryHash=program_cache_hash(gxp.data(),gxp.size());
  REQUIRE(gxm::valid_gxm_program_cache(cache,gxp.data(),gxp.size(),vh,gxm::ProgramStage::Vertex));
  REQUIRE(!gxm::valid_gxm_program_cache(cache,gxp.data(),gxp.size(),fh,gxm::ProgramStage::Vertex));
  REQUIRE(!gxm::valid_gxm_program_cache(cache,gxp.data(),gxp.size(),vh,gxm::ProgramStage::Fragment));
  gxp[5]^=1;
  REQUIRE(!gxm::valid_gxm_program_cache(cache,gxp.data(),gxp.size(),vh,gxm::ProgramStage::Vertex));
}
} // namespace
int main() {
  {
    TextureDesc d{}; d.format=TextureFormat::RGBA8888; d.mipCount=1;
    d.width=d.height=4;
    std::array<uint8_t,64> pixels{};
    for(unsigned i=0;i<16;++i) for(unsigned c=0;c<4;++c) pixels[4*i+c]=uint8_t(i);
    d.data=pixels.data();d.dataSize=pixels.size();
    const auto sw=gxm::prepare_swizzled_texture(d);
    const uint8_t expected[]{0,4,1,5,8,12,9,13,2,6,3,7,10,14,11,15};
    REQUIRE(sw.ok()); REQUIRE(sw.pixels.size()==64);
    for(unsigned i=0;i<16;++i) REQUIRE(sw.pixels[i*4]==expected[i]);
    for(unsigned w:{1u,2u,4u,8u,16u}) for(unsigned h:{1u,2u,4u,8u,16u}) {
      std::vector<uint8_t> input(size_t(w)*h*4);
      for(unsigned i=0;i<w*h;++i) std::memcpy(input.data()+i*4,&i,4);
      d.width=w;d.height=h;d.data=input.data();d.dataSize=input.size();
      const auto result=gxm::prepare_swizzled_texture(d); REQUIRE(result.ok());
      std::set<unsigned> seen;
      for(unsigned i=0;i<w*h;++i) {unsigned v;std::memcpy(&v,result.pixels.data()+i*4,4);REQUIRE(v<w*h);seen.insert(v);}
      REQUIRE(seen.size()==w*h);
    }
    d.width=3; REQUIRE(!gxm::prepare_swizzled_texture(d).ok());
    std::array<uint8_t,80> mipPixels{};
    for(unsigned i=0;i<16;++i)for(unsigned c=0;c<4;++c)mipPixels[i*4+c]=uint8_t(i);
    for(unsigned i=0;i<4;++i)for(unsigned c=0;c<4;++c)mipPixels[64+i*4+c]=uint8_t(100+i);
    d.width=d.height=4;d.mipCount=2;d.data=mipPixels.data();d.dataSize=mipPixels.size();
    const auto mips=gxm::prepare_swizzled_texture(d);
    REQUIRE(mips.ok());REQUIRE(mips.mipCount==2);REQUIRE(mips.pixels.size()==80);
    const uint8_t expectedMip1[]{100,102,101,103};
    for(unsigned i=0;i<4;++i)REQUIRE(mips.pixels[64+i*4]==expectedMip1[i]);
    auto p=basic(); p.tev.stages[0].texture=0;p.tev.stages[0].texCoord=0;
    p.tev.stages[0].color.d=TevColorArg::TexColor;
    const auto normalKey=pipeline_key(p);p.nativeTextureWrapMask=1;
    REQUIRE(pipeline_key(p)!=normalKey);
    const auto shader=gxm::build_tev_cg(p); REQUIRE(shader.ok());
    REQUIRE(shader.fragment.find("tex2D(u_tex0,gx_sample_uv(")!=std::string::npos);
  }
  {
    DrawPacket a{}, b{};
    a.pipelineKey = b.pipelineKey = 1;
    a.vertices = {1, 0, 112}; b.vertices = {1, 112, 112};
    a.indices = {2, 0, 12}; b.indices = {2, 12, 12};
    a.vertexCount = b.vertexCount = 4; a.indexCount = b.indexCount = 6;
    REQUIRE(local_draws_mergeable(a, b));
    auto changed = b; changed.textures[0].sampler.maxLod = 2;
    REQUIRE(!local_draws_mergeable(a, changed));
    changed = b; changed.uniforms.mvp[12] = 1;
    REQUIRE(!local_draws_mergeable(a, changed));
    changed = b; changed.vertices.offset += 4;
    REQUIRE(!local_draws_mergeable(a, changed));
    changed = b; changed.absoluteVertexIndices = true;
    REQUIRE(!local_draws_mergeable(a, changed));
    changed = b; changed.vertexCount = 63996;
    REQUIRE(!local_draws_mergeable(a, changed));
    changed = b; changed.textures[0].flipY = true;
    REQUIRE(!local_draws_mergeable(a, changed));
    changed = b; changed.pipelineKey = 2;
    REQUIRE(!local_draws_mergeable(a, changed));
  }
  {
    FlatHashMap<uint64_t,uint64_t> flat;
    for(uint64_t i=0;i<4096;++i)flat.emplace(i*0x9e3779b97f4a7c15ull,i);
    for(uint64_t i=0;i<4096;++i) {
      const auto it=flat.find(i*0x9e3779b97f4a7c15ull);
      REQUIRE(it!=flat.end());REQUIRE(it->second==i);
    }
    NodeHashMap<uint64_t,uint64_t> nodes;
    nodes.emplace(7,0x12345678u);
    auto* stable=&nodes.find(7)->second;
    for(uint64_t i=8;i<8192;++i)nodes.emplace(i,i);
    REQUIRE(nodes.find(7)!=nodes.end());REQUIRE(&nodes.find(7)->second==stable);
    REQUIRE(*stable==0x12345678u);
  }
  shader_masks(); shader_operations(); rejection_tests(); projection_contract(); common_decode_contract(); keys_and_defaults();
  native_extended_contract();
  std::printf("PASS: %u checks, 1024 input-mask combinations; native Cg generation and shared CPU contracts (not GPU execution).\n", checks);
  return 0;
}
