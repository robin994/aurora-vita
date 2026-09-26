#include "vita_pipeline_key.hpp"
#include "vita_hash.hpp"
#include <cstring>
#include <type_traits>
namespace aurora::vita::gfx {
namespace {
// Keys are built by serializing the same fields, in the same order, into a
// bounded byte buffer and hashing it once with two 32-bit lanes. The previous
// per-field 64-bit FNV + combine cost ~600 64-bit multiplies per pipeline key,
// which is slow on the 32-bit Cortex-A9 and ran for almost every GX draw.
struct KeyWriter {
  alignas(4) uint8_t bytes[2048];
  size_t size=0;
  bool overflow=false;
};
template<class T> inline void add(KeyWriter& w,const T& v) noexcept {
  static_assert(std::is_trivially_copyable_v<T>);
  if(w.size+sizeof(T)>sizeof(w.bytes)){w.overflow=true;return;}
  std::memcpy(w.bytes+w.size,&v,sizeof(T));w.size+=sizeof(T);
}
inline uint32_t rotl32(uint32_t v,unsigned r) noexcept {return (v<<r)|(v>>(32u-r));}
uint64_t finish(const KeyWriter& w) noexcept {
  uint32_t a=0x9e3779b9u^static_cast<uint32_t>(w.size),b=0x85ebca6bu+static_cast<uint32_t>(w.size);
  size_t i=0;
  for(;i+4<=w.size;i+=4){
    uint32_t v;std::memcpy(&v,w.bytes+i,4);
    a=rotl32(a^v,13)*0x9e3779b1u;
    b=rotl32(b+v,17)*0xc2b2ae35u;
  }
  uint32_t tail=0;
  for(unsigned s=0;i<w.size;++i,s+=8)tail|=uint32_t(w.bytes[i])<<s;
  a=rotl32(a^tail,13)*0x9e3779b1u;b=rotl32(b+tail,17)*0xc2b2ae35u;
  a^=a>>16;a*=0x85ebca6bu;a^=a>>13;
  b^=b>>15;b*=0x27d4eb2fu;b^=b>>16;
  const uint64_t h=(uint64_t(a)<<32)|b;
  // An overflow cannot happen with the bounded PipelineDesc arrays; if it ever
  // did, fall back to the slow full-object FNV so distinct keys stay distinct.
  return w.overflow?hash_combine(h,fnv1a64(w.bytes,w.size)):h;
}
}
uint64_t pipeline_key(const PipelineDesc& d) noexcept {
  KeyWriter h;
  add(h,d.primitive);add(h,d.depthFunc);add(h,d.cull);add(h,d.blendMode);add(h,d.srcFactor);add(h,d.dstFactor);add(h,d.logicOp);
  add(h,d.depthTest);add(h,d.depthWrite);add(h,d.colorWrite);add(h,d.alphaWrite);add(h,d.reversedZ);add(h,d.polygonOffset);add(h,d.polygonOffsetFactor);add(h,d.polygonOffsetUnits);add(h,d.dstAlpha);add(h,d.fogMode);add(h,d.fogOrthographic);add(h,d.fogRangeEnabled);add(h,d.positionIsClipSpace);
  add(h,d.layout.count);for(unsigned i=0;i<d.layout.count&&i<MaxVertexAttributes;i++){const auto&a=d.layout.attributes[i];add(h,a.location);add(h,a.components);add(h,a.scalar);add(h,a.normalized);add(h,a.stride);add(h,a.offset);}
  add(h,d.texgenCount);for(unsigned i=0;i<d.texgenCount&&i<MaxTextures;i++){const auto&t=d.texgens[i];add(h,t.type);add(h,t.source);add(h,t.matrix);add(h,t.postMatrix);add(h,t.embossSource);add(h,t.normalize);add(h,t.matrixFromVertex);}
  add(h,d.fixedVertexOnGpu);add(h,d.fixedVertexIndexedPn);add(h,d.fixedVertexTexMtxMask);
  add(h,d.fixedPointSprite);add(h,d.fixedLineSprite);add(h,d.fixedPrimitiveTexcoordMask);
  add(h,d.fragmentScissor);
  add(h,d.nativeTextureWrapMask);
  add(h,d.textureForceOpaqueMask);add(h,d.textureCopyModeBits);
  for(const auto&c:d.colorChannels){add(h,c.materialSource);add(h,c.ambientSource);add(h,c.diffuse);add(h,c.attenuation);add(h,c.lightingEnabled);add(h,c.lightMask);}
  add(h,d.tev.stageCount);add(h,d.tev.texCoordCount);add(h,d.tev.rasterColorCount);add(h,d.tev.indirectStageCount);
  for(const auto&s:d.tev.swapTable){add(h,s.r);add(h,s.g);add(h,s.b);add(h,s.a);}
  for(unsigned i=0;i<d.tev.indirectStageCount&&i<MaxIndStages;i++){const auto&s=d.tev.indirectStages[i];add(h,s.texCoord);add(h,s.texture);add(h,s.scaleSShift);add(h,s.scaleTShift);}
  add(h,d.tev.alphaCompare.comp0);add(h,d.tev.alphaCompare.ref0);add(h,d.tev.alphaCompare.op);add(h,d.tev.alphaCompare.comp1);add(h,d.tev.alphaCompare.ref1);
  for(unsigned i=0;i<d.tev.stageCount&&i<MaxTevStages;i++){
    const auto&s=d.tev.stages[i];
    add(h,s.color.a);add(h,s.color.b);add(h,s.color.c);add(h,s.color.d);add(h,s.alpha.a);add(h,s.alpha.b);add(h,s.alpha.c);add(h,s.alpha.d);
    add(h,s.colorOp);add(h,s.alphaOp);add(h,s.colorBias);add(h,s.alphaBias);add(h,s.colorScale);add(h,s.alphaScale);add(h,s.colorOut);add(h,s.alphaOut);add(h,s.konstColor);add(h,s.konstAlpha);add(h,s.texture);add(h,s.texCoord);add(h,s.rasterSource);add(h,s.rasSwap);add(h,s.texSwap);add(h,s.colorClamp);add(h,s.alphaClamp);add(h,s.indirectEnabled);add(h,s.indirectStage);add(h,s.indirectFormat);add(h,s.indirectBias);add(h,s.indirectAlpha);add(h,s.indirectMatrix);add(h,s.indirectWrapS);add(h,s.indirectWrapT);add(h,s.indirectUseOrigLod);add(h,s.indirectAddPrev);
  }
  return finish(h);
}
uint64_t texture_key(const TextureDesc& d) noexcept {
  KeyWriter h;add(h,d.sourceId);add(h,d.paletteSourceId);add(h,d.revision);add(h,d.paletteRevision);add(h,d.width);add(h,d.height);add(h,d.format);add(h,d.paletteFormat);add(h,d.mipCount);add(h,d.cacheable);add(h,d.generateMipmaps);
  if(!d.sourceId){const auto p=reinterpret_cast<uintptr_t>(d.data);add(h,p);}return finish(h);
}
} // namespace aurora::vita::gfx
