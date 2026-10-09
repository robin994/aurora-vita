#include "vita_native_assets.hpp"
#include <xxhash.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <cstdio>
#include <limits>
namespace aurora::vita::gfx {
static_assert(std::endian::native == std::endian::little,
              "AVNR canonical float records require a little-endian host");
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
namespace {
constexpr uint32_t Magic=0x524e5641,Version=1,VersionGpu=2,KindGpu=3,Limit=16*1024*1024;
NativeAssetReader sReader=nullptr;void* sContext=nullptr;NativeAssetStats sStats{};
void word(std::vector<uint8_t>& b,uint32_t v){for(unsigned i=0;i<4;++i)b.push_back(uint8_t(v>>(8*i)));}
void span(std::vector<uint8_t>& b,const void* p,size_t n){if(n){const auto* d=static_cast<const uint8_t*>(p);b.insert(b.end(),d,d+n);}}
struct Input {
  const std::vector<uint8_t>& b;size_t pos=0;bool ok=true;
  uint32_t word(){if(pos>b.size()||b.size()-pos<4){ok=false;return 0;}uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(b[pos++])<<(8*i);return v;}
  const uint8_t* span(size_t n){if(pos>b.size()||n>b.size()-pos){ok=false;return nullptr;}const auto* p=b.data()+pos;pos+=n;return p;}
};
bool texture_input(const std::vector<uint8_t>& req,TextureDesc& d) {
  Input in{req};if(in.word()!=1)return false;
  d.width=in.word();d.height=in.word();const auto fmt=in.word(),palette=in.word();
  const auto levels=in.word(),generate=in.word();const size_t bytes=in.word(),palBytes=in.word();
  if(fmt>unsigned(TextureFormat::RGBA8888)||palette>unsigned(PaletteFormat::RGB5A3)||
      !levels||levels>13||generate||!bytes||bytes>Limit||palBytes>65536)return false;
  d.format=TextureFormat(fmt);d.paletteFormat=PaletteFormat(palette);d.mipCount=levels;
  d.data=in.span(bytes);d.dataSize=bytes;d.palette=in.span(palBytes);d.paletteSize=palBytes;
  d.cacheable=false;d.immutableSource=true;
  return in.ok&&in.pos==req.size();
}
bool geometry_input(const std::vector<uint8_t>& req,PreparedDraw& out,
                    VertexSemanticMask* available=nullptr) {
  Input in{req};if(in.word()!=2)return false;
  const auto count=in.word(),primitive=in.word(),stride=in.word(),le=in.word(),attrs=in.word(),rawBytes=in.word();
  if(!count||count>65535||primitive>unsigned(SourcePrimitive::TriangleFan)||!stride||stride>4096||
      attrs>24||le>1||rawBytes>Limit||uint64_t(count)*stride!=rawBytes)return false;
  VertexDecodeLayout layout{};layout.count=attrs;layout.streamStride=stride;layout.streamLittleEndian=le;
  VertexSemanticMask semantics=0;
  for(unsigned i=0;i<attrs;++i) {
    auto& a=layout.attributes[i];const auto sem=in.word(),source=in.word(),component=in.word(),components=in.word(),frac=in.word();
    const auto offset=in.word(),valueOffset=in.word(),arrayStride=in.word(),arrayLe=in.word(),arrayBytes=in.word();
    if(sem>unsigned(VertexSemantic::Tex7)||source>unsigned(VertexSource::Index16)||
        component>unsigned(VertexComponent::RGBA8)||components>4||frac>31||offset>65535||valueOffset>65535||
        arrayStride>65535||arrayLe>1||arrayBytes>Limit)return false;
    a.semantic=VertexSemantic(sem);a.source=VertexSource(source);a.component=VertexComponent(component);
    a.components=components;a.frac=frac;a.streamOffset=offset;a.valueOffset=valueOffset;
    a.array.stride=arrayStride;a.array.littleEndian=arrayLe;a.array.size=arrayBytes;a.array.data=in.span(arrayBytes);
    if(a.source!=VertexSource::None)semantics|=vertex_semantic_bit(a.semantic);
  }
  const auto* raw=in.span(rawBytes);if(!in.ok||in.pos!=req.size())return false;
  compile_vertex_decode_layout(layout);
  PipelineDesc pipeline{};pipeline.fixedVertexOnGpu=true;
  auto recipe=build_draw_recipe(pipeline);recipe.decodeSemantics=AllVertexSemantics;
  const bool ok=prepare_draw_into(out,raw,rawBytes,count,SourcePrimitive(primitive),layout,pipeline,{},nullptr,{},nullptr,true,&recipe);
  if(ok&&available)*available=semantics;
  return ok;
}
std::vector<uint8_t> record_payload(const std::vector<uint8_t>& req,uint32_t type) {
  std::vector<uint8_t> record;if(!sReader||req.empty())return record;
  const auto path=type==KindGpu?native_gpu_geometry_path(req):native_asset_path(req);
  if(!sReader(path.c_str(),record,sContext)){++sStats.misses;return {};}
  Input in{record};const auto magic=in.word(),version=in.word(),kind=in.word(),sourceBytes=in.word(),payloadBytes=in.word();
  const uint32_t checksumLow=in.word(),checksumHigh=in.word();
  const uint64_t checksum=uint64_t(checksumLow)|(uint64_t(checksumHigh)<<32);
  const auto* source=in.span(sourceBytes);const auto* payload=in.span(payloadBytes);
  if(!in.ok||in.pos!=record.size()||magic!=Magic||version!=uint32_t(type==KindGpu?VersionGpu:Version)||kind!=type||sourceBytes!=req.size()||
      !payloadBytes||payloadBytes>Limit||std::memcmp(source,req.data(),sourceBytes)||
      XXH64(payload,payloadBytes,0)!=checksum) {++sStats.rejected;return {};}
  return {payload,payload+payloadBytes};
}
// AVNR v2 layout schema uses six little-endian uint32 fields per attribute.
// Do not serialize C++ padding/enum size, host addresses or GPU handles.
void layout_words(std::vector<uint8_t>& result,const VertexLayout& layout) {
  word(result,layout.count);
  for(unsigned i=0;i<layout.count;++i){
    const auto& a=layout.attributes[i];
    for(uint32_t v:{uint32_t(a.location),uint32_t(a.components),uint32_t(a.scalar),
                    uint32_t(a.normalized),uint32_t(a.stride),uint32_t(a.offset)})word(result,v);
  }
}
bool read_layout(Input& in,VertexLayout& layout) {
  const unsigned count=in.word();
  if(!count||count>MaxVertexAttributes)return false;
  layout={};layout.count=static_cast<uint8_t>(count);
  for(unsigned i=0;i<count;++i) {
    const unsigned location=in.word(),components=in.word(),scalar=in.word(),normalized=in.word();
    const unsigned stride=in.word(),offset=in.word();
    if(location>14||!components||components>4||scalar>unsigned(VertexScalar::U16)||
       normalized>1||!stride||stride>512||offset>stride||
       components*(scalar==unsigned(VertexScalar::F32)?4u:1u)>stride-offset)return false;
    layout.attributes[i]={uint8_t(location),uint8_t(components),VertexScalar(scalar),
                          normalized!=0,uint16_t(stride),uint16_t(offset)};
  }
  return in.ok;
}
bool same_gpu_layout(const VertexLayout& a,const VertexLayout& b) {
  if(a.count!=b.count)return false;
  for(unsigned i=0;i<a.count;++i) {
    const auto& x=a.attributes[i];const auto& y=b.attributes[i];
    if(x.location!=y.location||x.components!=y.components||x.scalar!=y.scalar||
       x.normalized!=y.normalized||x.stride!=y.stride||x.offset!=y.offset)return false;
  }
  return true;
}
VertexLayout gpu_profile(unsigned textures,bool color,bool normal,bool indexedPn) {
  VertexLayout result{};
  uint16_t stride=0;
  const auto add=[&](uint8_t location,uint8_t components,VertexScalar scalar,bool normalized) {
    auto& a=result.attributes[result.count++];
    a={location,components,scalar,normalized,0,stride};
    stride=static_cast<uint16_t>(stride+components*(scalar==VertexScalar::F32?4u:1u));
  };
  add(0,4,VertexScalar::F32,false);
  if(color)add(1,4,VertexScalar::U8,true);
  for(unsigned i=0;i<textures;++i)add(static_cast<uint8_t>(3+i),3,VertexScalar::F32,false);
  if(normal)add(11,3,VertexScalar::F32,false);
  if(indexedPn)add(14,1,VertexScalar::U8,false);
  stride=static_cast<uint16_t>((stride+3u)&~3u);
  for(unsigned i=0;i<result.count;++i)result.attributes[i].stride=stride;
  return result;
}
}
void configure_native_assets(NativeAssetReader reader,void* context) noexcept {sReader=reader;sContext=context;sStats={};}
NativeAssetStats native_asset_stats() noexcept {return sStats;}
std::vector<uint8_t> native_texture_request(const TextureDesc& d) {
  if(!d.data||!d.dataSize||d.dataSize>Limit||d.paletteSize>65536||(!d.palette&&d.paletteSize))return {};
  std::vector<uint8_t> req;req.reserve(32+d.dataSize+d.paletteSize);
  for(uint32_t v:{1u,d.width,d.height,unsigned(d.format),unsigned(d.paletteFormat),unsigned(d.mipCount),unsigned(d.generateMipmaps),unsigned(d.dataSize),unsigned(d.paletteSize)})word(req,v);
  span(req,d.data,d.dataSize);span(req,d.palette,d.paletteSize);return req;
}
std::vector<uint8_t> native_geometry_request(const uint8_t* raw,size_t bytes,uint32_t count,
                                            SourcePrimitive primitive,const VertexDecodeLayout& layout) {
  if(!raw||!bytes||bytes>Limit||layout.count>24)return {};
  size_t total=28+bytes+size_t(layout.count)*40;
  for(unsigned i=0;i<layout.count;++i){const auto& a=layout.attributes[i];
    if(a.array.size>Limit||total>Limit-a.array.size||(!a.array.data&&a.array.size))return {};total+=a.array.size;}
  std::vector<uint8_t> req;req.reserve(total);
  for(uint32_t v:{2u,count,unsigned(primitive),unsigned(layout.streamStride),unsigned(layout.streamLittleEndian),unsigned(layout.count),unsigned(bytes)})word(req,v);
  for(unsigned i=0;i<layout.count;++i) {const auto& a=layout.attributes[i];
    for(uint32_t v:{unsigned(a.semantic),unsigned(a.source),unsigned(a.component),unsigned(a.components),unsigned(a.frac),unsigned(a.streamOffset),unsigned(a.valueOffset),unsigned(a.array.stride),unsigned(a.array.littleEndian),unsigned(a.array.size)})word(req,v);
    span(req,a.array.data,a.array.size);
  }
  span(req,raw,bytes);return req;
}
std::string native_asset_path(const std::vector<uint8_t>& req) {
  if(req.empty())return {};
  char path[96];std::snprintf(path,sizeof path,"native/v1/%s/%016llx.avnr",req[0]==1?"textures":"geometry",
      (unsigned long long)XXH64(req.data(),req.size(),0));return path;
}
std::string native_gpu_geometry_path(const std::vector<uint8_t>& req) {
  if(req.size()<4||req[0]!=2)return {};
  char path[96];std::snprintf(path,sizeof path,"native/v2/geometry/%016llx.avng",
      (unsigned long long)XXH64(req.data(),req.size(),0));return path;
}
bool read_native_blob(const char* path,std::vector<uint8_t>& bytes,size_t limit) noexcept {
  bytes.clear();
  if(!sReader||!path||!sReader(path,bytes,sContext))return false;
  if(bytes.empty()||bytes.size()>limit){bytes.clear();return false;}
  return true;
}
bool compile_native_asset(const std::vector<uint8_t>& req,std::vector<uint8_t>& record,bool includeRgba) {
  record.clear();if(req.empty()||req.size()>Limit)return false;
  std::vector<uint8_t> payload;const uint32_t type=req[0];
  if(type==1) {
    TextureDesc desc;if(!texture_input(req,desc))return false;
    auto out=gxm::prepare_texture_upload(desc,true,true);
    if(!out.ok()||(!includeRgba&&out.format==NativeTextureFormat::Rgba8))return false;
    for(uint32_t v:{unsigned(out.format),out.stride,out.mipCount,unsigned(out.swizzled)})word(payload,v);
    span(payload,out.pixels.data(),out.pixels.size());
  } else if(type==2) {
    PreparedDraw out;if(!geometry_input(req,out))return false;
    word(payload,out.vertices.size());word(payload,out.indices.size());
    // Canonical wire ABI: 13 floats, 8 color bytes, 24 texcoord floats,
    // 9 matrix selectors, 3 zero padding bytes. No host pointer or enum ABI.
    static_assert(sizeof(CanonicalVertex)==168);
    for(const auto& v:out.vertices){
      span(payload,v.position,16);span(payload,v.normal,12);span(payload,v.binormal,12);span(payload,v.tangent,12);
      span(payload,v.color0,4);span(payload,v.color1,4);span(payload,v.texcoord,96);
      span(payload,&v.pnMatrixIndex,1);span(payload,v.texMatrixIndex,8);payload.insert(payload.end(),3,0);
    }
    for(uint16_t i:out.indices){payload.push_back(i&255);payload.push_back(i>>8);}
  } else return false;
  if(payload.empty()||payload.size()>Limit)return false;
  const auto checksum=XXH64(payload.data(),payload.size(),0);
  for(uint32_t v:{Magic,Version,type,unsigned(req.size()),unsigned(payload.size()),uint32_t(checksum),uint32_t(checksum>>32)})word(record,v);
  span(record,req.data(),req.size());span(record,payload.data(),payload.size());return true;
}
bool compile_native_gpu_geometry(const std::vector<uint8_t>& req,std::vector<uint8_t>& record) {
  record.clear();
  if(req.size()<4||req[0]!=2||req.size()>Limit)return false;
  PreparedDraw prepared{};VertexSemanticMask available=0;
  if(!geometry_input(req,prepared,&available)||prepared.vertices.empty()||prepared.indices.empty()||
     prepared.vertices.size()>65535||prepared.indices.size()>65535u*6u)return false;
  const auto present=[&](VertexSemantic value){return (available&vertex_semantic_bit(value))!=0;};
  if(!present(VertexSemantic::Position))return false;
  unsigned texCount=0;
  for(;texCount<3&&present(static_cast<VertexSemantic>(unsigned(VertexSemantic::Tex0)+texCount));++texCount) {}
  const unsigned possibleColor=present(VertexSemantic::Color0)?1u:0u;
  const unsigned possibleNormal=present(VertexSemantic::Normal)?1u:0u;
  const unsigned possiblePn=present(VertexSemantic::PnMatrixIndex)?1u:0u;
  std::vector<uint8_t> payload;
  for(uint32_t v:{uint32_t(prepared.vertices.size()),uint32_t(prepared.indices.size()),0u})word(payload,v);
  for(uint16_t index:prepared.indices){payload.push_back(uint8_t(index));payload.push_back(uint8_t(index>>8));}
  unsigned variants=0;
  for(unsigned color=0;color<=possibleColor;++color)
    for(unsigned normal=0;normal<=possibleNormal;++normal)
      for(unsigned pn=0;pn<=possiblePn;++pn)
        for(unsigned textures=0;textures<=texCount;++textures) {
          const VertexLayout layout=gpu_profile(textures,color!=0,normal!=0,pn!=0);
          const size_t stride=layout.attributes[0].stride;
          const size_t bytes=prepared.vertices.size()*stride;
          const size_t header=3*sizeof(uint32_t)+size_t(layout.count)*6*sizeof(uint32_t);
          if(bytes>Limit || payload.size()>Limit-header || bytes>Limit-header-payload.size())continue;
          layout_words(payload,layout);word(payload,static_cast<uint32_t>(bytes));
          const size_t start=payload.size();payload.resize(start+bytes,0);
          for(size_t i=0;i<prepared.vertices.size();++i)
            pack_gpu_vertex_bytes(payload.data()+start+i*stride,prepared.vertices[i],layout);
          ++variants;
        }
  if(!variants||req.size()+payload.size()+28u>32u*1024u*1024u)return false;
  for(unsigned i=0;i<4;++i)payload[8+i]=uint8_t(variants>>(8*i));
  const uint64_t checksum=XXH64(payload.data(),payload.size(),0);
  for(uint32_t v:{Magic,VersionGpu,KindGpu,uint32_t(req.size()),uint32_t(payload.size()),
                  uint32_t(checksum),uint32_t(checksum>>32)})word(record,v);
  span(record,req.data(),req.size());span(record,payload.data(),payload.size());return true;
}
bool load_native_gpu_geometry(const uint8_t* raw,size_t bytes,uint32_t count,
                              SourcePrimitive primitive,const VertexDecodeLayout& source,
                              const VertexLayout& gpu,NativeGpuGeometry& out) noexcept {
  out={};
  if(!sReader||!raw||!count||!gpu.count||primitive>SourcePrimitive::TriangleFan)return false;
  ++sStats.gpuGeometryAttempts;
  const auto request=native_geometry_request(raw,bytes,count,primitive,source);
  if(request.empty())return false;
  const auto payload=record_payload(request,KindGpu);
  if(payload.empty())return false;
  Input in{payload};
  const size_t vertices=in.word(),indices=in.word(),variants=in.word();
  if(!vertices||vertices>count||!indices||indices>65535u*6u||
     !variants||variants>64||vertices>65535)return false;
  const uint8_t* indexBytes=in.span(indices*2u);
  if(!in.ok)return false;
  // Visit all descriptors before publishing anything: malformed trailing
  // variants invalidate the entire record, even when the first one matched.
  const uint8_t* selected=nullptr;
  size_t selectedBytes=0;
  for(size_t i=0;i<variants;++i) {
    VertexLayout layout{};
    if(!read_layout(in,layout))return false;
    const size_t packedBytes=in.word();
    if(layout.attributes[0].stride>0&&vertices<=Limit/layout.attributes[0].stride&&
       packedBytes==vertices*layout.attributes[0].stride&&
       packedBytes<=Limit) {
      const auto* packed=in.span(packedBytes);
      if(!in.ok)return false;
      if(same_gpu_layout(gpu,layout)){selected=packed;selectedBytes=packedBytes;}
    } else return false;
  }
  if(!in.ok||in.pos!=payload.size()||!selected)return false;
  out.vertexCount=static_cast<uint32_t>(vertices);
  out.vertices.assign(selected,selected+selectedBytes);
  out.indices.resize(indices);
  for(size_t i=0;i<indices;++i) {
    const uint16_t value=uint16_t(indexBytes[i*2])|(uint16_t(indexBytes[i*2+1])<<8);
    if(value>=vertices){out={};return false;}
    out.indices[i]=value;
  }
  ++sStats.gpuGeometryHits;
  return true;
}
bool load_native_texture(const TextureDesc& desc,gxm::CompactTextureData& out,bool allowBc1) noexcept {
  if(!sReader||!desc.immutableSource)return false;
  auto payload=record_payload(native_texture_request(desc),1);if(payload.empty())return false;
  Input in{payload};const auto fmt=in.word(),stride=in.word(),mips=in.word(),swizzled=in.word();
  if(fmt<unsigned(NativeTextureFormat::Intensity8)||fmt>unsigned(NativeTextureFormat::Rgba8)||
      !mips||mips>13||mips!=std::max<unsigned>(desc.mipCount,1)||swizzled>1||
      (fmt==unsigned(NativeTextureFormat::Bc1)&&!allowBc1)||
      stride!=(swizzled?desc.width:fmt==unsigned(NativeTextureFormat::Bc1)?desc.width:(desc.width+7u)&~7u))return false;
  size_t expected=0;
  for(unsigned level=0;level<mips;++level) {
    const uint32_t w=std::max(1u,desc.width>>level),h=std::max(1u,desc.height>>level);
    const unsigned bpp=fmt==unsigned(NativeTextureFormat::Intensity8)?1:fmt==unsigned(NativeTextureFormat::Rgba8)?4:2;
    expected+=fmt==unsigned(NativeTextureFormat::Bc1)?dxt1_texture_size(w,h):size_t(swizzled?w:(w+7u)&~7u)*h*bpp;
  }
  if(!in.ok||payload.size()-in.pos!=expected)return false;
  out={};out.format=NativeTextureFormat(fmt);out.stride=stride;out.mipCount=mips;out.swizzled=swizzled;
  out.pixels.assign(payload.data()+in.pos,payload.data()+payload.size());++sStats.textureHits;return true;
}
bool load_native_geometry(const uint8_t* raw,size_t bytes,uint32_t count,SourcePrimitive primitive,
                          const VertexDecodeLayout& layout,PreparedDraw& out) noexcept {
  if(!sReader||primitive>SourcePrimitive::TriangleFan)return false;
  auto payload=record_payload(native_geometry_request(raw,bytes,count,primitive,layout),2);if(payload.empty())return false;
  Input in{payload};const size_t vertices=in.word(),indices=in.word();
  if(!vertices||vertices>count||indices>65535*6u||payload.size()!=8+vertices*168+indices*2)return false;
  out={};out.vertices.resize(vertices);out.indices.resize(indices);
  const auto* v=in.span(vertices*168);if(!v)return false;
  std::memcpy(out.vertices.data(),v,vertices*168);
  for(size_t i=0;i<indices;++i){const auto* p=in.span(2);if(!p)return false;out.indices[i]=uint16_t(p[0])|(uint16_t(p[1])<<8);if(out.indices[i]>=vertices)return false;}
  out.primitive=Primitive::Triangles;out.error=PrepareDrawError::None;++sStats.geometryHits;return true;
}
} // namespace aurora::vita::gfx
