#include "vita_vertex_decode.hpp"
#include "vita_cpu_workers.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace aurora::vita::gfx {
namespace {
uint16_t read16(const uint8_t* p, bool le) noexcept { return le ? static_cast<uint16_t>(p[0] | (p[1] << 8)) : static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t read32(const uint8_t* p, bool le) noexcept { return le ? (uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24) : (uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | uint32_t(p[3])); }
float readf(const uint8_t* p, bool le) noexcept { return std::bit_cast<float>(read32(p,le)); }
uint8_t ex4(uint32_t v) noexcept { return static_cast<uint8_t>((v<<4)|v); }
uint8_t ex5(uint32_t v) noexcept { return static_cast<uint8_t>((v<<3)|(v>>2)); }
uint8_t ex6(uint32_t v) noexcept { return static_cast<uint8_t>((v<<2)|(v>>4)); }
size_t component_bytes(VertexComponent c, uint8_t n) noexcept {
  switch(c){case VertexComponent::U8:case VertexComponent::S8:return n;case VertexComponent::U16:case VertexComponent::S16:return size_t(n)*2;case VertexComponent::F32:return size_t(n)*4;case VertexComponent::RGB565:case VertexComponent::RGBA4:return 2;case VertexComponent::RGB8:return 3;case VertexComponent::RGBX8:case VertexComponent::RGBA8:return 4;case VertexComponent::RGBA6:return 3;}return 0;
}
uint16_t semantic_offset(VertexSemantic semantic) noexcept {
  switch(semantic) {
  case VertexSemantic::PnMatrixIndex:return offsetof(CanonicalVertex,pnMatrixIndex);
  case VertexSemantic::TexMatrixIndex0: case VertexSemantic::TexMatrixIndex1:
  case VertexSemantic::TexMatrixIndex2: case VertexSemantic::TexMatrixIndex3:
  case VertexSemantic::TexMatrixIndex4: case VertexSemantic::TexMatrixIndex5:
  case VertexSemantic::TexMatrixIndex6: case VertexSemantic::TexMatrixIndex7:
    return static_cast<uint16_t>(offsetof(CanonicalVertex,texMatrixIndex)+
      static_cast<unsigned>(semantic)-static_cast<unsigned>(VertexSemantic::TexMatrixIndex0));
  case VertexSemantic::Position:return offsetof(CanonicalVertex,position);
  case VertexSemantic::Normal:return offsetof(CanonicalVertex,normal);
  case VertexSemantic::Binormal:return offsetof(CanonicalVertex,binormal);
  case VertexSemantic::Tangent:return offsetof(CanonicalVertex,tangent);
  case VertexSemantic::Color0:return offsetof(CanonicalVertex,color0);
  case VertexSemantic::Color1:return offsetof(CanonicalVertex,color1);
  case VertexSemantic::Tex0: case VertexSemantic::Tex1: case VertexSemantic::Tex2: case VertexSemantic::Tex3:
  case VertexSemantic::Tex4: case VertexSemantic::Tex5: case VertexSemantic::Tex6: case VertexSemantic::Tex7:
    return static_cast<uint16_t>(offsetof(CanonicalVertex,texcoord)+sizeof(float)*3u*
      (static_cast<unsigned>(semantic)-static_cast<unsigned>(VertexSemantic::Tex0)));
  }
  return 0;
}
VertexDecodeStore semantic_store(VertexSemantic semantic) noexcept {
  if(semantic==VertexSemantic::PnMatrixIndex)return VertexDecodeStore::PnMatrixIndex;
  if(semantic>=VertexSemantic::TexMatrixIndex0&&semantic<=VertexSemantic::TexMatrixIndex7)
    return VertexDecodeStore::TexMatrixIndex;
  if(semantic==VertexSemantic::Color0||semantic==VertexSemantic::Color1)return VertexDecodeStore::Color;
  return VertexDecodeStore::Float;
}
bool numeric(const uint8_t* p, size_t avail, const VertexDecodeAttribute& a, bool le, float out[4]) noexcept {
  if (a.components == 0 || a.components > 4 || avail < component_bytes(a.component, a.components)) return false;
  // GX fractional bits are small (5 bits in VAT). Constructing 2^-frac from
  // the IEEE exponent avoids a libm ldexp call for every attribute of every
  // vertex, which was a major cost on Vita's ARM CPU.
  const float scale = a.frac <= 126
      ? std::bit_cast<float>(static_cast<uint32_t>(127u - a.frac) << 23u)
      : std::ldexp(1.0f, -static_cast<int>(a.frac));
  for (unsigned i = 0; i < a.components; i++) {
    switch (a.component) {
    case VertexComponent::U8: out[i] = p[i] * scale; break;
    case VertexComponent::S8: out[i] = static_cast<int8_t>(p[i]) * scale; break;
    case VertexComponent::U16: out[i] = read16(p + i * 2, le) * scale; break;
    case VertexComponent::S16: out[i] = static_cast<int16_t>(read16(p + i * 2, le)) * scale; break;
    case VertexComponent::F32: out[i] = readf(p + i * 4, le); break;
    default: return false;
    }
  }
  return true;
}
bool color(const uint8_t*p,size_t avail,VertexComponent c,bool le,uint8_t out[4]) noexcept {
  out[0]=out[1]=out[2]=255;out[3]=255;switch(c){case VertexComponent::RGB565:{if(avail<2)return false;auto v=read16(p,le);out[0]=ex5((v>>11)&31);out[1]=ex6((v>>5)&63);out[2]=ex5(v&31);return true;}case VertexComponent::RGB8:if(avail<3)return false;std::copy_n(p,3,out);return true;case VertexComponent::RGBX8:if(avail<4)return false;std::copy_n(p,3,out);return true;case VertexComponent::RGBA4:{if(avail<2)return false;auto v=read16(p,le);out[0]=ex4((v>>12)&15);out[1]=ex4((v>>8)&15);out[2]=ex4((v>>4)&15);out[3]=ex4(v&15);return true;}case VertexComponent::RGBA6:{if(avail<3)return false;uint32_t v=le?(uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16):(uint32_t(p[0])<<16|uint32_t(p[1])<<8|p[2]);out[0]=ex6((v>>18)&63);out[1]=ex6((v>>12)&63);out[2]=ex6((v>>6)&63);out[3]=ex6(v&63);return true;}case VertexComponent::RGBA8:if(avail<4)return false;std::copy_n(p,4,out);return true;default:return false;}
}
bool resolve(const uint8_t* stream, size_t streamSize, size_t base, const VertexDecodeAttribute& a, bool streamLe,
             const uint8_t*& p, size_t& avail, bool& le) noexcept {
  if (a.source == VertexSource::None) return false;
  if (base + a.streamOffset >= streamSize) return false;
  const uint8_t* s = stream + base + a.streamOffset;
  if (a.source == VertexSource::Direct) {
    if (base + a.streamOffset + a.valueOffset >= streamSize) return false;
    p = s + a.valueOffset;
    avail = streamSize - (base + a.streamOffset + a.valueOffset);
    le = streamLe;
    return true;
  }
  const size_t indexBytes = a.source == VertexSource::Index8 ? 1 : 2;
  if (streamSize - (base + a.streamOffset) < indexBytes) return false;
  const size_t index = a.source == VertexSource::Index8 ? s[0] : read16(s, streamLe);
  if (!a.array.data || !a.array.stride) return false;
  const size_t off = index * a.array.stride;
  if (off >= a.array.size) return false;
  if (off + a.valueOffset >= a.array.size) return false;
  p = a.array.data + off + a.valueOffset;
  avail = a.array.size - off - a.valueOffset;
  le = a.array.littleEndian;
  return true;
}

bool decode_vertex(const uint8_t* stream, size_t streamSize, uint32_t vi,
                   const VertexDecodeLayout& layout, CanonicalVertex& v,
                   VertexSemanticMask requiredSemantics) noexcept {
  const size_t base = size_t(vi) * layout.streamStride;
  for (unsigned ai = 0; ai < layout.count; ++ai) {
    const auto& a = layout.attributes[ai];
    if (a.source == VertexSource::None) continue;
    if ((requiredSemantics & vertex_semantic_bit(a.semantic)) == 0) continue;
    const uint8_t* p = nullptr;
    size_t avail = 0;
    bool le = false;
    if (!resolve(stream, streamSize, base, a, layout.streamLittleEndian, p, avail, le)) return false;
    if (a.semantic == VertexSemantic::Color0 || a.semantic == VertexSemantic::Color1) {
      uint8_t* c = a.semantic == VertexSemantic::Color0 ? v.color0 : v.color1;
      if (!color(p, avail, a.component, le, c)) return false;
      continue;
    }
    float tmp[4]{};
    if (!numeric(p, avail, a, le, tmp)) return false;
    switch (a.semantic) {
    case VertexSemantic::PnMatrixIndex:
      v.pnMatrixIndex = static_cast<uint8_t>(tmp[0] / 3.f);
      break;
    case VertexSemantic::TexMatrixIndex0: case VertexSemantic::TexMatrixIndex1:
    case VertexSemantic::TexMatrixIndex2: case VertexSemantic::TexMatrixIndex3:
    case VertexSemantic::TexMatrixIndex4: case VertexSemantic::TexMatrixIndex5:
    case VertexSemantic::TexMatrixIndex6: case VertexSemantic::TexMatrixIndex7: {
      const unsigned mi = static_cast<unsigned>(a.semantic) - static_cast<unsigned>(VertexSemantic::TexMatrixIndex0);
      v.texMatrixIndex[mi] = static_cast<uint8_t>(tmp[0]);
      break;
    }
    case VertexSemantic::Position:
      for (unsigned j = 0; j < std::min<unsigned>(3, a.components); ++j) v.position[j] = tmp[j];
      break;
    case VertexSemantic::Normal:
      for (unsigned j = 0; j < std::min<unsigned>(3, a.components); ++j) v.normal[j] = tmp[j];
      break;
    case VertexSemantic::Binormal:
      for (unsigned j = 0; j < std::min<unsigned>(3, a.components); ++j) v.binormal[j] = tmp[j];
      break;
    case VertexSemantic::Tangent:
      for (unsigned j = 0; j < std::min<unsigned>(3, a.components); ++j) v.tangent[j] = tmp[j];
      break;
    case VertexSemantic::Tex0: case VertexSemantic::Tex1: case VertexSemantic::Tex2: case VertexSemantic::Tex3:
    case VertexSemantic::Tex4: case VertexSemantic::Tex5: case VertexSemantic::Tex6: case VertexSemantic::Tex7: {
      const unsigned ti = static_cast<unsigned>(a.semantic) - static_cast<unsigned>(VertexSemantic::Tex0);
      for (unsigned j = 0; j < std::min<unsigned>(3, a.components); ++j) v.texcoord[ti][j] = tmp[j];
      if (a.components < 3) v.texcoord[ti][2] = 1.f;
      break;
    }
    default:
      break;
    }
  }
  return true;
}

bool resolve_op(const uint8_t* stream,size_t streamSize,size_t base,const VertexDecodeOp& op,bool streamLe,
                const uint8_t*& p,size_t& avail,bool& le) noexcept {
  if(op.source==VertexSource::None||base+op.streamOffset>=streamSize)return false;
  const uint8_t* s=stream+base+op.streamOffset;
  if(op.source==VertexSource::Direct) {
    if(base+op.streamOffset+op.valueOffset>=streamSize)return false;
    p=s+op.valueOffset;avail=streamSize-(base+op.streamOffset+op.valueOffset);le=streamLe;return true;
  }
  if(streamSize-(base+op.streamOffset)<op.indexBytes)return false;
  const size_t index=op.source==VertexSource::Index8?s[0]:read16(s,streamLe);
  if(!op.array.data||!op.array.stride)return false;
  const size_t off=index*op.array.stride;
  if(off>=op.array.size||off+op.valueOffset>=op.array.size)return false;
  p=op.array.data+off+op.valueOffset;avail=op.array.size-off-op.valueOffset;le=op.array.littleEndian;return true;
}

bool decode_vertex_ops(const uint8_t* stream,size_t streamSize,uint32_t vi,const VertexDecodeLayout& layout,
                       CanonicalVertex& v,VertexSemanticMask requiredSemantics) noexcept {
  const size_t base=size_t(vi)*layout.streamStride;
  auto* dstBase=reinterpret_cast<uint8_t*>(&v);
  for(unsigned oi=0;oi<layout.opCount;++oi) {
    const auto& op=layout.ops[oi];
    if((requiredSemantics&vertex_semantic_bit(op.semantic))==0)continue;
    const uint8_t* p=nullptr;size_t avail=0;bool le=false;
    if(!resolve_op(stream,streamSize,base,op,layout.streamLittleEndian,p,avail,le))return false;
    if(op.store==VertexDecodeStore::Color) {
      if(!color(p,avail,op.component,le,dstBase+op.dstOffset))return false;
      continue;
    }
    if(avail<op.valueBytes)return false;
    float tmp[4]{};
    for(unsigned i=0;i<op.components;++i) {
      switch(op.component) {
      case VertexComponent::U8:tmp[i]=p[i]*op.scale;break;
      case VertexComponent::S8:tmp[i]=static_cast<int8_t>(p[i])*op.scale;break;
      case VertexComponent::U16:tmp[i]=read16(p+i*2,le)*op.scale;break;
      case VertexComponent::S16:tmp[i]=static_cast<int16_t>(read16(p+i*2,le))*op.scale;break;
      case VertexComponent::F32:tmp[i]=readf(p+i*4,le);break;
      default:return false;
      }
    }
    if(op.store==VertexDecodeStore::PnMatrixIndex) {
      dstBase[op.dstOffset]=static_cast<uint8_t>(tmp[0]/3.f);
    } else if(op.store==VertexDecodeStore::TexMatrixIndex) {
      dstBase[op.dstOffset]=static_cast<uint8_t>(tmp[0]);
    } else {
      auto* dst=reinterpret_cast<float*>(dstBase+op.dstOffset);
      for(unsigned i=0;i<op.components;++i)dst[i]=tmp[i];
      if(op.fillThirdOne&&op.components<3)dst[2]=1.f;
    }
  }
  return true;
}

struct DecodeContext {
  const uint8_t* stream = nullptr;
  size_t streamSize = 0;
  const VertexDecodeLayout* layout = nullptr;
  CanonicalVertex* vertices = nullptr;
  std::array<size_t, 3> badVertex{{
      std::numeric_limits<size_t>::max(),
      std::numeric_limits<size_t>::max(),
      std::numeric_limits<size_t>::max()}};
};

bool decode_range(void* opaque, size_t begin, size_t end, uint32_t lane) noexcept {
  auto& ctx = *static_cast<DecodeContext*>(opaque);
  if (lane >= ctx.badVertex.size()) return false;
  for (size_t vi = begin; vi < end; ++vi) {
    if (!decode_vertex(ctx.stream, ctx.streamSize, static_cast<uint32_t>(vi), *ctx.layout, ctx.vertices[vi], AllVertexSemantics)) {
      ctx.badVertex[lane] = vi;
      return false;
    }
  }
  return true;
}

uint64_t hash_vertex_record(const uint8_t* data,size_t bytes) noexcept {
  uint64_t h=1469598103934665603ull;
  for(size_t i=0;i<bytes;++i){h^=data[i];h*=1099511628211ull;}
  return h;
}
}
VertexLayout canonical_vertex_layout() noexcept {
  VertexLayout l{};l.count=11;
  l.attributes[0]={0,4,VertexScalar::F32,false,sizeof(CanonicalVertex),offsetof(CanonicalVertex,position)};
  l.attributes[1]={1,4,VertexScalar::U8,true,sizeof(CanonicalVertex),offsetof(CanonicalVertex,color0)};
  l.attributes[2]={2,4,VertexScalar::U8,true,sizeof(CanonicalVertex),offsetof(CanonicalVertex,color1)};
  for(unsigned i=0;i<8;i++)l.attributes[3+i]={static_cast<uint8_t>(3+i),3,VertexScalar::F32,false,sizeof(CanonicalVertex),static_cast<uint16_t>(offsetof(CanonicalVertex,texcoord)+sizeof(float)*3*i)};
  return l;
}
void compile_vertex_decode_layout(VertexDecodeLayout& layout) noexcept {
  layout.opCount=0;
  for(unsigned i=0;i<layout.count&&layout.opCount<layout.ops.size();++i) {
    const auto& a=layout.attributes[i];
    if(a.source==VertexSource::None)continue;
    auto& op=layout.ops[layout.opCount++];
    op.source=a.source;op.component=a.component;op.store=semantic_store(a.semantic);op.semantic=a.semantic;
    op.components=a.components;op.valueBytes=static_cast<uint8_t>(component_bytes(a.component,a.components));
    op.indexBytes=a.source==VertexSource::Index16?2u:(a.source==VertexSource::Index8?1u:0u);
    op.fillThirdOne=a.semantic>=VertexSemantic::Tex0&&a.semantic<=VertexSemantic::Tex7;
    op.streamOffset=a.streamOffset;op.valueOffset=a.valueOffset;op.dstOffset=semantic_offset(a.semantic);
    op.scale=a.frac<=126?std::bit_cast<float>(static_cast<uint32_t>(127u-a.frac)<<23u):
                            std::ldexp(1.f,-static_cast<int>(a.frac));
    op.array=a.array;
  }
}
VertexLayout gpu_vertex_layout(uint8_t texcoordMask,uint8_t colorMask) noexcept {
  VertexLayout l{};
  uint16_t stride=16;
  if(colorMask&1u)stride+=4;
  if(colorMask&2u)stride+=4;
  for(unsigned i=0;i<8;i++)if(texcoordMask&(1u<<i))stride+=12;
  l.attributes[l.count++]={0,4,VertexScalar::F32,false,stride,0};
  uint16_t offset=16;
  if(colorMask&1u){l.attributes[l.count++]={1,4,VertexScalar::U8,true,stride,offset};offset+=4;}
  if(colorMask&2u){l.attributes[l.count++]={2,4,VertexScalar::U8,true,stride,offset};offset+=4;}
  for(unsigned i=0;i<8;i++)if(texcoordMask&(1u<<i)){
    l.attributes[l.count++]={static_cast<uint8_t>(3+i),3,VertexScalar::F32,false,stride,offset};
    offset+=12;
  }
  return l;
}
bool deduplicate_vertex_records(const uint8_t* stream,size_t streamSize,uint32_t vertexCount,uint16_t streamStride,
                                std::vector<uint8_t>& compact,std::vector<uint16_t>& remap,
                                std::vector<uint32_t>& table) noexcept {
  compact.clear();remap.clear();table.clear();
  if(!stream||!vertexCount||!streamStride)return false;
  if(static_cast<size_t>(vertexCount)>streamSize/static_cast<size_t>(streamStride))return false;
  if(static_cast<size_t>(vertexCount)>std::numeric_limits<size_t>::max()/2u)return false;

  size_t tableSize=8;
  const size_t target=static_cast<size_t>(vertexCount)*2u;
  while(tableSize<target){
    if(tableSize>std::numeric_limits<size_t>::max()/2u)return false;
    tableSize*=2u;
  }
  table.assign(tableSize,0u);
  remap.resize(vertexCount);
  compact.reserve(static_cast<size_t>(vertexCount)*streamStride);
  const size_t mask=tableSize-1u;

  for(uint32_t vi=0;vi<vertexCount;++vi){
    const uint8_t* record=stream+static_cast<size_t>(vi)*streamStride;
    size_t slot=static_cast<size_t>(hash_vertex_record(record,streamStride))&mask;
    for(;;){
      const uint32_t entry=table[slot];
      if(!entry){
        const size_t unique=compact.size()/streamStride;
        if(unique>std::numeric_limits<uint16_t>::max())return false;
        const size_t old=compact.size();
        compact.resize(old+streamStride);
        std::memcpy(compact.data()+old,record,streamStride);
        table[slot]=static_cast<uint32_t>(unique+1u);
        remap[vi]=static_cast<uint16_t>(unique);
        break;
      }
      const size_t unique=static_cast<size_t>(entry-1u);
      if(std::memcmp(compact.data()+unique*streamStride,record,streamStride)==0){
        remap[vi]=static_cast<uint16_t>(unique);
        break;
      }
      slot=(slot+1u)&mask;
    }
  }
  return true;
}
bool decode_vertex_into(const uint8_t* stream, size_t streamSize, uint32_t vertexIndex,
                        const VertexDecodeLayout& layout, CanonicalVertex& vertex,
                        VertexSemanticMask requiredSemantics) noexcept {
  if (!stream || !layout.streamStride || layout.count > layout.attributes.size()) return false;
  if (size_t(vertexIndex) * layout.streamStride >= streamSize) return false;
  return layout.opCount?decode_vertex_ops(stream,streamSize,vertexIndex,layout,vertex,requiredSemantics):
                        decode_vertex(stream, streamSize, vertexIndex, layout, vertex, requiredSemantics);
}
VertexDecodeResult decode_vertices(const uint8_t* stream, size_t streamSize, uint32_t vertexCount,
                                   const VertexDecodeLayout& layout) noexcept {
  VertexDecodeResult r{};
  if (!stream || !layout.streamStride || layout.count > layout.attributes.size()) return r;
  if (size_t(vertexCount) * layout.streamStride > streamSize) return r;

  r.vertices.resize(vertexCount);
  DecodeContext ctx{stream, streamSize, &layout, r.vertices.data()};
  if (!cpu_parallel_for(vertexCount, decode_range, &ctx)) {
    size_t bad = std::numeric_limits<size_t>::max();
    for (const size_t candidate : ctx.badVertex) bad = std::min(bad, candidate);
    r.badVertex = bad == std::numeric_limits<size_t>::max() ? 0 : bad;
    return r;
  }
  r.ok = true;
  return r;
}
} // namespace aurora::vita::gfx
