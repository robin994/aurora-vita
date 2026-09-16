#include "gxm_shader_gen.hpp"
#include <array>
#include <sstream>
#include <vector>

namespace aurora::vita::gxm {
namespace {
using namespace gfx;
const char* reg(unsigned i) { static constexpr const char* names[]{"prev", "reg0", "reg1", "reg2"}; return names[i]; }
const char* component(unsigned i) { static constexpr const char* names[]{"r", "g", "b", "a"}; return names[i]; }
std::string splat(const std::string& value) { return "float3(" + value + ")"; }
std::string konst(unsigned value, bool alpha) {
  static constexpr const char* fractions[]{"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};
  if (value < 8) return alpha ? fractions[value] : splat(fractions[value]);
  if (!alpha && value < 12) return "u_kcolor[" + std::to_string(value - 8) + "].rgb";
  value -= alpha ? 8 : 12;
  const auto expression = "u_kcolor[" + std::to_string(value % 4) + "]." + component(value / 4);
  return alpha ? expression : splat(expression);
}
std::string color_arg(TevColorArg arg, const TevStage& s, const std::array<bool, 4>& cn,
                      const std::array<bool, 4>& an) {
  const unsigned value = static_cast<unsigned>(arg);
  if (value < 8) {
    const auto expression = std::string(reg(value / 2)) + (value & 1 ? ".a" : ".rgb");
    const auto result = value & 1 ? splat(expression) : expression;
    return (value & 1 ? an[value / 2] : cn[value / 2]) ? result : "tev_wrap3(" + result + ")";
  }
  switch (arg) {
  case TevColorArg::TexColor: return "texc.rgb";
  case TevColorArg::TexAlpha: return splat("texc.a");
  case TevColorArg::RasColor: return "rasc.rgb";
  case TevColorArg::RasAlpha: return splat("rasc.a");
  case TevColorArg::One: return splat("1.0");
  case TevColorArg::Half: return splat("0.5");
  case TevColorArg::Konst: return konst(static_cast<unsigned>(s.konstColor), false);
  default: return splat("0.0");
  }
}
std::string alpha_arg(TevAlphaArg arg, const TevStage& s, const std::array<bool, 4>& normalized) {
  const unsigned value = static_cast<unsigned>(arg);
  if (value < 4) {
    const auto result = std::string(reg(value)) + ".a";
    return normalized[value] ? result : "tev_wrap1(" + result + ")";
  }
  switch (arg) {
  case TevAlphaArg::TexAlpha: return "texc.a";
  case TevAlphaArg::RasAlpha: return "rasc.a";
  case TevAlphaArg::Konst: return konst(static_cast<unsigned>(s.konstAlpha), true);
  default: return "0.0";
  }
}
std::string swizzle(const char* value, const TevSwapDesc& s) {
  return std::string(value) + "." + component(static_cast<unsigned>(s.r)) +
      component(static_cast<unsigned>(s.g)) + component(static_cast<unsigned>(s.b)) +
      component(static_cast<unsigned>(s.a));
}
std::string comparison(Compare op, const std::string& a, const std::string& b) {
  static constexpr const char* ops[]{"", "<", "==", "<=", ">", "!=", ">=", ""};
  if (op == Compare::Never) return "false";
  if (op == Compare::Always) return "true";
  return "(" + a + ops[static_cast<unsigned>(op)] + b + ")";
}
void signature(std::ostringstream& stream, const std::vector<std::string>& parameters) {
  for (size_t i = 0; i < parameters.size(); ++i) {
    if (i) stream << ",\n";
    stream << parameters[i];
  }
}
std::string tev_arithmetic(bool alpha, TevOp op, TevBias b, TevScale s) {
  const std::string p = alpha ? "a" : "c";
  const auto A = p + "A", B = p + "B", C = p + "C", D = p + "D";
  if (op == TevOp::Add || op == TevOp::Sub) {
    static constexpr const char* bias[]{"", "+0.5", "-0.5"};
    static constexpr const char* scale[]{"1.0", "2.0", "4.0", "0.5"};
    return "(" + D + (op == TevOp::Add ? "+" : "-") + "lerp(" + A + "," + B + "," + C + ")" +
        bias[static_cast<unsigned>(b)] + ")*" + scale[static_cast<unsigned>(s)];
  }
  const unsigned mode = static_cast<unsigned>(op) - static_cast<unsigned>(TevOp::CompR8Greater);
  const std::string relation = mode & 1 ? "==" : ">";
  if (alpha) return D + "+((floor(" + A + "*255.0+0.5)" + relation + "floor(" + B + "*255.0+0.5))?" + C + ":0.0)";
  if (mode / 2 == 3) {
    std::string result = D + "+float3(";
    for (unsigned i = 0; i < 3; ++i) {
      if (i) result += ",";
      const auto c = std::string(".") + component(i);
      result += "(floor(" + A + c + "*255.0+0.5)" + relation + "floor(" + B + c + "*255.0+0.5))?" + C + c + ":0.0";
    }
    return result + ")";
  }
  const auto packed = [&](const std::string& value) {
    if (mode / 2 == 0) return "floor(" + value + ".r*255.0+0.5)";
    if (mode / 2 == 1) return "floor(dot(" + value + ".rg*255.0,float2(1.0,256.0))+0.5)";
    return "floor(dot(" + value + "*255.0,float3(1.0,256.0,65536.0))+0.5)";
  };
  return D + "+((" + packed(A) + relation + packed(B) + ")?" + C + ":float3(0.0))";
}
} // namespace

std::string validate_pipeline(const gfx::PipelineDesc& d) {
  if (!d.tev.stageCount || d.tev.stageCount > MaxTevStages || d.texgenCount > MaxTextures)
    return "invalid TEV/texgen count";
  if (d.fixedVertexOnGpu) return "GXM requires CPU-prepared vertices; fixed GX GPU transform is not implemented";
  if (d.fogMode != FogMode::None || d.fogRangeEnabled) return "GXM fog is not implemented";
  if (d.polygonOffset) return "GXM polygon offset is not implemented";
  if (d.blendMode == BlendMode::Logic) return "GXM logic operations are not implemented";
  if (static_cast<unsigned>(d.blendMode) > static_cast<unsigned>(BlendMode::Logic) ||
      static_cast<unsigned>(d.srcFactor) > static_cast<unsigned>(BlendFactor::OneMinusDstAlpha) ||
      static_cast<unsigned>(d.dstFactor) > static_cast<unsigned>(BlendFactor::OneMinusDstAlpha) ||
      static_cast<unsigned>(d.cull) > static_cast<unsigned>(CullMode::All) ||
      static_cast<unsigned>(d.depthFunc) > static_cast<unsigned>(Compare::Always) ||
      d.dstAlpha < -1 || d.dstAlpha > 255) return "invalid fixed pipeline state";
  if (d.primitive != Primitive::Triangles && d.primitive != Primitive::TriangleStrip && d.primitive != Primitive::TriangleFan)
    return "GXM expects triangle primitives; expand GX lines/points in common code first";
  for (const auto& s : d.tev.swapTable)
    if (static_cast<unsigned>(s.r) > 3 || static_cast<unsigned>(s.g) > 3 ||
        static_cast<unsigned>(s.b) > 3 || static_cast<unsigned>(s.a) > 3) return "invalid TEV swap";
  const auto& ac = d.tev.alphaCompare;
  if (static_cast<unsigned>(ac.comp0) > 7 || static_cast<unsigned>(ac.comp1) > 7 || ac.op > 3)
    return "invalid alpha compare";
  for (unsigned i = 0; i < d.tev.stageCount; ++i) {
    const auto& s = d.tev.stages[i];
    if (s.indirectEnabled) return "GXM indirect TEV is not implemented";
    if (static_cast<unsigned>(s.rasterSource) > static_cast<unsigned>(RasterSource::Zero)) return "invalid raster source";
    if (tev_stage_uses_raster(s) && (s.rasterSource == RasterSource::AlphaBump || s.rasterSource == RasterSource::AlphaBumpN))
      return "GXM alpha bump is not implemented";
    if (s.colorOut > TevReg::Reg2 || s.alphaOut > TevReg::Reg2 || s.colorOp > TevOp::CompRGB8Equal ||
        s.alphaOp > TevOp::CompRGB8Equal || s.colorBias > TevBias::SubHalf || s.alphaBias > TevBias::SubHalf ||
        s.colorScale > TevScale::Divide2 || s.alphaScale > TevScale::Divide2 ||
        s.konstColor > KonstColorSel::K3A || s.konstAlpha > KonstAlphaSel::K3A || s.rasSwap > 3 || s.texSwap > 3)
      return "invalid TEV operation";
    for (auto a : {s.color.a, s.color.b, s.color.c, s.color.d}) if (a > TevColorArg::Zero) return "invalid TEV color input";
    for (auto a : {s.alpha.a, s.alpha.b, s.alpha.c, s.alpha.d}) if (a > TevAlphaArg::Zero) return "invalid TEV alpha input";
    if ((s.texture >= MaxTextures && s.texture != 0xff) || (s.texCoord >= MaxTextures && s.texCoord != 0xff))
      return "invalid texture selector";
  }
  if (!d.layout.count || d.layout.count > MaxVertexAttributes) return "invalid vertex layout count";
  uint32_t supplied = 0;
  const unsigned stride = d.layout.attributes[0].stride;
  if (!stride || stride % 4) return "vertex stride must be a nonzero multiple of four";
  for (unsigned i = 0; i < d.layout.count; ++i) {
    const auto& a = d.layout.attributes[i];
    if (a.location > 10 || supplied & (1u << a.location) || a.stride != stride || !a.components || a.components > 4 ||
        a.scalar > VertexScalar::U16) return "invalid or duplicate vertex attribute";
    const unsigned scalar = a.scalar == VertexScalar::F32 ? 4 : (a.scalar == VertexScalar::U16 || a.scalar == VertexScalar::S16 ? 2 : 1);
    if (a.offset % scalar || static_cast<unsigned>(a.offset) + a.components * scalar > stride)
      return "vertex attribute exceeds its stride or is misaligned";
    if (a.location == 0 && (a.components != 4 || a.scalar != VertexScalar::F32)) return "position must be float4";
    if (a.location >= 3 && a.components != 3) return "prepared texture coordinates must have three components";
    if ((a.location == 1 || a.location == 2) && a.components != 4) return "prepared colors must have four components";
    supplied |= 1u << a.location;
  }
  const uint32_t required = 1u | (uint32_t(pipeline_raster_color_mask(d)) << 1) | (uint32_t(pipeline_texcoord_mask(d)) << 3);
  if ((supplied & required) != required) return "vertex layout does not provide all shader inputs";
  return {};
}

ShaderSources build_tev_cg(const gfx::PipelineDesc& d) {
  ShaderSources out;
  out.error = validate_pipeline(d);
  if (!out.error.empty()) return out;
  out.textureMask = pipeline_sampled_texture_mask(d);
  out.texcoordMask = pipeline_texcoord_mask(d);
  out.colorMask = pipeline_raster_color_mask(d);
  std::ostringstream vs, fs;
  std::vector<std::string> vp{"float4 a_position : POSITION", "out float4 v_position : POSITION"};
  std::vector<std::string> fp{"float4 window_position : WPOS", "uniform float4 u_clip_rect",
      "uniform float4 u_kcolor[4]", "uniform float4 u_tevreg[4]"};
  if (!d.positionIsClipSpace) vp.push_back("uniform float4 u_mvp[4]");
  for (unsigned i = 0; i < 2; ++i) if (out.colorMask & (1u << i)) {
    const auto n = std::to_string(i);
    vp.push_back("float4 a_color" + n + " : COLOR" + n);
    vp.push_back("out float4 v_color" + n + " : COLOR" + n);
    fp.push_back("float4 v_color" + n + " : COLOR" + n);
  }
  for (unsigned i = 0; i < MaxTextures; ++i) {
    const auto n = std::to_string(i);
    if (out.texcoordMask & (1u << i)) {
      vp.push_back("float3 a_tex" + n + " : TEXCOORD" + n);
      vp.push_back("out float3 v_tex" + n + " : TEXCOORD" + n);
      fp.push_back("float3 v_tex" + n + " : TEXCOORD" + n);
    }
    if (out.textureMask & (1u << i)) fp.push_back("uniform sampler2D u_tex" + n + " : TEXUNIT" + n);
  }
  vs << "void main(\n"; signature(vs, vp); vs << "){\nfloat4 p=";
  if (d.positionIsClipSpace) vs << "a_position;\n";
  else vs << "u_mvp[0]*a_position.x+u_mvp[1]*a_position.y+u_mvp[2]*a_position.z+u_mvp[3]*a_position.w;\n";
  // Shared projection values retain Aurora's GX depth convention. With the
  // native viewport zOffset/zScale=(near+far)/2,(far-near)/2 this is the same
  // effective depth as the existing renderer, without an OpenGL call.
  vs << (d.reversedZ ? "p.z=-2.0*p.z-p.w;\n" : "p.z=2.0*p.z+p.w;\n") << "v_position=p;\n";
  for (unsigned i = 0; i < 2; ++i) if (out.colorMask & (1u << i)) vs << "v_color" << i << "=a_color" << i << ";\n";
  for (unsigned i = 0; i < MaxTextures; ++i) if (out.texcoordMask & (1u << i)) vs << "v_tex" << i << "=a_tex" << i << ";\n";
  vs << "}\n";
  fs << "float tev_wrap1(float v){float b=v*255.0;return (b-floor(b/256.0)*256.0)/255.0;}\n"
        "float3 tev_wrap3(float3 v){float3 b=v*255.0;return (b-floor(b/256.0)*256.0)/255.0;}\n"
        "float4 main(\n";
  signature(fs, fp);
  fs << ") : COLOR {\n"
        "if(window_position.x<u_clip_rect.x||window_position.y<u_clip_rect.y||window_position.x>=u_clip_rect.z||window_position.y>=u_clip_rect.w) discard;\n"
        "float4 prev=u_tevreg[0],reg0=u_tevreg[1],reg1=u_tevreg[2],reg2=u_tevreg[3];\n";
  std::array<bool, 4> cn{}, an{};
  for (unsigned i = 0; i < d.tev.stageCount; ++i) {
    const auto& s = d.tev.stages[i];
    fs << "{\nfloat4 raw_tex=";
    if (tev_stage_uses_texture(s) && s.texture < MaxTextures) {
      std::string uv = "float2(0.0)";
      if (s.texCoord < MaxTextures) {
        const auto tex = "v_tex" + std::to_string(s.texCoord);
        uv = tex + ".xy";
        if (s.texCoord < d.texgenCount && d.texgens[s.texCoord].type == TexGenType::Matrix3x4) uv = "(" + uv + "/" + tex + ".z)";
      }
      fs << "tex2D(u_tex" << unsigned(s.texture) << "," << uv << ");\n";
    } else fs << "float4(1.0);\n";
    fs << "float4 texc=" << swizzle("raw_tex", d.tev.swapTable[s.texSwap]) << ";\nfloat4 raw_ras=";
    if (!tev_stage_uses_raster(s) || s.rasterSource == RasterSource::Zero) fs << "float4(0.0)";
    else fs << (s.rasterSource == RasterSource::Color1 ? "v_color1" : "v_color0");
    fs << ";\nfloat4 rasc=" << swizzle("raw_ras", d.tev.swapTable[s.rasSwap]) << ";\n";
    const TevColorArg colors[]{s.color.a,s.color.b,s.color.c,s.color.d};
    const TevAlphaArg alphas[]{s.alpha.a,s.alpha.b,s.alpha.c,s.alpha.d};
    for (unsigned j = 0; j < 4; ++j) {
      fs << "float3 c" << char('A'+j) << "=" << color_arg(colors[j], s, cn, an) << ";\n";
      fs << "float a" << char('A'+j) << "=" << alpha_arg(alphas[j], s, an) << ";\n";
    }
    fs << "float3 result_c=clamp(" << tev_arithmetic(false,s.colorOp,s.colorBias,s.colorScale)
       << (s.colorClamp ? ",0.0,1.0);\n" : ",-4.0,4.0);\n")
       << "float result_a=clamp(" << tev_arithmetic(true,s.alphaOp,s.alphaBias,s.alphaScale)
       << (s.alphaClamp ? ",0.0,1.0);\n" : ",-4.0,4.0);\n")
       << reg(unsigned(s.colorOut)) << ".rgb=result_c;\n" << reg(unsigned(s.alphaOut)) << ".a=result_a;\n}\n";
    cn[unsigned(s.colorOut)] = s.colorClamp;
    an[unsigned(s.alphaOut)] = s.alphaClamp;
  }
  const auto& last = d.tev.stages[d.tev.stageCount - 1];
  fs << "float4 result=float4(" << reg(unsigned(last.colorOut)) << ".rgb," << reg(unsigned(last.alphaOut)) << ".a);\n";
  if (!last.colorClamp) fs << "result.rgb=tev_wrap3(result.rgb);\n";
  if (!last.alphaClamp) fs << "result.a=tev_wrap1(result.a);\n";
  const auto& ac = d.tev.alphaCompare;
  const auto a = comparison(ac.comp0, "floor(result.a*255.0+0.5)", std::to_string(ac.ref0)+".0");
  const auto b = comparison(ac.comp1, "floor(result.a*255.0+0.5)", std::to_string(ac.ref1)+".0");
  static constexpr const char* operators[]{"&&", "||", "!=", "=="};
  fs << "if(!(" << a << operators[ac.op] << b << ")) discard;\n";
  if (d.dstAlpha >= 0) fs << "result.a=" << d.dstAlpha << ".0/255.0;\n";
  fs << "return result;\n}\n";
  out.vertex = vs.str(); out.fragment = fs.str();
  return out;
}
} // namespace aurora::vita::gxm
