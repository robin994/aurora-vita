#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace aurora::vita::gfx {

using Handle = uint32_t;
constexpr Handle InvalidHandle = 0;
constexpr uint32_t MaxTextures = 8;
constexpr uint32_t MaxTevStages = 16;
constexpr uint32_t MaxVertexAttributes = 16;
constexpr uint32_t MaxIndStages = 4;
constexpr uint32_t MaxIndMatrices = 3;
constexpr uint32_t MaxLights = 8;

enum class Primitive : uint8_t { Triangles, TriangleStrip, TriangleFan, Lines, LineStrip, Points };
enum class Compare : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class CullMode : uint8_t { None, Front, Back, All };
enum class BlendMode : uint8_t { None, Blend, Subtract, Logic };
enum class BlendFactor : uint8_t {
  Zero, One, SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor,
  SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha,
};
enum class LogicOp : uint8_t {
  Clear, And, ReverseAnd, Copy, InvertedAnd, Noop, Xor, Or, Nor,
  Equiv, Invert, ReverseOr, InvertedCopy, InvertedOr, Nand, Set,
};
enum class WrapMode : uint8_t { Clamp, Repeat, Mirror };
enum class Filter : uint8_t { Nearest, Linear, NearestMipmapNearest, LinearMipmapNearest, NearestMipmapLinear, LinearMipmapLinear };
enum class FogMode : uint8_t { None, Linear, Exp, Exp2, RevExp, RevExp2 };
enum class RasterSource : uint8_t { Color0, Color1, AlphaBump, AlphaBumpN, Zero };
enum class TevChannel : uint8_t { Red, Green, Blue, Alpha };
enum class IndirectFormat : uint8_t { Bits8, Bits5, Bits4, Bits3 };
enum class IndirectBias : uint8_t { None, S, T, ST, U, SU, TU, STU };
enum class IndirectAlphaSel : uint8_t { Off, S, T, U };
enum class IndirectMatrix : uint8_t { Off, Mtx0, Mtx1, Mtx2, S0, S1, S2, T0, T1, T2 };
enum class IndirectWrap : uint8_t { Off, W256, W128, W64, W32, W16, W0 };
enum class TexGenType : uint8_t { Matrix3x4, Matrix2x4, Bump0, Bump1, Bump2, Bump3, Bump4, Bump5, Bump6, Bump7, SRTG };
enum class TexGenSource : uint8_t { Position, Normal, Binormal, Tangent, Tex0, Tex1, Tex2, Tex3, Tex4, Tex5, Tex6, Tex7, Color0, Color1 };
enum class ColorSource : uint8_t { Register, Vertex };
enum class DiffuseFn : uint8_t { None, Signed, Clamp };
enum class AttenuationFn : uint8_t { None, Specular, Spot };

enum class TextureFormat : uint8_t {
  I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, C4, C8, C14X2, CMPR, RGBA8888,
};
enum class PaletteFormat : uint8_t { None, IA8, RGB565, RGB5A3 };

enum class VertexScalar : uint8_t { F32, S8, U8, S16, U16 };
struct VertexAttribute {
  uint8_t location = 0;
  uint8_t components = 0;
  VertexScalar scalar = VertexScalar::F32;
  bool normalized = false;
  uint16_t stride = 0;
  uint16_t offset = 0;
};
struct VertexLayout {
  std::array<VertexAttribute, MaxVertexAttributes> attributes{};
  uint8_t count = 0;
  uint8_t _pad[3]{};
};

struct Viewport {
  float x = 0.f;
  float y = 0.f;
  float width = 960.f;
  float height = 544.f;
  float znear = 0.f;
  float zfar = 1.f;
};
struct Scissor {
  int32_t x = 0;
  int32_t y = 0;
  int32_t width = 960;
  int32_t height = 544;
};
struct Color {
  float r = 0.f, g = 0.f, b = 0.f, a = 1.f;
};

// TEV backend description. The Aurora bridge translates native GX enums to these.
enum class TevColorArg : uint8_t {
  Prev, PrevA, Reg0, Reg0A, Reg1, Reg1A, Reg2, Reg2A,
  TexColor, TexAlpha, RasColor, RasAlpha, One, Half, Konst, Zero,
};
enum class TevAlphaArg : uint8_t { PrevA, Reg0A, Reg1A, Reg2A, TexAlpha, RasAlpha, Konst, Zero };
enum class TevOp : uint8_t {
  Add, Sub,
  CompR8Greater, CompR8Equal, CompGR16Greater, CompGR16Equal,
  CompBGR24Greater, CompBGR24Equal, CompRGB8Greater, CompRGB8Equal,
};
enum class TevBias : uint8_t { Zero, AddHalf, SubHalf };
enum class TevScale : uint8_t { Scale1, Scale2, Scale4, Divide2 };
enum class TevReg : uint8_t { Prev, Reg0, Reg1, Reg2 };
enum class KonstColorSel : uint8_t {
  One, SevenEighths, SixEighths, FiveEighths, FourEighths, ThreeEighths, TwoEighths, OneEighth,
  K0, K1, K2, K3, K0R, K1R, K2R, K3R, K0G, K1G, K2G, K3G,
  K0B, K1B, K2B, K3B, K0A, K1A, K2A, K3A,
};
enum class KonstAlphaSel : uint8_t {
  One, SevenEighths, SixEighths, FiveEighths, FourEighths, ThreeEighths, TwoEighths, OneEighth,
  K0R, K1R, K2R, K3R, K0G, K1G, K2G, K3G, K0B, K1B, K2B, K3B, K0A, K1A, K2A, K3A,
};
struct TevColorPass { TevColorArg a=TevColorArg::Zero,b=TevColorArg::Zero,c=TevColorArg::Zero,d=TevColorArg::Zero; };
struct TevAlphaPass { TevAlphaArg a=TevAlphaArg::Zero,b=TevAlphaArg::Zero,c=TevAlphaArg::Zero,d=TevAlphaArg::Zero; };
struct TevSwapDesc { TevChannel r=TevChannel::Red,g=TevChannel::Green,b=TevChannel::Blue,a=TevChannel::Alpha; };
struct IndirectStageDesc {
  uint8_t texCoord = 0xff;
  uint8_t texture = 0xff;
  uint8_t scaleSShift = 0;
  uint8_t scaleTShift = 0;
};
struct TexGenDesc {
  TexGenType type = TexGenType::Matrix2x4;
  TexGenSource source = TexGenSource::Tex0;
  int8_t matrix = -1;
  int8_t postMatrix = -1;
  uint8_t embossSource = 0;
  bool normalize = false;
  bool matrixFromVertex = false;
};
struct ColorChannelDesc {
  ColorSource materialSource = ColorSource::Register;
  ColorSource ambientSource = ColorSource::Register;
  DiffuseFn diffuse = DiffuseFn::None;
  AttenuationFn attenuation = AttenuationFn::None;
  bool lightingEnabled = false;
  uint8_t lightMask = 0;
};
struct LightUniform {
  std::array<float,4> position{{0,0,0,1}};
  std::array<float,4> direction{{0,0,-1,0}};
  std::array<float,4> color{{0,0,0,0}};
  std::array<float,4> cosAtt{{1,0,0,0}};
  std::array<float,4> distAtt{{1,0,0,0}};
};
struct TevStage {
  TevColorPass color{};
  TevAlphaPass alpha{};
  TevOp colorOp = TevOp::Add;
  TevOp alphaOp = TevOp::Add;
  TevBias colorBias = TevBias::Zero;
  TevBias alphaBias = TevBias::Zero;
  TevScale colorScale = TevScale::Scale1;
  TevScale alphaScale = TevScale::Scale1;
  TevReg colorOut = TevReg::Prev;
  TevReg alphaOut = TevReg::Prev;
  KonstColorSel konstColor = KonstColorSel::One;
  KonstAlphaSel konstAlpha = KonstAlphaSel::One;
  uint8_t texture = 0xff;
  uint8_t texCoord = 0xff;
  RasterSource rasterSource = RasterSource::Color0;
  uint8_t rasSwap = 0;
  uint8_t texSwap = 0;
  bool colorClamp = true;
  bool alphaClamp = true;
  bool indirectEnabled = false;
  uint8_t indirectStage = 0;
  IndirectFormat indirectFormat = IndirectFormat::Bits8;
  IndirectBias indirectBias = IndirectBias::None;
  IndirectAlphaSel indirectAlpha = IndirectAlphaSel::Off;
  IndirectMatrix indirectMatrix = IndirectMatrix::Off;
  IndirectWrap indirectWrapS = IndirectWrap::Off;
  IndirectWrap indirectWrapT = IndirectWrap::Off;
  bool indirectUseOrigLod = false;
  bool indirectAddPrev = false;
};
struct AlphaCompareDesc {
  Compare comp0 = Compare::Always;
  uint8_t ref0 = 0;
  uint8_t op = 0; // 0 AND, 1 OR, 2 XOR, 3 XNOR
  Compare comp1 = Compare::Always;
  uint8_t ref1 = 0;
};
enum class AlphaTestStaticResult : uint8_t { Dynamic, Pass, Fail };
inline constexpr AlphaTestStaticResult alpha_compare_static_result(const AlphaCompareDesc& a) noexcept {
  const auto known=[](Compare c) constexpr -> int {
    return c==Compare::Always?1:c==Compare::Never?0:-1;
  };
  const int lhs=known(a.comp0),rhs=known(a.comp1);
  switch(a.op&3u) {
  case 0: // AND
    if(lhs==0||rhs==0)return AlphaTestStaticResult::Fail;
    if(lhs==1&&rhs==1)return AlphaTestStaticResult::Pass;
    break;
  case 1: // OR
    if(lhs==1||rhs==1)return AlphaTestStaticResult::Pass;
    if(lhs==0&&rhs==0)return AlphaTestStaticResult::Fail;
    break;
  case 2: // XOR
    if(lhs>=0&&rhs>=0)return lhs!=rhs?AlphaTestStaticResult::Pass:AlphaTestStaticResult::Fail;
    break;
  case 3: // XNOR
    if(lhs>=0&&rhs>=0)return lhs==rhs?AlphaTestStaticResult::Pass:AlphaTestStaticResult::Fail;
    break;
  }
  return AlphaTestStaticResult::Dynamic;
}
struct TevProgramDesc {
  std::array<TevStage, MaxTevStages> stages{};
  std::array<TevSwapDesc, 4> swapTable{};
  std::array<IndirectStageDesc, MaxIndStages> indirectStages{};
  uint8_t stageCount = 1;
  uint8_t texCoordCount = 1;
  uint8_t rasterColorCount = 1;
  uint8_t indirectStageCount = 0;
  AlphaCompareDesc alphaCompare{};
};

struct PipelineDesc {
  Primitive primitive = Primitive::Triangles;
  Compare depthFunc = Compare::LessEqual;
  CullMode cull = CullMode::Back;
  BlendMode blendMode = BlendMode::None;
  BlendFactor srcFactor = BlendFactor::SrcAlpha;
  BlendFactor dstFactor = BlendFactor::OneMinusSrcAlpha;
  LogicOp logicOp = LogicOp::Copy;
  bool depthTest = true;
  bool depthWrite = true;
  bool colorWrite = true;
  bool alphaWrite = true;
  bool reversedZ = true;
  bool polygonOffset = false;
  float polygonOffsetFactor = 0.f;
  float polygonOffsetUnits = 0.f;
  int16_t dstAlpha = -1; // -1: preserve TEV alpha, 0..255: GX destination alpha override
  FogMode fogMode = FogMode::None;
  bool fogOrthographic = false;
  bool fogRangeEnabled = false;
  bool positionIsClipSpace = false; // CPU-expanded GX lines/points already contain clip-space xyzw.
  bool fragmentScissor = true; // Native GXM may omit clipping only for the complete target.
  // GXM-only specialization: these sampled slots can use the hardware sampler
  // directly with an unmodified interpolated .xy coordinate. This both avoids
  // shader-side wrap/transform arithmetic and lets vitaShaRK classify eligible
  // tex2D calls as non-dependent texture reads.
  uint8_t nativeTextureWrapMask = 0;
  bool fixedVertexOnGpu = false; // Raw object-space inputs; GX vertex processing runs in the shader.
  bool fixedVertexIndexedPn = false; // Per-vertex GX PNMTXIDX selects the 10-entry position/normal palette.
  VertexLayout layout{};
  std::array<TexGenDesc, MaxTextures> texgens{};
  uint8_t texgenCount = 0;
  std::array<ColorChannelDesc, 4> colorChannels{};
  TevProgramDesc tev{};
};

inline constexpr bool tev_color_arg_uses_texture(TevColorArg a) noexcept {
  return a==TevColorArg::TexColor||a==TevColorArg::TexAlpha;
}

inline constexpr bool tev_alpha_arg_uses_texture(TevAlphaArg a) noexcept {
  return a==TevAlphaArg::TexAlpha;
}

inline constexpr bool tev_stage_uses_texture(const TevStage& s) noexcept {
  return tev_color_arg_uses_texture(s.color.a)||tev_color_arg_uses_texture(s.color.b)||
         tev_color_arg_uses_texture(s.color.c)||tev_color_arg_uses_texture(s.color.d)||
         tev_alpha_arg_uses_texture(s.alpha.a)||tev_alpha_arg_uses_texture(s.alpha.b)||
         tev_alpha_arg_uses_texture(s.alpha.c)||tev_alpha_arg_uses_texture(s.alpha.d);
}

inline uint8_t pipeline_sampled_texture_mask(const PipelineDesc& d) noexcept {
  uint8_t mask=0;
  const unsigned stages=std::min<unsigned>(d.tev.stageCount,MaxTevStages);
  for(unsigned i=0;i<stages;i++){
    const auto&s=d.tev.stages[i];
    if(tev_stage_uses_texture(s)&&s.texture<MaxTextures)
      mask|=static_cast<uint8_t>(1u<<s.texture);
    if(s.indirectEnabled&&s.indirectStage<d.tev.indirectStageCount&&s.indirectStage<MaxIndStages){
      const auto&ind=d.tev.indirectStages[s.indirectStage];
      if(ind.texture<MaxTextures)mask|=static_cast<uint8_t>(1u<<ind.texture);
    }
  }
  return mask;
}

inline uint8_t pipeline_texcoord_mask(const PipelineDesc& d) noexcept {
  uint8_t mask=0;
  const unsigned stages=std::min<unsigned>(d.tev.stageCount,MaxTevStages);
  for(unsigned i=0;i<stages;i++){
    const auto&s=d.tev.stages[i];
    // The stage coordinate affects the result only when sampling a direct texture
    // or when indirect TEV consumes/propagates the base coordinate.
    const bool directSample=tev_stage_uses_texture(s)&&s.texture<MaxTextures;
    if((directSample||s.indirectEnabled)&&s.texCoord<MaxTextures)
      mask|=static_cast<uint8_t>(1u<<s.texCoord);
    if(s.indirectEnabled&&s.indirectStage<d.tev.indirectStageCount&&s.indirectStage<MaxIndStages){
      const auto&ind=d.tev.indirectStages[s.indirectStage];
      if(ind.texCoord<MaxTextures)mask|=static_cast<uint8_t>(1u<<ind.texCoord);
    }
  }
  return mask;
}

inline constexpr bool texgen_type_is_bump(TexGenType t) noexcept {
  return t>=TexGenType::Bump0&&t<=TexGenType::Bump7;
}

inline uint8_t pipeline_texgen_compute_mask(const PipelineDesc& d) noexcept {
  const unsigned count=std::min<unsigned>(d.texgenCount,MaxTextures);
  uint8_t mask=pipeline_texcoord_mask(d);
  if(count<MaxTextures)mask&=static_cast<uint8_t>((1u<<count)-1u);
  bool changed=true;
  while(changed){
    changed=false;
    for(unsigned i=0;i<count;i++)if(mask&(1u<<i)){
      const auto&t=d.texgens[i];
      if(texgen_type_is_bump(t.type)&&t.embossSource<count){
        const uint8_t bit=static_cast<uint8_t>(1u<<t.embossSource);
        if((mask&bit)==0){mask|=bit;changed=true;}
      }
    }
  }
  return mask;
}

inline bool tev_stage_uses_raster(const TevStage& s) noexcept {
  const auto colorUses=[](TevColorArg a) noexcept {
    return a==TevColorArg::RasColor||a==TevColorArg::RasAlpha;
  };
  const auto alphaUses=[](TevAlphaArg a) noexcept { return a==TevAlphaArg::RasAlpha; };
  return colorUses(s.color.a)||colorUses(s.color.b)||colorUses(s.color.c)||colorUses(s.color.d)||
         alphaUses(s.alpha.a)||alphaUses(s.alpha.b)||alphaUses(s.alpha.c)||alphaUses(s.alpha.d);
}

inline uint8_t pipeline_raster_color_mask(const PipelineDesc& d) noexcept {
  uint8_t mask=0;
  const unsigned stages=std::min<unsigned>(d.tev.stageCount,MaxTevStages);
  for(unsigned i=0;i<stages;i++){
    const auto&s=d.tev.stages[i];
    if(!tev_stage_uses_raster(s))continue;
    if(s.rasterSource==RasterSource::Color0)mask|=1u;
    else if(s.rasterSource==RasterSource::Color1)mask|=2u;
  }
  return mask;
}

struct TextureDesc {
  uint32_t width = 0;
  uint32_t height = 0;
  TextureFormat format = TextureFormat::RGBA8888;
  PaletteFormat paletteFormat = PaletteFormat::None;
  const void* data = nullptr;
  size_t dataSize = 0;
  const void* palette = nullptr;
  size_t paletteSize = 0;
  uint64_t sourceId = 0; // guest address / stable asset id
  uint64_t paletteSourceId = 0; // guest address / stable palette id
  uint32_t revision = 0;
  uint32_t paletteRevision = 0;
  uint8_t mipCount = 1; // Wii mip levels stored consecutively after level 0.
  bool cacheable = true;
  bool generateMipmaps = false; // Generate a full chain only when the source provides level 0 alone.
};
struct SamplerDesc {
  WrapMode wrapS = WrapMode::Repeat;
  WrapMode wrapT = WrapMode::Repeat;
  Filter minFilter = Filter::Linear;
  Filter magFilter = Filter::Linear;
  float lodBias = 0.f;
  float minLod = 0.f;
  float maxLod = 1000.f;
};
enum class EfbCopyFormat : uint8_t {
  Passthrough = 0,
  I4, I8, IA4, IA8, RGB565,
  R4, RA4, RA8, A8, R8, G8, B8, RG8, GB8,
  DepthZ8, DepthZ16, DepthZ24X8, DepthZ4, DepthZ8M, DepthZ8L, DepthZ16L,
  Unsupported,
  Count,
};
inline constexpr uint8_t efb_copy_sample_mode(EfbCopyFormat f) noexcept {
  switch (f) {
  case EfbCopyFormat::R4: return 1;
  case EfbCopyFormat::A8: return 2;
  default: return 0;
  }
}
inline constexpr bool is_depth_copy_format(EfbCopyFormat f) noexcept {
  return f >= EfbCopyFormat::DepthZ8 && f <= EfbCopyFormat::DepthZ16L;
}
inline constexpr bool is_supported_color_copy_format(EfbCopyFormat f) noexcept {
  return f <= EfbCopyFormat::GB8;
}
// GXTexFmt wire values are part of the FIFO ABI and are stable independently of Aurora's C++ enum types.
// Keeping this translation numeric makes the Vita backend testable without pulling Dawn/Aurora GX headers into Vita builds.
inline constexpr EfbCopyFormat efb_copy_format_from_gx_raw(uint32_t fmt) noexcept {
  switch (fmt) {
  case 0x00: return EfbCopyFormat::I4;
  case 0x01: return EfbCopyFormat::I8;
  case 0x02: return EfbCopyFormat::IA4;
  case 0x03: return EfbCopyFormat::IA8;
  case 0x04: return EfbCopyFormat::RGB565;
  case 0x05: // GX_TF_RGB5A3: Aurora can keep this as an RGBA render texture.
  case 0x06: // GX_TF_RGBA8
    return EfbCopyFormat::Passthrough;
  case 0x20: return EfbCopyFormat::R4;
  case 0x22: return EfbCopyFormat::RA4;
  case 0x23: return EfbCopyFormat::RA8;
  case 0x27: return EfbCopyFormat::A8;
  case 0x28: return EfbCopyFormat::R8;
  case 0x29: return EfbCopyFormat::G8;
  case 0x2A: return EfbCopyFormat::B8;
  case 0x2B: return EfbCopyFormat::RG8;
  case 0x2C: return EfbCopyFormat::GB8;
  case 0x11: return EfbCopyFormat::DepthZ8;
  case 0x13: return EfbCopyFormat::DepthZ16;
  case 0x16: return EfbCopyFormat::DepthZ24X8;
  case 0x30: return EfbCopyFormat::DepthZ4;
  case 0x39: return EfbCopyFormat::DepthZ8M;
  case 0x3A: return EfbCopyFormat::DepthZ8L;
  case 0x3C: return EfbCopyFormat::DepthZ16L;
  default: return EfbCopyFormat::Unsupported;
  }
}

enum class TextureSource : uint8_t { Cache, Efb };
struct TextureBinding {
  Handle texture = InvalidHandle;
  SamplerDesc sampler{};
  TextureSource source = TextureSource::Cache;
  // Native EFB copies can preserve their GPU storage orientation and express
  // the GX image convention at sampling time instead of physically mirroring
  // half a megabyte of uncached memory on the CPU.
  bool flipX = false;
  bool flipY = false;
  bool forceOpaque = false;
  // Native GXM may keep an EFB copy in RGBA8 storage and reproduce a cheap GX
  // copy-format conversion while sampling, avoiding a synchronized CPU readback.
  EfbCopyFormat sampleFormat = EfbCopyFormat::Passthrough;
  // Optional source sub-rectangle transform. Defaults preserve ordinary texture
  // bindings; native EFB blits use it to crop/scale entirely in the fragment
  // path instead of rewriting UV vertex buffers or touching uncached pixels.
  float uvScaleX = 1.f;
  float uvScaleY = 1.f;
  float uvBiasX = 0.f;
  float uvBiasY = 0.f;
};

struct BufferSlice { Handle buffer=InvalidHandle; uint32_t offset=0; uint32_t size=0; };
struct FixedVertexUniforms {
  std::array<float,12> position{{1,0,0,0, 0,1,0,0, 0,0,1,0}};
  std::array<float,12> normal{{1,0,0,0, 0,1,0,0, 0,0,1,0}};
  std::array<std::array<float,12>,10> positionPalette{};
  std::array<std::array<float,12>,10> normalPalette{};
  std::array<std::array<float,12>,MaxTextures> texture{};
  std::array<std::array<float,12>,MaxTextures> post{};
  std::array<std::array<float,4>,4> material{};
  std::array<std::array<float,4>,4> ambient{};
  // Five vec4s per GX light: position, direction, color, cosine attenuation,
  // distance attenuation. Channel light masks are baked into PipelineDesc.
  std::array<std::array<float,4>,MaxLights*5> light{};
};
struct DrawUniforms {
  std::array<float, 16> mvp{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  std::array<std::array<float,4>,4> kcolor{{{{1,1,1,1}},{{1,1,1,1}},{{1,1,1,1}},{{1,1,1,1}}}};
  std::array<std::array<float,4>,4> tevreg{{{{1,1,1,1}},{{0,0,0,0}},{{0,0,0,0}},{{0,0,0,0}}}};
  std::array<float,4> fogColor{{0,0,0,0}};
  // Aurora/GX fog parameters A, B, C and range center.
  std::array<float,4> fogParams{{0,0.5f,0,0}};
  // Ten GX fog-range K coefficients in Aurora's shader/LUT order, plus render width.
  std::array<float,10> fogRangeK{};
  float renderViewportWidth = 960.f;
  // Two vec4 rows per GX indirect matrix. xyz are matrix coefficients, w is translation/bias term.
  std::array<std::array<float,4>, MaxIndMatrices * 2> indirectMatrices{};
  // width, height, lod bias, reserved for each texture/texcoord.
  std::array<std::array<float,4>, MaxTextures> texcoordScale{};
  std::array<std::array<float,4>, MaxTextures> textureSizeBias{};
  std::array<std::array<float,4>,4> channelAmbient{};
  std::array<std::array<float,4>,4> channelMaterial{};
  std::array<std::array<float,MaxLights>,4> lightEnabled{};
  std::array<LightUniform,MaxLights> lights{};
};
// Compact GPU-facing subset of DrawUniforms. Lighting/material state above is
// consumed while the CPU transforms GX vertices and never reaches vitaGL. Keeping
// it out of every queued DrawPacket saves almost 0.9 KiB per draw on Vita.
struct GpuDrawUniforms {
  std::array<float, 16> mvp{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  std::array<std::array<float,4>,4> kcolor{{{{1,1,1,1}},{{1,1,1,1}},{{1,1,1,1}},{{1,1,1,1}}}};
  std::array<std::array<float,4>,4> tevreg{{{{1,1,1,1}},{{0,0,0,0}},{{0,0,0,0}},{{0,0,0,0}}}};
  std::array<float,4> fogColor{{0,0,0,0}};
  std::array<float,4> fogParams{{0,0.5f,0,0}};
  std::array<float,10> fogRangeK{};
  float renderViewportWidth = 960.f;
  std::array<std::array<float,4>, MaxIndMatrices * 2> indirectMatrices{};
  std::array<std::array<float,4>, MaxTextures> texcoordScale{};
  std::array<std::array<float,4>, MaxTextures> textureSizeBias{};

  GpuDrawUniforms() = default;
  GpuDrawUniforms(const DrawUniforms& src) noexcept { *this = src; }
  GpuDrawUniforms& operator=(const DrawUniforms& src) noexcept {
    std::memcpy(this, &src, sizeof(*this));
    return *this;
  }
};
static_assert(sizeof(GpuDrawUniforms) == offsetof(DrawUniforms, channelAmbient));
struct DrawPacket {
  uint64_t pipelineKey = 0;
  BufferSlice vertices{};
  BufferSlice indices{};
  uint32_t vertexCount = 0;
  uint32_t indexCount = 0;
  uint32_t firstVertex = 0;
  uint32_t instanceCount = 1;
  // Streaming GX draws can store U16 indices relative to the start of the
  // frame VBO instead of the packet's vertex slice. This lets adjacent draws
  // concatenate index ranges into one physical draw without moving vertices.
  bool absoluteVertexIndices = false;
  std::array<TextureBinding, MaxTextures> textures{};
  GpuDrawUniforms uniforms{};
  // Optional immutable snapshot owned by the submitting command batch. Never
  // points at live GX state; the owner retains it until execute() has returned.
  const FixedVertexUniforms* fixedVertexUniforms = nullptr;
  Viewport viewport{};
  Scissor scissor{};
};

struct FrameStats {
  uint32_t drawCalls = 0;
  uint32_t triangles = 0;
  uint32_t pipelineHits = 0;
  uint32_t pipelineMisses = 0;
  uint32_t pipelineMruHits = 0;
  uint32_t textureHits = 0;
  uint32_t textureMisses = 0;
  uint32_t textureUploads = 0;
  uint32_t stateChanges = 0;
  uint64_t cpuFrameUs = 0;
  // Sampled native CPU timings (zero on unsampled frames and other backends).
  bool nativeTimingsSampled = false;
  uint64_t nativePipelineUs = 0;
  uint64_t nativeTextureUs = 0;
  uint64_t nativeDrawUs = 0;
  // CPU time spent inside sceGxmDisplayQueueAddEntry. A sustained non-trivial
  // value indicates display-queue/GPU backpressure rather than frontend CPU work.
  uint64_t nativeDisplayQueueAddUs = 0;
  uint32_t nativeVertexUniformReuses = 0;
  uint32_t nativeFragmentUniformReuses = 0;
  uint32_t nativeHsrOpaqueDraws = 0;
  uint32_t nativeHsrDiscardDraws = 0;
  uint32_t nativeHsrTranslucentDraws = 0;
  uint32_t nativeHsrPassSwitches = 0;
  uint32_t nativeDirectTextureDraws = 0;
  uint32_t nativeDirectTextureSlots = 0;
  uint32_t nativeSceneCount = 0;
  uint32_t nativeDepthLoadScenes = 0;
  uint32_t nativeDepthWrittenScenes = 0;
  uint32_t nativeDepthReadOnlyScenes = 0;
  uint32_t nativeDepthClearSkippedLoads = 0;
  uint32_t nativeSceneEndFrame = 0;
  uint32_t nativeSceneEndTargetSwitch = 0;
  uint32_t nativeSceneEndTransfer = 0;
  uint32_t nativeSceneEndDisplayCopy = 0;
  uint32_t nativeSceneEndFinish = 0;
  uint32_t nativeEfbCopies = 0;
  uint32_t nativeEfbDownscaleAttempts = 0;
  uint32_t nativeEfbDownscaleSuccesses = 0;
  uint32_t nativeEfbDownscaleFallbacks = 0;
  uint64_t nativeEfbEndSceneUs = 0;
  uint64_t nativeEfbTransferSubmitUs = 0;
  uint64_t nativeEfbTransferWaitUs = 0;
  uint64_t nativeEfbCpuFixupUs = 0;
};

} // namespace aurora::vita::gfx
