#include "gxm_shader_gen.hpp"
#include "gfx/vita_fixed_vertex.hpp"
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
                      const std::array<bool, 4>& an, bool wrap = true) {
  const unsigned value = static_cast<unsigned>(arg);
  if (value < 8) {
    const auto expression = std::string(reg(value / 2)) + (value & 1 ? ".a" : ".rgb");
    const auto result = value & 1 ? splat(expression) : expression;
    return (!wrap || (value & 1 ? an[value / 2] : cn[value / 2])) ? result : "tev_wrap3(" + result + ")";
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
std::string alpha_arg(TevAlphaArg arg, const TevStage& s, const std::array<bool, 4>& normalized, bool wrap = true) {
  const unsigned value = static_cast<unsigned>(arg);
  if (value < 4) {
    const auto result = std::string(reg(value)) + ".a";
    return (!wrap || normalized[value]) ? result : "tev_wrap1(" + result + ")";
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
std::string alpha_test(const std::string& a, const std::string& b, unsigned op) {
  // Do not leave a syntactic discard in an unconditional-pass shader or rely
  // on the runtime compiler to remove its kill metadata.
  if (a == b) return op < 2 ? a : op == 2 ? "false" : "true";
  const bool ac = a == "true" || a == "false";
  const bool bc = b == "true" || b == "false";
  if (ac && bc) {
    const bool av = a == "true", bv = b == "true";
    const bool values[]{av && bv, av || bv, av != bv, av == bv};
    return values[op] ? "true" : "false";
  }
  if (ac || bc) {
    const auto& value = ac ? b : a;
    const bool constant = (ac ? a : b) == "true";
    switch (op) {
    case 0: return constant ? value : "false";
    case 1: return constant ? "true" : value;
    case 2: return constant ? "!(" + value + ")" : value;
    case 3: return constant ? value : "!(" + value + ")";
    }
  }
  static constexpr const char* operators[]{"&&", "||", "!=", "=="};
  return a + operators[op] + b;
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
std::string coordinate(const PipelineDesc& d,unsigned index) {
  if(index>=MaxTextures || !(pipeline_texcoord_mask(d)&(1u<<index)))return "float2(0.0)";
  const auto v="v_tex"+std::to_string(index);
  return index<d.texgenCount && d.texgens[index].type==TexGenType::Matrix3x4?
      "("+v+".xy/"+v+".z)":v+".xy";
}
std::string wrapped(IndirectWrap mode,const std::string& value) {
  if(mode==IndirectWrap::Off)return value;
  if(mode==IndirectWrap::W0)return "0.0";
  const auto size=std::to_string(256u>>(unsigned(mode)-1u))+".0";
  // Cg fmod has different behavior for negative inputs than GX/GLSL modulo.
  return "("+value+"-floor("+value+"/"+size+")*"+size+")";
}
void indirect(std::ostringstream& fs,const PipelineDesc& d,const TevStage& s) {
  if(!s.indirectEnabled)return;
  const auto& ind=d.tev.indirectStages[s.indirectStage];
  const unsigned itc=ind.texCoord,itex=ind.texture;
  const unsigned divisors[]{1,8,16,32};
  fs<<"float2 ind_uv="<<coordinate(d,itc)<<"*max(u_texcoord_scale["<<itc<<"].xy,float2(1.0))*float2("
    <<(1.f/float(1u<<ind.scaleSShift))<<","<<(1.f/float(1u<<ind.scaleTShift))
    <<")/max(u_texture_size_bias["<<itex<<"].xy,float2(1.0));\n"
      "float4 ind_sample=tex2D(u_tex"<<itex<<",gx_sample_uv(ind_uv,u_tex_transform["<<itex<<"]));\n"
      "ind_sample.a=lerp(ind_sample.a,1.0,u_tex_force_opaque["<<itex<<"]);\n"
      "float3 ind_raw=ind_sample.abg*255.0;\nfloat3 indv=floor(ind_raw/"<<divisors[unsigned(s.indirectFormat)]<<".0);\n";
  const unsigned bias=unsigned(s.indirectBias);
  const char* biasValue=s.indirectFormat==IndirectFormat::Bits8?"-128.0":"1.0";
  if(bias&1u)fs<<"indv.x+="<<biasValue<<";\n";
  if(bias&2u)fs<<"indv.y+="<<biasValue<<";\n";
  if(bias&4u)fs<<"indv.z+="<<biasValue<<";\n";
  if(s.indirectAlpha!=IndirectAlphaSel::Off) {
    const char axis="xyz"[unsigned(s.indirectAlpha)-1];
    const unsigned step=s.indirectFormat==IndirectFormat::Bits5?32:s.indirectFormat==IndirectFormat::Bits4?16:8;
    fs<<"ind_alpha=floor(ind_raw."<<axis<<"/"<<step<<".0)*"<<step<<".0/255.0;\n";
  }
  const unsigned tc=s.texCoord<MaxTextures?s.texCoord:0,tex=s.texture<MaxTextures?s.texture:0;
  const bool simple=s.indirectMatrix==IndirectMatrix::Off && !s.indirectAddPrev;
  fs<<"float2 base_texel=tev_uv";
  if(!simple)fs<<"*max(u_texcoord_scale["<<tc<<"].xy,float2(1.0))";
  fs<<";\nbase_texel=float2("<<wrapped(s.indirectWrapS,"base_texel.x")<<","<<wrapped(s.indirectWrapT,"base_texel.y")<<");\n";
  const unsigned matrix=unsigned(s.indirectMatrix);
  if(!matrix)fs<<"float2 ind_off=float2(0.0);\n";
  else if(matrix<=unsigned(IndirectMatrix::Mtx2)) {
    const unsigned m=matrix-unsigned(IndirectMatrix::Mtx0);
    fs<<"float4 im0=u_ind_mtx["<<m*2<<"],im1=u_ind_mtx["<<m*2+1<<"];\n"
      "float2 ind_off=float2(dot(float3(im0.x,im0.z,im1.x),indv),dot(float3(im0.y,im0.w,im1.y),indv))*im1.z;\n";
  } else {
    const bool rowS=matrix<=unsigned(IndirectMatrix::S2);
    const unsigned m=matrix-unsigned(rowS?IndirectMatrix::S0:IndirectMatrix::T0);
    fs<<"float2 ind_off="<<coordinate(d,tc)<<"*max(u_texcoord_scale["<<tc<<"].xy,float2(1.0))*indv."
      <<(rowS?'x':'y')<<"*u_ind_mtx["<<m*2+1<<"].z/256.0;\n";
  }
  fs<<"prev_ind_uv"<<(s.indirectAddPrev?"+=":"=")<<"base_texel+ind_off;\ntev_uv=prev_ind_uv";
  if(!simple)fs<<"/max(u_texture_size_bias["<<tex<<"].xy,float2(1.0))";
  fs<<";\n";
}
std::string fog_range(const std::string& index) {
  std::string result="u_fog_range_k[9]";
  for(int i=8;i>=0;--i)result="("+index+"<"+std::to_string(i)+".5?u_fog_range_k["+std::to_string(i)+"]:"+result+")";
  return result;
}
} // namespace

std::string validate_pipeline(const gfx::PipelineDesc& d) {
  if (!d.tev.stageCount || d.tev.stageCount > MaxTevStages || d.texgenCount > MaxTextures)
    return "invalid TEV/texgen count";
  if (d.fogMode>FogMode::RevExp2 || d.tev.indirectStageCount>MaxIndStages) return "invalid fog/indirect state";
  if (d.polygonOffset) return "GXM polygon offset is not implemented";
  if (d.blendMode == BlendMode::Logic && d.logicOp!=LogicOp::Clear && d.logicOp!=LogicOp::Copy && d.logicOp!=LogicOp::Noop)
    return "GXM supports only CLEAR, COPY and NOOP logic operations";
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
    if(s.indirectEnabled) {
      if(s.indirectStage>=d.tev.indirectStageCount || s.indirectStage>=MaxIndStages ||
          s.indirectFormat>IndirectFormat::Bits3 || s.indirectBias>IndirectBias::STU ||
          s.indirectMatrix>IndirectMatrix::T2 || s.indirectAlpha>IndirectAlphaSel::U ||
          s.indirectWrapS>IndirectWrap::W0 || s.indirectWrapT>IndirectWrap::W0) return "invalid indirect TEV stage";
      const auto& ind=d.tev.indirectStages[s.indirectStage];
      if(ind.texture>=MaxTextures || ind.texCoord>=MaxTextures || ind.scaleSShift>8 || ind.scaleTShift>8)
        return "invalid indirect TEV texture/scale";
    }
    if (static_cast<unsigned>(s.rasterSource) > static_cast<unsigned>(RasterSource::Zero)) return "invalid raster source";
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
    if (a.location > 14 || supplied & (1u << a.location) || a.stride != stride || !a.components || a.components > 4 ||
        a.scalar > VertexScalar::U16) return "invalid or duplicate vertex attribute";
    const unsigned scalar = a.scalar == VertexScalar::F32 ? 4 : (a.scalar == VertexScalar::U16 || a.scalar == VertexScalar::S16 ? 2 : 1);
    if (a.offset % scalar || static_cast<unsigned>(a.offset) + a.components * scalar > stride)
      return "vertex attribute exceeds its stride or is misaligned";
    if (a.location == 0 && (a.components != 4 || a.scalar != VertexScalar::F32)) return "position must be float4";
    if (a.location >= 3 && a.location <= 10 && a.components != 3) return "prepared texture coordinates must have three components";
    if (a.location >= 11 && a.location <= 13 && a.components != 3) return "fixed GX basis inputs must have three components";
    if (a.location == 14 && (a.components != 1 || a.scalar != VertexScalar::U8)) return "GX PN matrix selector must be u8";
    if ((a.location == 1 || a.location == 2) && a.components != 4) return "prepared colors must have four components";
    supplied |= 1u << a.location;
  }
  uint32_t required = 0;
  if (d.fixedVertexOnGpu) {
    const auto expected = fixed_vertex_gpu_layout(d);
    for (unsigned i=0;i<expected.count;++i) required |= 1u << expected.attributes[i].location;
  } else {
    required = 1u | (uint32_t(pipeline_raster_color_mask(d)) << 1) | (uint32_t(pipeline_texcoord_mask(d)) << 3);
  }
  if ((supplied & required) != required) return "vertex layout does not provide all shader inputs";
  return {};
}

std::string fixed_vertex_source_cg(TexGenSource source) {
  switch(source) {
  case TexGenSource::Position:return "float4(a_position.xyz,1.0)";
  case TexGenSource::Normal:return "float4(a_normal,1.0)";
  case TexGenSource::Binormal:return "float4(a_binormal,1.0)";
  case TexGenSource::Tangent:return "float4(a_tangent,1.0)";
  case TexGenSource::Color0:return "a_color0";
  case TexGenSource::Color1:return "a_color1";
  default: {
    const unsigned i=static_cast<unsigned>(source)-static_cast<unsigned>(TexGenSource::Tex0);
    return i<MaxTextures?"float4(a_tex"+std::to_string(i)+".xy,1.0,1.0)":"float4(0.0,0.0,1.0,1.0)";
  }
  }
}

bool fixed_vertex_lighting(const PipelineDesc& d) noexcept {
  for(const auto& c:d.colorChannels)if(c.lightingEnabled)return true;
  return false;
}

void emit_fixed_channel_cg(std::ostringstream& vs,const PipelineDesc& d,unsigned ch,
                           unsigned color,bool alpha) {
  const auto& c=d.colorChannels[ch];
  const std::string n=std::to_string(color);
  const std::string sw=alpha?".a":".rgb";
  const std::string mat=c.materialSource==ColorSource::Vertex?
      "a_color"+n:"u_gx_material["+std::to_string(ch)+"]";
  if(!c.lightingEnabled) {
    if(c.materialSource==ColorSource::Vertex)
      vs<<"v_color"<<n<<sw<<"="<<mat<<sw<<";\n";
    else if(alpha)
      vs<<"gxi=clamp("<<mat<<".a*255.0,0.0,255.0);v_color"<<n<<".a=floor(gxi+0.5)/255.0;\n";
    else
      vs<<"gxq=clamp("<<mat<<".rgb*255.0,0.0,255.0);v_color"<<n<<".rgb=floor(gxq+0.5)/255.0;\n";
    return;
  }
  const std::string amb=c.ambientSource==ColorSource::Vertex?
      "a_color"+n:"u_gx_ambient["+std::to_string(ch)+"]";
  vs<<"{float4 gx_lit="<<amb<<";\n";
  for(unsigned li=0;li<MaxLights;++li)if(c.lightMask&(1u<<li)) {
    const unsigned b=li*5u;
    vs<<"{float3 gx_ldir=u_gx_light["<<b<<"].xyz-mv;float gx_dist2=dot(gx_ldir,gx_ldir);"
         "float gx_dist=sqrt(max(gx_dist2,0.00000000000000000001));gx_ldir*=1.0/gx_dist;float gx_attn=1.0;\n";
    if(c.attenuation==AttenuationFn::Spot) {
      vs<<"float gx_cos=max(0.0,dot(gx_ldir,u_gx_light["<<b+1<<"].xyz));"
           "float gx_ca=u_gx_light["<<b+3<<"].x+u_gx_light["<<b+3<<"].y*gx_cos+u_gx_light["<<b+3<<"].z*gx_cos*gx_cos;"
           "float gx_da=u_gx_light["<<b+4<<"].x+u_gx_light["<<b+4<<"].y*gx_dist+u_gx_light["<<b+4<<"].z*gx_dist2;"
           "gx_attn=gx_da!=0.0?max(0.0,gx_ca/gx_da):0.0;\n";
    } else if(c.attenuation==AttenuationFn::Specular) {
      vs<<"float gx_spec=dot(gx_nrm,gx_ldir)>=0.0?max(0.0,dot(gx_nrm,u_gx_light["<<b+1<<"].xyz)):0.0;"
           "float gx_ca=u_gx_light["<<b+3<<"].x+u_gx_light["<<b+3<<"].y*gx_spec+u_gx_light["<<b+3<<"].z*gx_spec*gx_spec;"
           "float3 gx_da3=u_gx_light["<<b+4<<"].xyz;";
      if(c.diffuse!=DiffuseFn::None)
        vs<<"float gx_dal=sqrt(dot(gx_da3,gx_da3));gx_da3=gx_dal>0.0000000001?gx_da3/gx_dal:float3(0.0);";
      vs<<"float gx_da=max(0.0,gx_da3.x+gx_da3.y*gx_spec+gx_da3.z*gx_spec*gx_spec);"
           "gx_attn=gx_da!=0.0?max(0.0,gx_ca/gx_da):0.0;\n";
    }
    if(c.diffuse==DiffuseFn::Signed)
      vs<<"float gx_diff=dot(gx_ldir,gx_nrm);\n";
    else if(c.diffuse==DiffuseFn::Clamp)
      vs<<"float gx_diff=max(0.0,dot(gx_ldir,gx_nrm));\n";
    else
      vs<<"float gx_diff=1.0;\n";
    vs<<"gx_lit+=u_gx_light["<<b+2<<"]*(gx_attn*gx_diff);}\n";
  }
  if(alpha)
    vs<<"gxi=clamp(("<<mat<<"*clamp(gx_lit,0.0,1.0)).a*255.0,0.0,255.0);v_color"<<n<<".a=floor(gxi+0.5)/255.0;}\n";
  else
    vs<<"gxq=clamp(("<<mat<<"*clamp(gx_lit,0.0,1.0)).rgb*255.0,0.0,255.0);v_color"<<n<<".rgb=floor(gxq+0.5)/255.0;}\n";
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
  std::vector<std::string> fp{"uniform float4 u_clip_rect",
      "uniform float4 u_kcolor[4]", "uniform float4 u_tevreg[4]",
      "uniform float4 u_ind_mtx[6]", "uniform float4 u_texcoord_scale[8]", "uniform float4 u_texture_size_bias[8]",
      "uniform float4 u_tex_transform[8]", "uniform float4 u_tex_wrap[8]", "uniform float u_tex_force_opaque[8]",
      "uniform float u_tex_copy_mode[8]",
      "uniform float4 u_fog_color", "uniform float4 u_fog_params", "uniform float u_fog_range_k[10]",
      "uniform float u_render_viewport_width"};
  if (d.fragmentScissor || d.fogMode != FogMode::None)
    fp.push_back("float4 window_position : WPOS");
  if (!d.positionIsClipSpace || d.fixedVertexOnGpu) vp.push_back("uniform float4 u_mvp[4]");
  const auto fixedInputs=d.fixedVertexOnGpu?fixed_vertex_gpu_inputs(d):VertexSemanticMask{};
  for (unsigned i = 0; i < 2; ++i) {
    const auto n = std::to_string(i);
    const bool inputColor=!d.fixedVertexOnGpu ||
        (fixedInputs&vertex_semantic_bit(i?VertexSemantic::Color1:VertexSemantic::Color0));
    if(inputColor)
      vp.push_back("float4 a_color" + n + " : COLOR" + n);
    if(out.colorMask & (1u << i)) {
      vp.push_back("out float4 v_color" + n + " : COLOR" + n);
      fp.push_back("float4 v_color" + n + " : COLOR" + n);
    }
  }
  if(d.fixedVertexOnGpu) {
    if(fixedInputs&vertex_semantic_bit(VertexSemantic::Normal))vp.push_back("float3 a_normal : TEXCOORD8");
    if(fixedInputs&vertex_semantic_bit(VertexSemantic::Binormal))vp.push_back("float3 a_binormal : TEXCOORD9");
    if(fixedInputs&vertex_semantic_bit(VertexSemantic::Tangent))vp.push_back("float3 a_tangent : TEXCOORD10");
    if(d.fixedVertexIndexedPn) {
      vp.push_back("float a_pn_mtx : TEXCOORD11");
      vp.push_back("uniform float4 u_gx_position_palette[30]");
      if(fixedInputs&vertex_semantic_bit(VertexSemantic::Normal))vp.push_back("uniform float4 u_gx_normal_palette[30]");
    } else {
      vp.push_back("uniform float4 u_gx_position[3]");
      if(fixedInputs&vertex_semantic_bit(VertexSemantic::Normal))vp.push_back("uniform float4 u_gx_normal[3]");
    }
    vp.push_back("uniform float4 u_gx_material[4]");
    if(fixed_vertex_lighting(d)) {
      vp.push_back("uniform float4 u_gx_ambient[4]");
      vp.push_back("uniform float4 u_gx_light[40]");
    }
  }
  for (unsigned i = 0; i < MaxTextures; ++i) {
    const auto n = std::to_string(i);
    const bool fixedInput=d.fixedVertexOnGpu &&
      (fixedInputs&vertex_semantic_bit(static_cast<VertexSemantic>(static_cast<unsigned>(VertexSemantic::Tex0)+i)));
    if ((!d.fixedVertexOnGpu && (out.texcoordMask & (1u << i))) || fixedInput)
      vp.push_back("float3 a_tex" + n + " : TEXCOORD" + n);
    if (out.texcoordMask & (1u << i)) {
      vp.push_back("out float3 v_tex" + n + " : TEXCOORD" + n);
      fp.push_back("float3 v_tex" + n + " : TEXCOORD" + n);
      if(d.fixedVertexOnGpu && i<d.texgenCount) {
        const auto& t=d.texgens[i];
        if(t.matrix>=0&&t.type!=TexGenType::SRTG)vp.push_back("uniform float4 u_gx_texture"+n+"[3]");
        if(t.postMatrix>=0)vp.push_back("uniform float4 u_gx_post"+n+"[3]");
      }
    }
    if (out.textureMask & (1u << i)) fp.push_back("uniform sampler2D u_tex" + n + " : TEXUNIT" + n);
  }
  vs << "void main(\n"; signature(vs, vp); vs << "){\n";
  if(d.fixedVertexOnGpu) {
    vs << "float4 object_pos=float4(a_position.xyz,1.0);\n";
    if(d.fixedVertexIndexedPn) {
      vs << "int gxmi=(int)clamp(floor(a_pn_mtx+0.5),0.0,9.0)*3;\n"
            "float3 mv=float3(dot(u_gx_position_palette[gxmi],object_pos),dot(u_gx_position_palette[gxmi+1],object_pos),dot(u_gx_position_palette[gxmi+2],object_pos));\n";
      if(fixedInputs&vertex_semantic_bit(VertexSemantic::Normal))
        vs << "float3 gx_nrm=float3(dot(u_gx_normal_palette[gxmi].xyz,a_normal),dot(u_gx_normal_palette[gxmi+1].xyz,a_normal),dot(u_gx_normal_palette[gxmi+2].xyz,a_normal));"
              "float gx_nl2=dot(gx_nrm,gx_nrm);gx_nrm=gx_nl2>0.00000000000000000001?gx_nrm/sqrt(gx_nl2):float3(0.0);\n";
    } else {
      vs << "float3 mv=float3(dot(u_gx_position[0],object_pos),dot(u_gx_position[1],object_pos),dot(u_gx_position[2],object_pos));\n";
      if(fixedInputs&vertex_semantic_bit(VertexSemantic::Normal))
        vs << "float3 gx_nrm=float3(dot(u_gx_normal[0].xyz,a_normal),dot(u_gx_normal[1].xyz,a_normal),dot(u_gx_normal[2].xyz,a_normal));"
              "float gx_nl2=dot(gx_nrm,gx_nrm);gx_nrm=gx_nl2>0.00000000000000000001?gx_nrm/sqrt(gx_nl2):float3(0.0);\n";
    }
    vs << "float4 p=u_mvp[0]*mv.x+u_mvp[1]*mv.y+u_mvp[2]*mv.z+u_mvp[3]*a_position.w;\n";
  } else {
    vs << "float4 p=";
    if (d.positionIsClipSpace) vs << "a_position;\n";
    else vs << "u_mvp[0]*a_position.x+u_mvp[1]*a_position.y+u_mvp[2]*a_position.z+u_mvp[3]*a_position.w;\n";
  }
  // Shared projection values retain Aurora's GX depth convention. With the
  // native viewport zOffset/zScale=(near+far)/2,(far-near)/2 this is the same
  // effective depth as the existing renderer, without an OpenGL call.
  vs << (d.reversedZ ? "p.z=-2.0*p.z-p.w;\n" : "p.z=2.0*p.z+p.w;\n") << "v_position=p;\n";
  if(d.fixedVertexOnGpu) {
    vs << "float3 gxq;float gxi;\n";
    for(unsigned i=0;i<2;++i)if(out.colorMask&(1u<<i)) {
      emit_fixed_channel_cg(vs,d,i,i,false);
      emit_fixed_channel_cg(vs,d,i+2,i,true);
    }
    for(unsigned i=0;i<MaxTextures;++i)if(out.texcoordMask&(1u<<i)) {
      const auto n=std::to_string(i);
      if(i>=d.texgenCount){vs<<"v_tex"<<n<<"=a_tex"<<n<<";\n";continue;}
      const auto& t=d.texgens[i];
      vs<<"{float4 src="<<fixed_vertex_source_cg(t.source)<<";float3 tc;\n";
      if(t.type==TexGenType::SRTG)vs<<"tc=float3(src.xy,1.0);\n";
      else if(t.matrix<0)vs<<"tc=src.xyz;\n";
      else vs<<"tc=float3(dot(u_gx_texture"<<n<<"[0],src),dot(u_gx_texture"<<n<<"[1],src),dot(u_gx_texture"<<n<<"[2],src));\n";
      if(t.type==TexGenType::Matrix2x4)vs<<"tc.z=1.0;\n";
      if(t.normalize)vs<<"float l=sqrt(dot(tc,tc));tc=l>0.0000000001?tc/l:float3(0.0);\n";
      if(t.postMatrix>=0)vs<<"float4 pt=float4(tc,1.0);tc=float3(dot(u_gx_post"<<n<<"[0],pt),dot(u_gx_post"<<n<<"[1],pt),dot(u_gx_post"<<n<<"[2],pt));\n";
      if(t.type!=TexGenType::Matrix3x4)vs<<"tc.z=1.0;\n";
      vs<<"v_tex"<<n<<"=tc;}\n";
    }
  } else {
    for (unsigned i = 0; i < 2; ++i) if (out.colorMask & (1u << i)) vs << "v_color" << i << "=a_color" << i << ";\n";
    for (unsigned i = 0; i < MaxTextures; ++i) if (out.texcoordMask & (1u << i)) vs << "v_tex" << i << "=a_tex" << i << ";\n";
  }
  vs << "}\n";
  fs << "float2 gx_sample_uv(float2 uv,float4 t){return uv*t.xy+t.zw;}\n"
        "float gx_wrap_coord(float v,float mode){if(mode<0.5)return clamp(v,0.0,1.0);if(mode<1.5)return frac(v);float f=frac(v*0.5)*2.0;return 1.0-abs(f-1.0);}\n"
        "float2 gx_wrap_uv(float2 uv,float2 mode){return float2(gx_wrap_coord(uv.x,mode.x),gx_wrap_coord(uv.y,mode.y));}\n"
        "float tev_wrap1(float v){float b=v*255.0;return (b-floor(b/256.0)*256.0)/255.0;}\n"
        "float3 tev_wrap3(float3 v){float3 b=v*255.0;return (b-floor(b/256.0)*256.0)/255.0;}\n"
        "float4 main(\n";
  signature(fs, fp);
  fs << ") : COLOR {\n";
  if (d.fragmentScissor)
    fs << "if(window_position.x<u_clip_rect.x||window_position.y<u_clip_rect.y||window_position.x>=u_clip_rect.z||window_position.y>=u_clip_rect.w) discard;\n";
  fs << "float4 prev=u_tevreg[0],reg0=u_tevreg[1],reg1=u_tevreg[2],reg2=u_tevreg[3];\n"
        "float2 prev_ind_uv=float2(0.0);\n";
  std::array<bool, 4> cn{}, an{};
  for (unsigned i = 0; i < d.tev.stageCount; ++i) {
    const auto& s = d.tev.stages[i];
    fs << "{\nfloat ind_alpha=0.0;\nfloat2 tev_uv="<<coordinate(d,s.texCoord)<<";\n";
    indirect(fs,d,s);
    fs << "float4 raw_tex=";
    if (tev_stage_uses_texture(s) && s.texture < MaxTextures) {
      const bool nativeWrap=(d.nativeTextureWrapMask&(1u<<s.texture))!=0;
      fs << "tex2D(u_tex" << unsigned(s.texture) << ",";
      if(!nativeWrap) fs << "gx_wrap_uv(";
      fs << "gx_sample_uv(tev_uv,u_tex_transform[" << unsigned(s.texture) << "])";
      if(!nativeWrap) fs << ",u_tex_wrap[" << unsigned(s.texture) << "].xy)";
      fs << ");\nif(u_tex_copy_mode[" << unsigned(s.texture)
         << "]>1.5){float a=raw_tex.a;raw_tex=float4(a,a,a,a);}\n"
         << "else if(u_tex_copy_mode[" << unsigned(s.texture)
         << "]>0.5){float q=min(floor(raw_tex.r*16.0)/15.0,1.0);raw_tex=float4(q,q,q,q);}\n"
         << "raw_tex.a=lerp(raw_tex.a,1.0,u_tex_force_opaque[" << unsigned(s.texture) << "]);\n";
    } else fs << "float4(1.0);\n";
    fs << "float4 texc=" << swizzle("raw_tex", d.tev.swapTable[s.texSwap]) << ";\nfloat4 raw_ras=";
    if (!tev_stage_uses_raster(s) || s.rasterSource == RasterSource::Zero) fs << "float4(0.0)";
    else if(s.rasterSource==RasterSource::AlphaBump)fs<<"float4(ind_alpha)";
    else if(s.rasterSource==RasterSource::AlphaBumpN)fs<<"float4(min(ind_alpha*(255.0/248.0),1.0))";
    else fs << (s.rasterSource == RasterSource::Color1 ? "v_color1" : "v_color0");
    fs << ";\nfloat4 rasc=" << swizzle("raw_ras", d.tev.swapTable[s.rasSwap]) << ";\n";
    const TevColorArg colors[]{s.color.a,s.color.b,s.color.c,s.color.d};
    const TevAlphaArg alphas[]{s.alpha.a,s.alpha.b,s.alpha.c,s.alpha.d};
    for (unsigned j = 0; j < 4; ++j) {
      // GX truncates A/B/C to eight bits, but D is the signed accumulator.
      // Wrapping D turns negative material offsets into bright positive ones.
      fs << "float3 c" << char('A'+j) << "=" << color_arg(colors[j], s, cn, an, j != 3) << ";\n";
      fs << "float a" << char('A'+j) << "=" << alpha_arg(alphas[j], s, an, j != 3) << ";\n";
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
  const auto predicate = alpha_test(a, b, ac.op);
  // vitaShaRK can enter an internal-error state when Cg contains an
  // unconditional kill ("if(!(false)) discard"). Preserve GX semantics by
  // marking the pipeline as side-effect-free instead; the native renderer then
  // disables color/depth writes while compiling this shader without a kill.
  if (predicate == "false") out.discardAll = true;
  else if (predicate != "true") fs << "if(!(" << predicate << ")) discard;\n";
  if (d.dstAlpha >= 0) fs << "result.a=" << d.dstAlpha << ".0/255.0;\n";
  if(d.fogMode!=FogMode::None) {
    fs<<"{float fd="<<(d.reversedZ?"window_position.z":"(1.0-window_position.z)")<<";\nfloat fb="
      <<(d.fogOrthographic?"u_fog_params.x*fd":"u_fog_params.x/max(u_fog_params.y-fd,0.000001)")<<";\n";
    if(d.fogRangeEnabled) {
      fs<<"float sx=window_position.x/max(u_render_viewport_width,1.0)*2.0-1.0;\n"
          "float fo=sx-u_fog_params.w;float ri=clamp(9.0-abs(fo)*9.0,0.0,9.0);\n"
          "float lo=floor(ri),hi=min(lo+1.0,9.0);float fk=max(lerp("
        <<fog_range("lo")<<","<<fog_range("hi")<<",ri-lo),0.000001);\nfb*=sqrt(fo*fo+fk*fk)/fk;\n";
    }
    fs<<"float f=clamp(fb-u_fog_params.z,0.0,1.0);\n";
    switch(d.fogMode) {
    case FogMode::Exp:fs<<"f=1.0-exp2(-8.0*f);\n";break;
    case FogMode::Exp2:fs<<"f=1.0-exp2(-8.0*f*f);\n";break;
    case FogMode::RevExp:fs<<"f=exp2(-8.0*(1.0-f));\n";break;
    case FogMode::RevExp2:fs<<"f=1.0-f;f=exp2(-8.0*f*f);\n";break;
    default:break;
    }
    fs<<"result.rgb=lerp(result.rgb,u_fog_color.rgb,clamp(f,0.0,1.0));}\n";
  }
  fs << "return result;\n}\n";
  out.vertex = vs.str(); out.fragment = fs.str();
  return out;
}
} // namespace aurora::vita::gxm
